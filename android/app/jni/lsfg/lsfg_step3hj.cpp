/* Step 3h-j: LIGHT FG GPU chain (Adreno-safe)
 * Real compute: mipmaps(255) → generate(256), not hist-copy.
 * Motion/detail start as cleared dummies (flow chain later);
 * generate still runs the real SPIR-V path with hist0/hist1.
 * Config: fg_live=1, fg_inject=1, fg_throttle=2..30
 */
#include "lsfg_log.h"

#include <stdio.h>
#include <string.h>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <atomic>
#include <stdint.h>

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

struct PassDef { const char *name; uint32_t id; };
static const PassDef k_light[] = {
    { "p_mipmaps",  255 },
    { "p_generate", 256 },
};
enum {
    N_LIGHT = 2,
    MAX_BINDS = 24,
    CMD_SLOTS = 2,
    MIP_LEVELS = 7,
};

struct BindInfo {
    uint32_t binding;
    VkDescriptorType type;
};

struct PassRt {
    uint32_t id;
    VkShaderModule sm;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout pl;
    VkPipeline pipe;
    VkDescriptorSet ds;
    BindInfo binds[MAX_BINDS];
    int nbinds;
    int ok;
};

/* Matches WinNative / lsfg-vk LsfgConstants (48 bytes). */
struct LsfgConstants {
    uint32_t input_offset[2];
    uint32_t first_iter;
    uint32_t first_iter_s;
    uint32_t advanced_color_kind;
    uint32_t hdr_support;
    float resolution_inv_scale;
    float timestamp;
    float ui_threshold;
    uint32_t padding[3];
};
static_assert(sizeof(LsfgConstants) == 48, "LsfgConstants size");

static VkDevice g_dev = VK_NULL_HANDLE;
static VkPhysicalDevice g_phys = VK_NULL_HANDLE;
static uint32_t g_qfam;
static VkCommandPool g_cp = VK_NULL_HANDLE;
static VkCommandBuffer g_cmd[CMD_SLOTS] = {};
static VkFence g_fence[CMD_SLOTS] = {};
static int g_slot;
static VkDescriptorPool g_dpool = VK_NULL_HANDLE;
static VkSampler g_sampler = VK_NULL_HANDLE;
static VkBuffer g_ubo = VK_NULL_HANDLE;
static VkDeviceMemory g_ubo_mem = VK_NULL_HANDLE;

static PassRt g_pass[N_LIGHT];
static int g_npass = 0;

static int g_ready;
static VkImage g_gen = VK_NULL_HANDLE;
static VkDeviceMemory g_gen_mem = VK_NULL_HANDLE;
static VkImageView g_gen_view = VK_NULL_HANDLE;

static VkImage g_mip[MIP_LEVELS] = {};
static VkDeviceMemory g_mip_mem[MIP_LEVELS] = {};
static VkImageView g_mip_view[MIP_LEVELS] = {};
static uint32_t g_mip_w[MIP_LEVELS] = {}, g_mip_h[MIP_LEVELS] = {};
static VkImage g_motion = VK_NULL_HANDLE, g_detail1 = VK_NULL_HANDLE, g_detail2 = VK_NULL_HANDLE;
static VkDeviceMemory g_motion_mem = VK_NULL_HANDLE, g_detail1_mem = VK_NULL_HANDLE, g_detail2_mem = VK_NULL_HANDLE;
static VkImageView g_motion_view = VK_NULL_HANDLE, g_detail1_view = VK_NULL_HANDLE, g_detail2_view = VK_NULL_HANDLE;

static int g_imgs_ok;
static int g_build_fail;
static int g_fg_live;
static int g_fg_inject = 1;
static unsigned g_throttle = 2;
static unsigned g_frame_n;
static unsigned g_submit_n;
static unsigned g_skip_n;
static unsigned g_inject_n;
static int g_gen_ready;
static int g_inflight = -1;
static int g_logged;
static float g_timestamp;

static uint32_t find_mem(VkPhysicalDevice phys, uint32_t bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if (bits & (1u << i)) return i;
    return 0;
}

static bool load_spirv(VkDevice dev, uint32_t id, VkShaderModule *out) {
    const char *cache = lsfg_host_cache_dir();
    if (!cache || !*cache) return false;
    char path[512];
    snprintf(path, sizeof path, "%s/%u.spv", cache, id);
    FILE *f = fopen(path, "rb");
    if (!f) {
        lsfg_log("W", "step3h-j: missing SPIR-V %s", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 4 || (sz % 4) != 0) { fclose(f); return false; }
    std::vector<uint32_t> code((size_t)sz / 4);
    if (fread(code.data(), 1, (size_t)sz, f) != (size_t)sz) { fclose(f); return false; }
    fclose(f);
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = (size_t)sz;
    ci.pCode = code.data();
    if (vkCreateShaderModule(dev, &ci, nullptr, out) != VK_SUCCESS) return false;
    return true;
}

static bool create_img(VkDevice dev, VkPhysicalDevice phys, uint32_t w, uint32_t h,
                       VkFormat fmt, VkImage *img, VkDeviceMemory *mem, VkImageView *view) {
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
    vkBindImageMemory(dev, *img, *mem, 0);
    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = *img; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = fmt;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1; vci.subresourceRange.layerCount = 1;
    return vkCreateImageView(dev, &vci, nullptr, view) == VK_SUCCESS;
}

static void barrier_img(VkCommandBuffer cmd, VkImage img) {
    if (!img) return;
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1; b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &b);
}

static void barrier_undef_to_general(VkCommandBuffer cmd, VkImage img) {
    if (!img) return;
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1; b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &b);
}

static bool ensure_imgs(VkDevice dev, VkPhysicalDevice phys, uint32_t w, uint32_t h) {
    if (g_imgs_ok && g_gen) return true;
    if (!create_img(dev, phys, w, h, VK_FORMAT_R8G8B8A8_UNORM, &g_gen, &g_gen_mem, &g_gen_view))
        return false;
    uint32_t mw = w, mh = h;
    for (int i = 0; i < MIP_LEVELS; i++) {
        if (mw < 1) mw = 1; if (mh < 1) mh = 1;
        g_mip_w[i] = mw; g_mip_h[i] = mh;
        if (!create_img(dev, phys, mw, mh, VK_FORMAT_R8G8B8A8_UNORM,
                        &g_mip[i], &g_mip_mem[i], &g_mip_view[i]))
            return false;
        mw = (mw + 1) / 2; mh = (mh + 1) / 2;
    }
    if (!create_img(dev, phys, w, h, VK_FORMAT_R16G16_SFLOAT, &g_motion, &g_motion_mem, &g_motion_view))
        return false;
    if (!create_img(dev, phys, w, h, VK_FORMAT_R8G8B8A8_UNORM, &g_detail1, &g_detail1_mem, &g_detail1_view))
        return false;
    if (!create_img(dev, phys, w, h, VK_FORMAT_R8G8B8A8_UNORM, &g_detail2, &g_detail2_mem, &g_detail2_view))
        return false;
    g_imgs_ok = 1;
    lsfg_log("I", "step3h-j: intermediates OK %ux%u gen+mip7+motion+detail", w, h);
    return true;
}

static bool reflect_binds(const uint32_t *code, size_t nwords, PassRt *p) {
    /* Minimal SPIR-V scan for OpDecorate Binding + OpVariable descriptor types. */
    p->nbinds = 0;
    std::unordered_map<uint32_t, uint32_t> binding_of;
    std::unordered_map<uint32_t, uint32_t> dtype_of;
    for (size_t i = 5; i + 1 < nwords; ) {
        uint32_t word = code[i];
        uint32_t opc = word & 0xffff;
        uint32_t cnt = word >> 16;
        if (cnt == 0) break;
        if (i + cnt > nwords) break;
        if (opc == 71 /* OpDecorate */ && cnt >= 4) {
            uint32_t target = code[i+1];
            uint32_t dec = code[i+2];
            if (dec == 33 /* Binding */ && cnt >= 4)
                binding_of[target] = code[i+3];
        } else if (opc == 59 /* OpVariable */ && cnt >= 4) {
            uint32_t result_type = code[i+1];
            uint32_t result_id = code[i+2];
            uint32_t storage = code[i+3];
            if (storage == 0 /* UniformConstant */ || storage == 2 /* Uniform */ || storage == 12 /* StorageBuffer */) {
                /* defer type resolution */
                dtype_of[result_id] = storage;
            }
        }
        i += cnt;
    }
    /* Map by binding number sorted */
    std::vector<std::pair<uint32_t, uint32_t>> items;
    for (auto &kv : binding_of) {
        uint32_t id = kv.first;
        uint32_t b = kv.second;
        VkDescriptorType t = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        auto it = dtype_of.find(id);
        if (it != dtype_of.end()) {
            if (it->second == 2) t = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            else if (it->second == 12) t = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        }
        /* Heuristic by binding ranges used in WinNative LSFG */
        if (b == 0) t = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        else if (b == 1) t = VK_DESCRIPTOR_TYPE_SAMPLER;
        else if (b >= 2 && b <= 9) t = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        else t = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        items.push_back({b, (uint32_t)t});
    }
    std::sort(items.begin(), items.end());
    for (auto &it : items) {
        if (p->nbinds >= MAX_BINDS) break;
        p->binds[p->nbinds].binding = it.first;
        p->binds[p->nbinds].type = (VkDescriptorType)it.second;
        p->nbinds++;
    }
    return p->nbinds > 0;
}

static bool build_pass(VkDevice dev, PassRt *p, uint32_t id) {
    p->id = id;
    if (!load_spirv(dev, id, &p->sm)) return false;
    /* re-read for reflect */
    const char *cache = lsfg_host_cache_dir();
    char path[512];
    snprintf(path, sizeof path, "%s/%u.spv", cache, id);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<uint32_t> code((size_t)sz / 4);
    fread(code.data(), 1, (size_t)sz, f); fclose(f);
    if (!reflect_binds(code.data(), code.size(), p)) {
        /* fallback default layout */
        p->nbinds = 0;
        auto add = [&](uint32_t b, VkDescriptorType t) {
            if (p->nbinds < MAX_BINDS) { p->binds[p->nbinds] = {b, t}; p->nbinds++; }
        };
        if (id == 255) {
            add(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
            add(1, VK_DESCRIPTOR_TYPE_SAMPLER);
            add(2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
            for (int i = 0; i < 7; i++) add(10 + i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        } else {
            add(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
            add(1, VK_DESCRIPTOR_TYPE_SAMPLER);
            for (int i = 0; i < 8; i++) add(2 + i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
            add(10, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        }
    }
    std::vector<VkDescriptorSetLayoutBinding> lbs;
    for (int i = 0; i < p->nbinds; i++) {
        VkDescriptorSetLayoutBinding lb{};
        lb.binding = p->binds[i].binding;
        lb.descriptorType = p->binds[i].type;
        lb.descriptorCount = 1;
        lb.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        lbs.push_back(lb);
    }
    VkDescriptorSetLayoutCreateInfo dlci{};
    dlci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dlci.bindingCount = (uint32_t)lbs.size();
    dlci.pBindings = lbs.data();
    if (vkCreateDescriptorSetLayout(dev, &dlci, nullptr, &p->dsl) != VK_SUCCESS) return false;
    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &p->dsl;
    if (vkCreatePipelineLayout(dev, &plci, nullptr, &p->pl) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = p->sm;
    cpci.stage.pName = "main";
    cpci.layout = p->pl;
    if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &p->pipe) != VK_SUCCESS)
        return false;
    p->ok = 1;
    lsfg_log("I", "step3h-j: pass %u built binds=%d", id, p->nbinds);
    return true;
}

static void update_ds(VkDevice dev, PassRt *p, VkImageView h0, VkImageView h1) {
    if (!p->ok || !p->ds) return;
    VkWriteDescriptorSet writes[MAX_BINDS];
    VkDescriptorBufferInfo bufs[4];
    VkDescriptorImageInfo imgs[MAX_BINDS];
    int nw = 0, nb = 0, ni = 0;
    int so = 0, si = 0;
    VkImageView gen_sampled[8] = {
        h0, h1, g_mip_view[0], g_mip_view[1], g_mip_view[2],
        g_motion_view, g_detail1_view, g_detail2_view
    };
    VkImageView mip_storage[MIP_LEVELS];
    for (int i = 0; i < MIP_LEVELS; i++) mip_storage[i] = g_mip_view[i];

    for (int i = 0; i < p->nbinds; i++) {
        BindInfo &b = p->binds[i];
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = p->ds; w.dstBinding = b.binding; w.descriptorCount = 1;
        w.descriptorType = b.type;
        if (b.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
            VkDescriptorBufferInfo bb{};
            bb.buffer = g_ubo; bb.range = sizeof(LsfgConstants);
            bufs[nb] = bb; w.pBufferInfo = &bufs[nb]; nb++;
        } else if (b.type == VK_DESCRIPTOR_TYPE_SAMPLER) {
            VkDescriptorImageInfo ii{};
            ii.sampler = g_sampler; ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            imgs[ni] = ii; w.pImageInfo = &imgs[ni]; ni++;
        } else if (b.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
            VkImageView v = VK_NULL_HANDLE;
            if (p->id == 256) {
                v = g_gen_view;
            } else {
                if (so < MIP_LEVELS) v = mip_storage[so];
                else v = g_gen_view;
                so++;
            }
            VkDescriptorImageInfo ii{};
            ii.imageView = v ? v : g_gen_view;
            ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            imgs[ni] = ii; w.pImageInfo = &imgs[ni]; ni++;
        } else {
            VkImageView v;
            if (p->id == 255) {
                v = h1 ? h1 : h0;
            } else {
                v = gen_sampled[si < 8 ? si : 0];
                si++;
            }
            VkDescriptorImageInfo ii{};
            ii.imageView = v ? v : g_gen_view;
            ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            imgs[ni] = ii; w.pImageInfo = &imgs[ni]; ni++;
        }
        writes[nw++] = w;
    }
    if (nw > 0)
        vkUpdateDescriptorSets(dev, (uint32_t)nw, writes, 0, nullptr);
}

static bool finish_pool(VkDevice dev, uint32_t qfam) {
    lsfg_log("I", "step3h-j: finish_pool begin passes=%d qfam=%u", g_npass, qfam);
    if (g_npass < 1 || g_npass > N_LIGHT) {
        lsfg_log("W", "step3h-j: bad pass count %d — abort FG", g_npass);
        return false;
    }
    VkDescriptorPoolSize sizes[4] = {};
    sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; sizes[0].descriptorCount = 64;
    sizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; sizes[1].descriptorCount = 64;
    sizes[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; sizes[2].descriptorCount = 16;
    sizes[3].type = VK_DESCRIPTOR_TYPE_SAMPLER; sizes[3].descriptorCount = 8;
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 8;
    dpci.poolSizeCount = 4;
    dpci.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(dev, &dpci, nullptr, &g_dpool) != VK_SUCCESS) return false;

    for (int i = 0; i < g_npass; i++) {
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = g_dpool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &g_pass[i].dsl;
        if (vkAllocateDescriptorSets(dev, &ai, &g_pass[i].ds) != VK_SUCCESS) return false;
    }

    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(dev, &sci, nullptr, &g_sampler) != VK_SUCCESS) return false;

    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = sizeof(LsfgConstants);
    bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (vkCreateBuffer(dev, &bci, nullptr, &g_ubo) != VK_SUCCESS) return false;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, g_ubo, &mr);
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = find_mem(g_phys, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(dev, &mai, nullptr, &g_ubo_mem) != VK_SUCCESS) return false;
    vkBindBufferMemory(dev, g_ubo, g_ubo_mem, 0);

    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = qfam;
    if (vkCreateCommandPool(dev, &pci, nullptr, &g_cp) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = g_cp;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = CMD_SLOTS;
    if (vkAllocateCommandBuffers(dev, &cai, g_cmd) != VK_SUCCESS) return false;
    for (int i = 0; i < CMD_SLOTS; i++) {
        VkFenceCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (vkCreateFence(dev, &fi, nullptr, &g_fence[i]) != VK_SUCCESS) return false;
    }
    lsfg_log("I", "step3h-j: pool/cmd/fence OK");
    return true;
}

static bool build_tick(VkDevice dev, VkPhysicalDevice phys, uint32_t qfam) {
    if (g_ready || g_build_fail) return g_ready;
    g_npass = 0;
    for (int i = 0; i < N_LIGHT; i++) {
        if (!build_pass(dev, &g_pass[g_npass], k_light[i].id))
            lsfg_log("W", "step3h-j: pass %u failed", k_light[i].id);
        else
            g_npass++;
    }
    if (g_npass < 2) {
        g_build_fail = 1;
        lsfg_log("W", "step3h-j: need both mipmaps+generate — got %d", g_npass);
        return false;
    }
    if (!finish_pool(dev, qfam)) {
        g_build_fail = 1;
        return false;
    }
    g_ready = 1;
    lsfg_log("I", "step3h-j: READY live=%d inject=%d throttle=%u stages=%d",
             g_fg_live, g_fg_inject, g_throttle, g_npass);
    return true;
}

static void poll_fence(VkDevice dev) {
    if (g_inflight < 0) return;
    if (vkGetFenceStatus(dev, g_fence[g_inflight]) == VK_SUCCESS) {
        g_gen_ready = 1;
        g_inflight = -1;
    }
}

static bool submit_full(VkDevice dev) {
    if (!g_ready || g_inflight >= 0) return false;
    VkQueue q = (VkQueue)lsfg_step3d_get_queue();
    if (!q) return false;
    uint32_t w = lsfg_step3d_fb_width();
    uint32_t h = lsfg_step3d_fb_height();
    if (!w || !h) return false;
    if (!ensure_imgs(dev, g_phys, w, h)) return false;

    VkImageView h0 = (VkImageView)lsfg_step3d_get_view(0);
    VkImageView h1 = (VkImageView)lsfg_step3d_get_view(1);
    if (!h0) return false;

    /* update UBO */
    LsfgConstants c{};
    c.resolution_inv_scale = 1.0f;
    c.timestamp = g_timestamp;
    g_timestamp += 1.0f / 60.0f;
    if (g_timestamp > 1e6f) g_timestamp = 0;
    void *map = nullptr;
    if (vkMapMemory(dev, g_ubo_mem, 0, sizeof(c), 0, &map) == VK_SUCCESS) {
        memcpy(map, &c, sizeof(c));
        vkUnmapMemory(dev, g_ubo_mem);
    }

    for (int i = 0; i < g_npass; i++)
        update_ds(dev, &g_pass[i], h0, h1);

    int s = g_slot;
    vkResetFences(dev, 1, &g_fence[s]);
    vkResetCommandBuffer(g_cmd[s], 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(g_cmd[s], &bi) != VK_SUCCESS) return false;

    barrier_undef_to_general(g_cmd[s], g_gen);
    for (int i = 0; i < MIP_LEVELS; i++) barrier_undef_to_general(g_cmd[s], g_mip[i]);
    barrier_undef_to_general(g_cmd[s], g_motion);
    barrier_undef_to_general(g_cmd[s], g_detail1);
    barrier_undef_to_general(g_cmd[s], g_detail2);

    auto groups = [](uint32_t dim) { return (dim + 7u) / 8u; };

    /* Pass 0: mipmaps */
    {
        PassRt &p = g_pass[0];
        vkCmdBindPipeline(g_cmd[s], VK_PIPELINE_BIND_POINT_COMPUTE, p.pipe);
        vkCmdBindDescriptorSets(g_cmd[s], VK_PIPELINE_BIND_POINT_COMPUTE, p.pl, 0, 1, &p.ds, 0, nullptr);
        vkCmdDispatch(g_cmd[s], groups(w), groups(h), 1);
        for (int i = 0; i < MIP_LEVELS; i++) barrier_img(g_cmd[s], g_mip[i]);
    }
    /* Pass 1: generate */
    {
        PassRt &p = g_pass[1];
        vkCmdBindPipeline(g_cmd[s], VK_PIPELINE_BIND_POINT_COMPUTE, p.pipe);
        vkCmdBindDescriptorSets(g_cmd[s], VK_PIPELINE_BIND_POINT_COMPUTE, p.pl, 0, 1, &p.ds, 0, nullptr);
        vkCmdDispatch(g_cmd[s], groups(w), groups(h), 1);
        barrier_img(g_cmd[s], g_gen);
    }

    vkEndCommandBuffer(g_cmd[s]);

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g_cmd[s];
    if (vkQueueSubmit(q, 1, &si, g_fence[s]) != VK_SUCCESS) return false;

    g_inflight = s;
    g_slot = (s + 1) % CMD_SLOTS;
    g_gen_ready = 0;
    g_submit_n++;
    if (g_submit_n <= 5 || (g_submit_n % 60) == 0)
        lsfg_log("I", "step3h-j: GPU dispatch #%u %ux%u mipmaps→generate", g_submit_n, w, h);
    return true;
}

extern "C" void lsfg_step3hj_set_phys(void *phys) { g_phys = (VkPhysicalDevice)phys; }
extern "C" void lsfg_step3hj_set_live(int live) {
    g_fg_live = live ? 1 : 0;
    if (g_fg_live && g_throttle < 2) g_throttle = 2;
}
extern "C" void lsfg_step3hj_set_inject(int inj) { g_fg_inject = inj ? 1 : 0; }
extern "C" void lsfg_step3hj_set_full(int /*full*/) { }
extern "C" void lsfg_step3hj_set_throttle(int n) {
    if (n < 1) n = 1;
    if (n > 120) n = 120;
    g_throttle = (unsigned)n;
}
extern "C" int lsfg_step3hj_gen_ready(void) { return g_gen_ready && g_fg_inject; }
extern "C" void *lsfg_step3hj_gen_image(void) { return g_gen_ready ? (void*)g_gen : nullptr; }
extern "C" void *lsfg_step3hj_gen_view(void) { return g_gen_ready ? (void*)g_gen_view : nullptr; }

extern "C" void lsfg_step3hj_on_frame(void *device, unsigned qfam) {
    if (!device || !g_phys) return;
    if (!lsfg_step3d_frames_ready()) return;
    g_dev = (VkDevice)device;
    g_qfam = qfam;

    if (!g_ready && !g_build_fail)
        build_tick(g_dev, g_phys, qfam);

    unsigned n = ++g_frame_n;
    if (g_ready)
        poll_fence(g_dev);

    if (!g_fg_live) {
        if (!g_logged && n >= 5) {
            g_logged = 1;
            lsfg_log("I", "step3h-j: idle (fg_live=0)");
        }
        return;
    }
    if (!g_ready) return;
    if (n < 10) return;
    if (g_throttle > 1 && (n % g_throttle) != 0) { g_skip_n++; return; }
    bool ok = submit_full(g_dev);
    if (!ok) g_skip_n++;
    if (n == 10 || n == 30 || n == 60 || n == 120 || n == 300 || (n % 600) == 0)
        lsfg_log("I", "step3h-j: frame #%u submit=%d ok=%u skip=%u inflight=%d gen=%d inject=%d",
                 n, ok ? 1 : 0, g_submit_n, g_skip_n, g_inflight, g_gen_ready, g_fg_inject);
}

extern "C" int lsfg_step3hj_should_inject_impl(void) {
    return (g_fg_inject && g_gen_ready && g_gen) ? 1 : 0;
}
extern "C" void lsfg_step3hj_note_inject(void) {
    g_inject_n++;
    g_gen_ready = 0;
    if (g_inject_n <= 5 || g_inject_n == 60 || (g_inject_n % 300) == 0)
        lsfg_log("I", "step3h-j: INJECT #%u (gen→swapchain GPU)", g_inject_n);
}
extern "C" void lsfg_set_should_inject_hook(int (*fn)(void));

extern "C" void lsfg_step3hj_install(void) {
    lsfg_set_should_inject_hook(lsfg_step3hj_should_inject_impl);
    lsfg_log("I", "LSFG: step3h-j LIGHT GPU chain installer ready (mipmaps+generate)");
}
