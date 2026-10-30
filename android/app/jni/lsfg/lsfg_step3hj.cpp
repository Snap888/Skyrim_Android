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
static int g_cleared;

static int g_fg_live, g_fg_inject;
static unsigned g_throttle = 30;
static float g_flow_scale = 0.7f;
static std::atomic<unsigned> g_frame_n{0};
static unsigned g_submit_n, g_skip_n, g_inject_n;
static int g_inflight = -1;
static int g_gen_ready;
static int g_logged;
static int g_build_phase;
static int g_pipe_i;
static int g_pipe_ok;
static int g_build_fail;

static bool load_spv(const char *path, std::vector<uint32_t> &out) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 20 || (sz % 4)) { fclose(f); return false; }
    out.resize((size_t)sz / 4);
    bool ok = fread(out.data(), 1, (size_t)sz, f) == (size_t)sz;
    fclose(f);
    return ok && out[0] == 0x07230203u;
}

static void reflect(const std::vector<uint32_t> &c, BindInfo *out, int *nout) {
    *nout = 0;
    std::unordered_map<uint32_t, uint32_t> id_bind, id_type, type_pointee;
    std::unordered_map<uint32_t, int> type_kind;
    for (size_t i = 5; i < c.size();) {
        uint32_t w = c[i], op = w & 0xffff, len = w >> 16;
        if (!len) break;
        if (op == 71 && len >= 4) { if (c[i+2] == 33) id_bind[c[i+1]] = c[i+3]; }
        else if (op == 32 && len >= 4) type_pointee[c[i+1]] = c[i+3];
        else if (op == 30 && len >= 2) type_kind[c[i+1]] = 1;
        else if (op == 25 && len >= 3) {
            uint32_t sampled = (len > 6) ? c[i+6] : 0;
            type_kind[c[i+1]] = (sampled == 2) ? 3 : 2;
        } else if (op == 27 && len >= 3) type_kind[c[i+1]] = 2;
        else if (op == 26 && len >= 2) type_kind[c[i+1]] = 4;
        else if (op == 59 && len >= 4) id_type[c[i+2]] = c[i+1];
        i += len;
    }
    std::vector<uint32_t> binds;
    for (auto &kv : id_bind) binds.push_back(kv.second);
    std::sort(binds.begin(), binds.end());
    binds.erase(std::unique(binds.begin(), binds.end()), binds.end());
    for (uint32_t b : binds) {
        if (*nout >= MAX_BINDS) break;
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
        out[(*nout)++] = bi;
    }
    for (int i = *nout - 1; i >= 0; i--) {
        if (out[i].type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
            out[i].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            break;
        }
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
    if (w < 1) w = 1; if (h < 1) h = 1;
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D; ici.format = fmt; ici.extent = { w, h, 1 };
    ici.mipLevels = 1; ici.arrayLayers = 1; ici.samples = VK_SAMPLE_COUNT_1_BIT;
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
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &b);
}

static bool ensure_imgs(VkDevice dev, VkPhysicalDevice phys) {
    if (g_imgs_ok) return true;
    uint32_t w = lsfg_step3d_fb_width(); uint32_t h = lsfg_step3d_fb_height();
    if (w < 16) w = 16; if (h < 16) h = 16;

    if (!create_img(dev, phys, w, h, VK_FORMAT_R8G8B8A8_UNORM, &g_gen, &g_gen_mem, &g_gen_view)) {
        lsfg_log("W", "step3h-j: gen out alloc fail %ux%u", w, h);
        return false;
    }

    uint32_t fw = (uint32_t)((float)w * g_flow_scale);
    uint32_t fh = (uint32_t)((float)h * g_flow_scale);
    if (fw < 8) fw = 8; if (fh < 8) fh = 8;

    for (int i = 0; i < MIP_LEVELS; i++) {
        g_mip_w[i] = fw >> i; if (g_mip_w[i] < 1) g_mip_w[i] = 1;
        g_mip_h[i] = fh >> i; if (g_mip_h[i] < 1) g_mip_h[i] = 1;
        if (!create_img(dev, phys, g_mip_w[i], g_mip_h[i], VK_FORMAT_R8_UNORM,
                        &g_mip[i], &g_mip_mem[i], &g_mip_view[i])) {
            lsfg_log("W", "step3h-j: mip L%d alloc fail", i);
            return false;
        }
    }

    if (!create_img(dev, phys, fw, fh, VK_FORMAT_R16G16B16A16_SFLOAT,
                    &g_motion, &g_motion_mem, &g_motion_view)) return false;
    if (!create_img(dev, phys, fw, fh, VK_FORMAT_R16G16B16A16_SFLOAT,
                    &g_detail1, &g_detail1_mem, &g_detail1_view)) return false;
    if (!create_img(dev, phys, fw, fh, VK_FORMAT_R16G16B16A16_SFLOAT,
                    &g_detail2, &g_detail2_mem, &g_detail2_view)) return false;

    g_imgs_ok = 1;
    lsfg_log("I", "step3h-j: GPU intermediates gen=%ux%u flow=%ux%u mips=%d",
             w, h, fw, fh, MIP_LEVELS);
    return true;
}

static void upload_ubo(float timestamp) {
    if (!g_ubo_mem) return;
    void *map = nullptr;
    if (vkMapMemory(g_dev, g_ubo_mem, 0, sizeof(LsfgConstants), 0, &map) != VK_SUCCESS)
        return;
    LsfgConstants c{};
    c.first_iter = 1;
    c.first_iter_s = 1;
    c.resolution_inv_scale = (g_flow_scale > 0.01f) ? (1.0f / g_flow_scale) : 1.0f;
    c.timestamp = timestamp;
    c.ui_threshold = 0.5f;
    memcpy(map, &c, sizeof(c));
    vkUnmapMemory(g_dev, g_ubo_mem);
}

static bool ensure_shared(VkDevice dev, VkPhysicalDevice phys) {
    if (g_sampler) return true;
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
    mai.memoryTypeIndex = find_mem(phys, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(dev, &mai, nullptr, &g_ubo_mem) != VK_SUCCESS) return false;
    vkBindBufferMemory(dev, g_ubo, g_ubo_mem, 0);
    upload_ubo(0.5f);
    return true;
}

static bool build_pass(VkDevice dev, const char *cache, const PassDef &def, PassRt *p) {
    memset(p, 0, sizeof(*p));
    p->id = def.id;
    char path[700];
    snprintf(path, sizeof path, "%s/%u.spv", cache, def.id);
    std::vector<uint32_t> code;
    if (!load_spv(path, code)) {
        lsfg_log("W", "step3h-j: load_spv fail %s", path);
        return false;
    }
    reflect(code, p->binds, &p->nbinds);
    lsfg_log("I", "step3h-j: id=%u nbinds=%d", def.id, p->nbinds);
    VkShaderModuleCreateInfo smci{};
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = code.size() * 4; smci.pCode = code.data();
    if (vkCreateShaderModule(dev, &smci, nullptr, &p->sm) != VK_SUCCESS) {
        lsfg_log("W", "step3h-j: CreateShaderModule fail id=%u", def.id);
        return false;
    }
    VkDescriptorSetLayoutBinding lbs[MAX_BINDS];
    int nlbs = 0;
    for (int i = 0; i < p->nbinds; i++) {
        VkDescriptorSetLayoutBinding lb{};
        lb.binding = p->binds[i].binding;
        lb.descriptorType = p->binds[i].type;
        lb.descriptorCount = 1;
        lb.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        lbs[nlbs++] = lb;
    }
    VkDescriptorSetLayoutCreateInfo dlci{};
    dlci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dlci.bindingCount = (uint32_t)nlbs;
    dlci.pBindings = nlbs ? lbs : nullptr;
    if (vkCreateDescriptorSetLayout(dev, &dlci, nullptr, &p->dsl) != VK_SUCCESS) return false;
    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1; plci.pSetLayouts = &p->dsl;
    if (vkCreatePipelineLayout(dev, &plci, nullptr, &p->pl) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pci.stage.module = p->sm; pci.stage.pName = "main";
    pci.layout = p->pl;
    if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pci, nullptr, &p->pipe) != VK_SUCCESS) {
        lsfg_log("W", "step3h-j: CreateComputePipelines fail id=%u", def.id);
        return false;
    }
    p->ok = 1;
    return true;
}

static void update_ds(VkDevice dev, PassRt *p, VkImageView h0, VkImageView h1) {
    if (!p->ok || !p->ds || p->nbinds <= 0) return;
    VkWriteDescriptorSet writes[MAX_BINDS];
    VkDescriptorImageInfo imgs[MAX_BINDS];
    VkDescriptorBufferInfo bufs[MAX_BINDS];
    int nw = 0, ni = 0, nb = 0;
    int si = 0, so = 0;

    VkImageView gen_sampled[8] = {
        h0, h1, g_motion_view, g_detail1_view, g_detail2_view, h0, h1, g_gen_view
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
    sizes[3].type = VK_DESCRIPTOR_TYPE_SAMPLER; sizes[3].descriptorCount = 16;
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = (uint32_t)g_npass + 4;
    dpci.poolSizeCount = 4; dpci.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(dev, &dpci, nullptr, &g_dpool) != VK_SUCCESS) {
        lsfg_log("W", "step3h-j: descriptor pool fail");
        return false;
    }
    lsfg_log("I", "step3h-j: descriptor pool OK");
    for (int i = 0; i < g_npass; i++) {
        PassRt &p = g_pass[i];
        if (!p.ok || !p.dsl) continue;
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = g_dpool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &p.dsl;
        if (vkAllocateDescriptorSets(dev, &dsai, &p.ds) != VK_SUCCESS) {
            p.ds = VK_NULL_HANDLE;
            lsfg_log("W", "step3h-j: ds alloc fail id=%u", p.id);
        }
    }
    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = qfam;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (vkCreateCommandPool(dev, &pci, nullptr, &g_cp) != VK_SUCCESS) {
        lsfg_log("W", "step3h-j: cmd pool fail");
        return false;
    }
    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = g_cp; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = CMD_SLOTS;
    if (vkAllocateCommandBuffers(dev, &cai, g_cmd) != VK_SUCCESS) {
        lsfg_log("W", "step3h-j: cmd alloc fail");
        return false;
    }
    for (int i = 0; i < CMD_SLOTS; i++) {
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (vkCreateFence(dev, &fci, nullptr, &g_fence[i]) != VK_SUCCESS) {
            lsfg_log("W", "step3h-j: fence fail");
            return false;
        }
    }
    lsfg_log("I", "step3h-j: finish_pool OK");
    return true;
}

static void build_tick(VkDevice dev, VkPhysicalDevice phys, uint32_t qfam) {
    const char *cache = lsfg_host_cache_dir();
    if (!cache || !cache[0]) {
        lsfg_log("W", "step3h-j: no cache dir");
        g_build_fail = 1;
        return;
    }
    if (g_build_phase == 0) {
        lsfg_log("I", "=== LSFG step 3h-j: LIGHT GPU chain (mipmaps+generate) ===");
        if (!ensure_imgs(dev, phys) || !ensure_shared(dev, phys)) {
            g_build_fail = 1;
            return;
        }
        g_pipe_i = 0;
        g_pipe_ok = 0;
        g_npass = 0;
        g_build_phase = 1;
        return;
    }
    if (g_build_phase == 1) {
        if (g_pipe_i >= N_LIGHT) {
            g_build_phase = 3;
            return;
        }
        const PassDef &def = k_light[g_pipe_i];
        lsfg_log("I", "step3h-j: creating pipe %d/%d id=%u %s ...",
                 g_pipe_i + 1, N_LIGHT, def.id, def.name);
        PassRt tmp{};
        if (!build_pass(dev, cache, def, &tmp)) {
            lsfg_log("W", "step3h-j: pipe FAIL id=%u — abort", def.id);
            g_build_fail = 1;
            return;
        }
        g_pass[g_npass] = tmp;
        g_npass++;
        g_pipe_ok++;
        lsfg_log("I", "step3h-j: pipe OK id=%u %s (ok=%d npass=%d)",
                 def.id, def.name, g_pipe_ok, g_npass);
        g_pipe_i++;
        if (g_pipe_i >= N_LIGHT) {
            lsfg_log("I", "step3h-j: pipelines ok=%d/%d npass=%d mode=gpu-light",
                     g_pipe_ok, N_LIGHT, g_npass);
            g_build_phase = 3;
        }
        return;
    }
    if (g_build_phase == 3) {
        if (!finish_pool(dev, qfam)) {
            lsfg_log("W", "step3h-j: finish_pool failed — FG disabled");
            g_build_fail = 1;
            return;
        }
        g_ready = 1;
        g_build_phase = 4;
        lsfg_log("I", "step3h-j: READY live=%d inject=%d throttle=%u stages=%d mode=gpu-mip+gen",
                 g_fg_live, g_fg_inject, g_throttle, g_pipe_ok);
        lsfg_log("I", "step3h-j: async 1-in-flight, NO WaitIdle — present-safe");
        lsfg_log("I", "NEXT: GPU dispatch mipmaps→generate; inject when fence signals");
    }
}

static void poll_fence(VkDevice dev) {
    if (g_inflight < 0) return;
    int s = g_inflight;
    if (vkWaitForFences(dev, 1, &g_fence[s], VK_TRUE, 0) == VK_SUCCESS) {
        g_gen_ready = 1;
        g_inflight = -1;
    }
}

static uint32_t groups(uint32_t size) {
    return (size + 15u) / 16u;
}

static bool submit_full(VkDevice dev) {
    if (!g_ready || g_npass < 2) return false;
    if (g_inflight >= 0) return false;

    VkQueue q = (VkQueue)lsfg_step3d_get_queue();
    if (!q) return false;

    VkImageView h0 = (VkImageView)lsfg_step3d_get_view(0);
    VkImageView h1 = (VkImageView)lsfg_step3d_get_view(1);
    if (!h0 && !h1) return false;
    if (!h0) h0 = h1;
    if (!h1) h1 = h0;

    uint32_t w = lsfg_step3d_fb_width();
    uint32_t h = lsfg_step3d_fb_height();
    if (w < 16) w = 16;
    if (h < 16) h = 16;

    int s = g_slot % CMD_SLOTS;
    if (vkGetFenceStatus(dev, g_fence[s]) != VK_SUCCESS)
        return false;
    vkResetFences(dev, 1, &g_fence[s]);
    vkResetCommandBuffer(g_cmd[s], 0);

    upload_ubo(0.5f);
    update_ds(dev, &g_pass[0], h0, h1);
    update_ds(dev, &g_pass[1], h0, h1);

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(g_cmd[s], &bi) != VK_SUCCESS) return false;

    if (!g_cleared) {
        barrier_undef_to_general(g_cmd[s], g_gen);
        for (int i = 0; i < MIP_LEVELS; i++)
            barrier_undef_to_general(g_cmd[s], g_mip[i]);
        barrier_undef_to_general(g_cmd[s], g_motion);
        barrier_undef_to_general(g_cmd[s], g_detail1);
        barrier_undef_to_general(g_cmd[s], g_detail2);
        VkClearColorValue z{};
        VkImageSubresourceRange sr{};
        sr.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        sr.levelCount = 1; sr.layerCount = 1;
        vkCmdClearColorImage(g_cmd[s], g_motion, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &sr);
        vkCmdClearColorImage(g_cmd[s], g_detail1, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &sr);
        vkCmdClearColorImage(g_cmd[s], g_detail2, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &sr);
        barrier_img(g_cmd[s], g_motion);
        barrier_img(g_cmd[s], g_detail1);
        barrier_img(g_cmd[s], g_detail2);
        g_cleared = 1;
    }

    VkImage hist0 = (VkImage)lsfg_step3d_get_image(0);
    VkImage hist1 = (VkImage)lsfg_step3d_get_image(1);
    if (hist0) barrier_img(g_cmd[s], hist0);
    if (hist1) barrier_img(g_cmd[s], hist1);

    {
        PassRt &p = g_pass[0];
        vkCmdBindPipeline(g_cmd[s], VK_PIPELINE_BIND_POINT_COMPUTE, p.pipe);
        vkCmdBindDescriptorSets(g_cmd[s], VK_PIPELINE_BIND_POINT_COMPUTE, p.pl, 0, 1, &p.ds, 0, nullptr);
        uint32_t gw = groups(g_mip_w[0] ? g_mip_w[0] : w);
        uint32_t gh = groups(g_mip_h[0] ? g_mip_h[0] : h);
        vkCmdDispatch(g_cmd[s], gw, gh, 1);
        for (int i = 0; i < MIP_LEVELS; i++)
            barrier_img(g_cmd[s], g_mip[i]);
    }

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

extern "C" int lsfg_step3hj_should_inject(void) {
    return (g_fg_inject && g_gen_ready && g_gen) ? 1 : 0;
}
extern "C" void lsfg_step3hj_note_inject(void) {
    g_inject_n++;
    g_gen_ready = 0;
    if (g_inject_n <= 5 || g_inject_n == 60 || (g_inject_n % 300) == 0)
        lsfg_log("I", "step3h-j: INJECT #%u (gen→swapchain GPU)", g_inject_n);
}
extern "C" void lsfg_step3hj_install(void) {
    lsfg_log("I", "LSFG: step3h-j LIGHT GPU chain installer ready (mipmaps+generate)");
}
