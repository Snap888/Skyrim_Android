/* Step 3h-c: per-pass SPIR-V descriptor layouts + dispatch chain (mipmaps -> generate).
 * Not full lsfg-vk wiring — binds history + intermediate images with reflected types.
 */
#include "lsfg_log.h"
#include "lsfg_host.h"

#include <stdio.h>
#include <string.h>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <atomic>

#ifdef __ANDROID__
#include <vulkan/vulkan.h>
#else
#include <vulkan/vulkan_core.h>
#endif

extern "C" int  lsfg_step3d_frames_ready(void);
extern "C" void *lsfg_step3d_get_image(int idx);
extern "C" void *lsfg_step3d_get_view(int idx);
extern "C" void *lsfg_step3d_get_queue(void);
extern "C" unsigned lsfg_step3d_fb_width(void);
extern "C" unsigned lsfg_step3d_fb_height(void);
extern "C" const char *lsfg_host_cache_dir(void);
extern "C" int lsfg_step3hb_graph_ready(void);
extern "C" int lsfg_roles_ready(void);
extern "C" uint32_t lsfg_role_generate_id(void);
extern "C" uint32_t lsfg_role_mipmaps_id(void);
extern "C" int lsfg_role_perf_mode(void);

struct BindInfo {
    uint32_t binding;
    VkDescriptorType type;
};

struct PassRt {
    uint32_t id;
    const char *name;
    VkShaderModule sm;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout pl;
    VkPipeline pipe;
    VkDescriptorSet ds;
    std::vector<BindInfo> binds;
    int ok;
};

static VkDevice g_dev = VK_NULL_HANDLE;
static VkPhysicalDevice g_phys = VK_NULL_HANDLE;
static uint32_t g_qfam;
static VkDescriptorPool g_pool = VK_NULL_HANDLE;
static VkCommandPool g_cp = VK_NULL_HANDLE;
static VkCommandBuffer g_cmd = VK_NULL_HANDLE;
static VkFence g_fence = VK_NULL_HANDLE;
static VkSampler g_sampler = VK_NULL_HANDLE;
static VkBuffer g_ubo = VK_NULL_HANDLE;
static VkDeviceMemory g_ubo_mem = VK_NULL_HANDLE;

static VkImage g_flow = VK_NULL_HANDLE;
static VkDeviceMemory g_flow_mem = VK_NULL_HANDLE;
static VkImageView g_flow_view = VK_NULL_HANDLE;
static VkImage g_out = VK_NULL_HANDLE;
static VkDeviceMemory g_out_mem = VK_NULL_HANDLE;
static VkImageView g_out_view = VK_NULL_HANDLE;

static PassRt g_mip{}, g_gen{};
static int g_ready;
static std::atomic<unsigned> g_disp_n{0};

static bool load_spv(const char *path, std::vector<uint32_t> &out) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 20 || (sz % 4) != 0) { fclose(f); return false; }
    out.resize((size_t)sz / 4);
    if (fread(out.data(), 1, (size_t)sz, f) != (size_t)sz) { fclose(f); out.clear(); return false; }
    fclose(f);
    return out[0] == 0x07230203u;
}

static void reflect(const std::vector<uint32_t> &c, std::vector<BindInfo> &out) {
    out.clear();
    std::unordered_map<uint32_t, uint32_t> id_bind;
    std::unordered_map<uint32_t, uint32_t> id_type;
    std::unordered_map<uint32_t, uint32_t> type_pointee;
    std::unordered_map<uint32_t, int> type_kind;

    for (size_t i = 5; i < c.size();) {
        uint32_t w = c[i], op = w & 0xffff, len = w >> 16;
        if (!len) break;
        if (op == 71 && len >= 4) {
            uint32_t tgt = c[i+1], dec = c[i+2], lit = c[i+3];
            if (dec == 33) id_bind[tgt] = lit;
        } else if (op == 32 && len >= 4) {
            type_pointee[c[i+1]] = c[i+3];
        } else if (op == 30 && len >= 2) {
            type_kind[c[i+1]] = 1;
        } else if (op == 25 && len >= 3) {
            uint32_t tid = c[i+1];
            uint32_t sampled = (len > 6) ? c[i+6] : 0;
            type_kind[tid] = (sampled == 2) ? 3 : 2;
        } else if (op == 27 && len >= 3) {
            type_kind[c[i+1]] = 2;
        } else if (op == 26 && len >= 2) {
            type_kind[c[i+1]] = 4;
        } else if (op == 59 && len >= 4) {
            id_type[c[i+2]] = c[i+1];
        }
        i += len;
    }

    std::vector<uint32_t> binds;
    for (auto &kv : id_bind) binds.push_back(kv.second);
    std::sort(binds.begin(), binds.end());
    binds.erase(std::unique(binds.begin(), binds.end()), binds.end());

    for (uint32_t b : binds) {
        BindInfo bi{};
        bi.binding = b;
        bi.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        for (auto &kv : id_bind) {
            if (kv.second != b) continue;
            auto it = id_type.find(kv.first);
            if (it == id_type.end()) continue;
            uint32_t base = it->second;
            auto pt = type_pointee.find(base);
            if (pt != type_pointee.end()) base = pt->second;
            auto k = type_kind.find(base);
            if (k != type_kind.end()) {
                if (k->second == 1) bi.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                else if (k->second == 2) bi.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                else if (k->second == 3) bi.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                else if (k->second == 4) bi.type = VK_DESCRIPTOR_TYPE_SAMPLER;
            }
            break;
        }
        out.push_back(bi);
    }
}

static uint32_t find_mem(VkPhysicalDevice phys, uint32_t bits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props)
            return i;
    return 0;
}

static bool create_img(VkDevice dev, VkPhysicalDevice phys, uint32_t w, uint32_t h, VkFormat fmt,
                       VkImage *img, VkDeviceMemory *mem, VkImageView *view) {
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = fmt;
    ici.extent = { w, h, 1 };
    ici.mipLevels = 1; ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
              | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev, &ici, nullptr, img) != VK_SUCCESS) return false;
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, *img, &mr);
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = find_mem(phys, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(dev, &mai, nullptr, mem) != VK_SUCCESS) return false;
    if (vkBindImageMemory(dev, *img, *mem, 0) != VK_SUCCESS) return false;
    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = *img; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = fmt;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1; vci.subresourceRange.layerCount = 1;
    return vkCreateImageView(dev, &vci, nullptr, view) == VK_SUCCESS;
}

static bool build_pass(VkDevice dev, const char *cache, uint32_t id, const char *name, PassRt *p) {
    memset(p, 0, sizeof(*p));
    p->id = id; p->name = name;
    char path[700];
    snprintf(path, sizeof path, "%s/%u.spv", cache, id);
    std::vector<uint32_t> code;
    if (!load_spv(path, code)) {
        lsfg_log("W", "step3h-c: missing %s id=%u", name, id);
        return false;
    }
    reflect(code, p->binds);

    VkShaderModuleCreateInfo smci{};
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = code.size() * 4;
    smci.pCode = code.data();
    if (vkCreateShaderModule(dev, &smci, nullptr, &p->sm) != VK_SUCCESS) return false;

    std::vector<VkDescriptorSetLayoutBinding> lbs;
    for (auto &b : p->binds) {
        VkDescriptorSetLayoutBinding lb{};
        lb.binding = b.binding;
        lb.descriptorType = b.type;
        lb.descriptorCount = 1;
        lb.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        lbs.push_back(lb);
    }
    VkDescriptorSetLayoutCreateInfo dlci{};
    dlci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dlci.bindingCount = (uint32_t)lbs.size();
    dlci.pBindings = lbs.empty() ? nullptr : lbs.data();
    if (vkCreateDescriptorSetLayout(dev, &dlci, nullptr, &p->dsl) != VK_SUCCESS) return false;

    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &p->dsl;
    if (vkCreatePipelineLayout(dev, &plci, nullptr, &p->pl) != VK_SUCCESS) return false;

    VkComputePipelineCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pci.stage.module = p->sm;
    pci.stage.pName = "main";
    pci.layout = p->pl;
    if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pci, nullptr, &p->pipe) != VK_SUCCESS) {
        lsfg_log("W", "step3h-c: pipeline fail %s id=%u binds=%zu", name, id, p->binds.size());
        return false;
    }
    p->ok = 1;
    return true;
}

static void update_ds(VkDevice dev, PassRt *p,
                      VkImageView hist0, VkImageView hist1,
                      VkImageView flow, VkImageView outv) {
    if (!p->ok || !p->ds) return;
    std::vector<VkWriteDescriptorSet> writes;
    std::vector<VkDescriptorImageInfo> imgs;
    std::vector<VkDescriptorBufferInfo> bufs;
    imgs.reserve(p->binds.size() + 4);
    bufs.reserve(4);
    writes.reserve(p->binds.size());

    int img_i = 0;
    for (auto &b : p->binds) {
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = p->ds;
        w.dstBinding = b.binding;
        w.descriptorCount = 1;
        w.descriptorType = b.type;

        if (b.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
            VkDescriptorBufferInfo bi{};
            bi.buffer = g_ubo; bi.offset = 0; bi.range = 256;
            bufs.push_back(bi);
            w.pBufferInfo = &bufs.back();
        } else if (b.type == VK_DESCRIPTOR_TYPE_SAMPLER) {
            VkDescriptorImageInfo ii{};
            ii.sampler = g_sampler;
            ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            imgs.push_back(ii);
            w.pImageInfo = &imgs.back();
        } else {
            VkImageView views[4] = { hist0, hist1, flow, outv };
            VkImageView v = views[img_i % 4];
            img_i++;
            if (!v) v = hist0;
            VkDescriptorImageInfo ii{};
            ii.imageView = v;
            ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            imgs.push_back(ii);
            w.pImageInfo = &imgs.back();
        }
        writes.push_back(w);
    }
    if (!writes.empty())
        vkUpdateDescriptorSets(dev, (uint32_t)writes.size(), writes.data(), 0, nullptr);
}

static bool ensure_resources(VkDevice dev, VkPhysicalDevice phys, uint32_t qfam) {
    if (g_cp) return true;
    if (!phys) return false;

    uint32_t w = lsfg_step3d_fb_width();
    uint32_t h = lsfg_step3d_fb_height();
    if (w < 16) w = 16;
    if (h < 16) h = 16;
    uint32_t fw = (w * 70) / 100; if (fw < 8) fw = 8;
    uint32_t fh = (h * 70) / 100; if (fh < 8) fh = 8;

    if (!create_img(dev, phys, fw, fh, VK_FORMAT_R8G8B8A8_UNORM, &g_flow, &g_flow_mem, &g_flow_view))
        return false;
    if (!create_img(dev, phys, w, h, VK_FORMAT_R8G8B8A8_UNORM, &g_out, &g_out_mem, &g_out_view))
        return false;

    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = 256;
    bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (vkCreateBuffer(dev, &bci, nullptr, &g_ubo) != VK_SUCCESS) return false;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, g_ubo, &mr);
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = find_mem(phys, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(dev, &mai, nullptr, &g_ubo_mem) != VK_SUCCESS) return false;
    vkBindBufferMemory(dev, g_ubo, g_ubo_mem, 0);
    void *map = nullptr;
    if (vkMapMemory(dev, g_ubo_mem, 0, 256, 0, &map) == VK_SUCCESS) {
        memset(map, 0, 256);
        float *f = (float*)map;
        f[0] = 0.5f;
        f[1] = (float)w; f[2] = (float)h;
        vkUnmapMemory(dev, g_ubo_mem);
    }

    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(dev, &sci, nullptr, &g_sampler) != VK_SUCCESS) return false;

    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = qfam;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (vkCreateCommandPool(dev, &pci, nullptr, &g_cp) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = g_cp;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(dev, &cai, &g_cmd) != VK_SUCCESS) return false;

    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkCreateFence(dev, &fci, nullptr, &g_fence) != VK_SUCCESS) return false;
    return true;
}

static bool build(VkDevice dev, VkPhysicalDevice phys, uint32_t qfam) {
    if (g_ready) return true;
    if (!lsfg_roles_ready()) return false;
    const char *cache = lsfg_host_cache_dir();
    if (!cache || !*cache) return false;
    if (!ensure_resources(dev, phys, qfam)) {
        lsfg_log("W", "step3h-c: resource create failed");
        return false;
    }

    lsfg_log("I", "=== LSFG step 3h-c: descriptor layouts + dispatch chain ===");

    uint32_t mid = lsfg_role_mipmaps_id();
    uint32_t gid = lsfg_role_generate_id();
    if (!build_pass(dev, cache, mid, "mipmaps", &g_mip)) {
        lsfg_log("W", "step3h-c: mipmaps pass build failed");
        return false;
    }
    if (!build_pass(dev, cache, gid, "generate", &g_gen)) {
        lsfg_log("W", "step3h-c: generate pass build failed");
        return false;
    }

    VkDescriptorPoolSize sizes[4] = {};
    sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; sizes[0].descriptorCount = 32;
    sizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; sizes[1].descriptorCount = 32;
    sizes[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; sizes[2].descriptorCount = 8;
    sizes[3].type = VK_DESCRIPTOR_TYPE_SAMPLER; sizes[3].descriptorCount = 8;
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 4;
    dpci.poolSizeCount = 4;
    dpci.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(dev, &dpci, nullptr, &g_pool) != VK_SUCCESS) return false;

    VkDescriptorSetLayout layouts[2] = { g_mip.dsl, g_gen.dsl };
    VkDescriptorSet sets[2];
    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = g_pool;
    dsai.descriptorSetCount = 2;
    dsai.pSetLayouts = layouts;
    if (vkAllocateDescriptorSets(dev, &dsai, sets) != VK_SUCCESS) {
        lsfg_log("W", "step3h-c: AllocateDescriptorSets fail");
        return false;
    }
    g_mip.ds = sets[0];
    g_gen.ds = sets[1];

    lsfg_log("I", "step3h-c: mipmaps id=%u binds=%zu pipe=OK", g_mip.id, g_mip.binds.size());
    for (size_t i = 0; i < g_mip.binds.size() && i < 8; i++)
        lsfg_log("I", "step3h-c:   mip b%u type=%u", g_mip.binds[i].binding, (unsigned)g_mip.binds[i].type);
    lsfg_log("I", "step3h-c: generate id=%u binds=%zu pipe=OK", g_gen.id, g_gen.binds.size());
    for (size_t i = 0; i < g_gen.binds.size() && i < 8; i++)
        lsfg_log("I", "step3h-c:   gen b%u type=%u", g_gen.binds[i].binding, (unsigned)g_gen.binds[i].type);

    g_ready = 1;
    lsfg_log("I", "step3h-c: CHAIN READY (mipmaps + generate descriptors)");
    lsfg_log("I", "NEXT step 3h-d: present generated frame into swapchain path");
    return true;
}

static void barrier_img(VkCommandBuffer cmd, VkImage img) {
    if (!img) return;
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &b);
}

static bool dispatch_chain(VkDevice dev) {
    if (!g_ready || !g_cmd) return false;
    VkImageView h0 = (VkImageView)lsfg_step3d_get_view(0);
    VkImageView h1 = (VkImageView)lsfg_step3d_get_view(1);
    if (!h0 || !h1) return false;

    update_ds(dev, &g_mip, h0, h1, g_flow_view, g_out_view);
    update_ds(dev, &g_gen, h0, h1, g_flow_view, g_out_view);

    vkWaitForFences(dev, 1, &g_fence, VK_TRUE, 1000000000ull);
    vkResetFences(dev, 1, &g_fence);
    vkResetCommandBuffer(g_cmd, 0);

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(g_cmd, &bi);

    barrier_img(g_cmd, (VkImage)lsfg_step3d_get_image(0));
    barrier_img(g_cmd, (VkImage)lsfg_step3d_get_image(1));
    barrier_img(g_cmd, g_flow);
    barrier_img(g_cmd, g_out);

    uint32_t w = lsfg_step3d_fb_width();
    uint32_t h = lsfg_step3d_fb_height();
    if (w < 8) w = 8; if (h < 8) h = 8;
    uint32_t fw = (w * 70) / 100; if (fw < 8) fw = 8;
    uint32_t fh = (h * 70) / 100; if (fh < 8) fh = 8;

    vkCmdBindPipeline(g_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_mip.pipe);
    vkCmdBindDescriptorSets(g_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_mip.pl, 0, 1, &g_mip.ds, 0, nullptr);
    vkCmdDispatch(g_cmd, (fw + 7) / 8, (fh + 7) / 8, 1);
    barrier_img(g_cmd, g_flow);

    vkCmdBindPipeline(g_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_gen.pipe);
    vkCmdBindDescriptorSets(g_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_gen.pl, 0, 1, &g_gen.ds, 0, nullptr);
    vkCmdDispatch(g_cmd, (w + 7) / 8, (h + 7) / 8, 1);
    barrier_img(g_cmd, g_out);

    vkEndCommandBuffer(g_cmd);

    VkQueue q = (VkQueue)lsfg_step3d_get_queue();
    if (!q) return false;
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g_cmd;
    if (vkQueueSubmit(q, 1, &si, g_fence) != VK_SUCCESS) {
        lsfg_log("W", "step3h-c: QueueSubmit fail");
        return false;
    }
    return true;
}

extern "C" void lsfg_step3hc_set_phys(void *phys) {
    g_phys = (VkPhysicalDevice)phys;
}

extern "C" void lsfg_step3hc_after_copy(void *device, unsigned qfam) {
    if (!device) return;
    g_dev = (VkDevice)device;
    g_qfam = qfam;
    if (!lsfg_step3d_frames_ready()) return;
    if (!lsfg_step3d_get_view(0) || !lsfg_step3d_get_view(1)) return;

    if (!g_ready) {
        if (!build(g_dev, g_phys, qfam)) return;
    }

    unsigned n = ++g_disp_n;
    if (n >= 2) {
        if (dispatch_chain(g_dev)) {
            if (n == 2 || n == 60 || n == 300 || (n % 1800) == 0) {
                lsfg_log("I", "step3h-c: DISPATCH CHAIN OK #%u mipmaps->generate %ux%u (out not presented yet)",
                         n, lsfg_step3d_fb_width(), lsfg_step3d_fb_height());
            }
        }
    }
}

extern "C" void lsfg_step3hc_install(void) {
    lsfg_log("I", "LSFG: step3h-c descriptor+dispatch chain installed");
}
