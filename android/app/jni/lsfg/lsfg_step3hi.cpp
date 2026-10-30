/* Step 3h-i: per-frame FG with pacing (OFF by default).
 * CRITICAL: full 25-pass every frame → ~0.1 FPS on Adreno.
 * Live path = LIGHT (mipmaps+generate only) + throttle (default 30).
 * Config: fg_live=1, fg_throttle=30, fg_light=1
 */
#include "lsfg_log.h"

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

extern "C" int  lsfg_step3hh_present_enabled(void);
extern "C" int  lsfg_step3he_pyramid_ready(void);
extern "C" void *lsfg_step3he_get_gen_view(int idx);
extern "C" void *lsfg_step3he_get_mip_view(int idx);
extern "C" void *lsfg_step3he_get_flow_view(int idx);
extern "C" void *lsfg_step3d_get_view(int idx);
extern "C" void *lsfg_step3d_get_image(int idx);
extern "C" void *lsfg_step3d_get_queue(void);
extern "C" unsigned lsfg_step3d_fb_width(void);
extern "C" unsigned lsfg_step3d_fb_height(void);
extern "C" const char *lsfg_host_cache_dir(void);

struct PassDef {
    const char *name;
    uint32_t id;
};

static const PassDef k_perf[] = {
    { "p_mipmaps",  255 },
    { "p_alpha0",   290 }, { "p_alpha1", 291 }, { "p_alpha2", 292 }, { "p_alpha3", 293 },
    { "p_beta0",    298 }, { "p_beta1",  299 }, { "p_beta2",  300 }, { "p_beta3",  301 }, { "p_beta4", 302 },
    { "p_gamma0",   280 }, { "p_gamma1", 282 }, { "p_gamma2", 283 }, { "p_gamma3", 284 }, { "p_gamma4", 285 },
    { "p_delta5",   281 },
    { "p_delta1",   286 }, { "p_delta2", 287 }, { "p_delta3", 288 }, { "p_delta4", 289 },
    { "p_delta6",   294 }, { "p_delta7", 295 }, { "p_delta8", 296 }, { "p_delta9", 297 },
    { "p_generate", 256 },
};

struct BindInfo {
    uint32_t binding;
    VkDescriptorType type;
};

struct PassRt {
    const char *name;
    uint32_t id;
    VkShaderModule sm;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout pl;
    VkPipeline pipe;
    VkDescriptorSet ds;
    std::vector<BindInfo> binds;
    int ok;
};

enum { CMD_SLOTS = 2 };

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
static std::vector<PassRt> g_passes;
static int g_ready;
static int g_fg_live;
static int g_logged_live;
static std::atomic<unsigned> g_frame_n{0};
static unsigned g_submit_n;
static unsigned g_skip_n;
static unsigned g_throttle = 30;
static int g_light = 1;

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

static void reflect(const std::vector<uint32_t> &c, std::vector<BindInfo> &out) {
    out.clear();
    std::unordered_map<uint32_t, uint32_t> id_bind, id_type, type_pointee;
    std::unordered_map<uint32_t, int> type_kind;
    for (size_t i = 5; i < c.size();) {
        uint32_t w = c[i], op = w & 0xffff, len = w >> 16;
        if (!len) break;
        if (op == 71 && len >= 4) {
            if (c[i+2] == 33) id_bind[c[i+1]] = c[i+3];
        } else if (op == 32 && len >= 4) {
            type_pointee[c[i+1]] = c[i+3];
        } else if (op == 30 && len >= 2) {
            type_kind[c[i+1]] = 1;
        } else if (op == 25 && len >= 3) {
            uint32_t sampled = (len > 6) ? c[i+6] : 0;
            type_kind[c[i+1]] = (sampled == 2) ? 3 : 2;
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
    int last_sampled = -1;
    for (int i = (int)out.size() - 1; i >= 0; i--) {
        if (out[i].type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
            last_sampled = i;
            break;
        }
    }
    if (last_sampled >= 0)
        out[last_sampled].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
}

static uint32_t find_mem(VkPhysicalDevice phys, uint32_t bits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props)
            return i;
    return 0;
}

static bool ensure_shared(VkDevice dev, VkPhysicalDevice phys) {
    if (g_sampler) return true;
    if (!phys) return false;
    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(dev, &sci, nullptr, &g_sampler) != VK_SUCCESS) return false;
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = 256;
    bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (vkCreateBuffer(dev, &bci, nullptr, &g_ubo) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, g_ubo, &mr);
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
        f[1] = (float)lsfg_step3d_fb_width();
        f[2] = (float)lsfg_step3d_fb_height();
        vkUnmapMemory(dev, g_ubo_mem);
    }
    return true;
}

static bool build_pass(VkDevice dev, const char *cache, const PassDef &def, PassRt *p) {
    memset(p, 0, sizeof(*p));
    p->name = def.name;
    p->id = def.id;
    char path[700];
    snprintf(path, sizeof path, "%s/%u.spv", cache, def.id);
    std::vector<uint32_t> code;
    if (!load_spv(path, code)) return false;
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
    if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pci, nullptr, &p->pipe) != VK_SUCCESS)
        return false;
    p->ok = 1;
    return true;
}

static void update_ds(VkDevice dev, PassRt *p,
                      VkImageView h0, VkImageView h1,
                      VkImageView *mips, int nmip,
                      VkImageView *flows, int nflow,
                      VkImageView gen0) {
    if (!p->ok || !p->ds || p->binds.empty()) return;
    std::vector<VkWriteDescriptorSet> writes;
    std::vector<VkDescriptorImageInfo> imgs;
    std::vector<VkDescriptorBufferInfo> bufs;
    imgs.reserve(p->binds.size() + 4);
    bufs.reserve(4);
    writes.reserve(p->binds.size());
    int si = 0, fi = 0, n_storage = 0;
    for (auto &b : p->binds)
        if (b.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) n_storage++;
    int storage_i = 0;
    for (auto &b : p->binds) {
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = p->ds;
        w.dstBinding = b.binding;
        w.descriptorCount = 1;
        w.descriptorType = b.type;
        if (b.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
            VkDescriptorBufferInfo bb{};
            bb.buffer = g_ubo; bb.offset = 0; bb.range = 256;
            bufs.push_back(bb);
            w.pBufferInfo = &bufs.back();
        } else if (b.type == VK_DESCRIPTOR_TYPE_SAMPLER) {
            VkDescriptorImageInfo ii{};
            ii.sampler = g_sampler;
            ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            imgs.push_back(ii);
            w.pImageInfo = &imgs.back();
        } else if (b.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
            VkImageView v = gen0;
            if (n_storage > 1) {
                if (storage_i < nflow && flows[storage_i]) v = flows[storage_i];
                else v = gen0;
                storage_i++;
            }
            VkDescriptorImageInfo ii{};
            ii.imageView = v ? v : h0;
            ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            imgs.push_back(ii);
            w.pImageInfo = &imgs.back();
        } else {
            VkImageView v = h0;
            if (si == 0) v = h0;
            else if (si == 1) v = h1;
            else if (si < 2 + nmip && mips[si - 2]) v = mips[si - 2];
            else if (fi < nflow && flows[fi]) { v = flows[fi]; fi++; }
            si++;
            VkDescriptorImageInfo ii{};
            ii.imageView = v ? v : h0;
            ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            imgs.push_back(ii);
            w.pImageInfo = &imgs.back();
        }
        writes.push_back(w);
    }
    if (!writes.empty())
        vkUpdateDescriptorSets(dev, (uint32_t)writes.size(), writes.data(), 0, nullptr);
}

static void barrier(VkCommandBuffer cmd, VkImage img) {
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

static bool build(VkDevice dev, VkPhysicalDevice phys, uint32_t qfam) {
    if (g_ready) return true;
    if (!lsfg_step3he_pyramid_ready()) return false;
    const char *cache = lsfg_host_cache_dir();
    if (!cache || !*cache) return false;
    if (!ensure_shared(dev, phys)) return false;

    lsfg_log("I", "=== LSFG step 3h-i: per-frame FG pacing (storage fix) ===");

    size_t ndefs = sizeof(k_perf) / sizeof(k_perf[0]);
    int ok = 0, n_storage = 0;
    g_passes.clear();
    for (size_t i = 0; i < ndefs; i++) {
        PassRt p{};
        if (!build_pass(dev, cache, k_perf[i], &p)) continue;
        for (auto &b : p.binds)
            if (b.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) n_storage++;
        ok++;
        g_passes.push_back(std::move(p));
    }
    lsfg_log("I", "step3h-i: pipelines ok=%d storage_binds=%d (promoted last sampled→storage)",
             ok, n_storage);
    if (ok < 2) return false;

    VkDescriptorPoolSize sizes[4] = {};
    sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; sizes[0].descriptorCount = 256;
    sizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; sizes[1].descriptorCount = 256;
    sizes[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; sizes[2].descriptorCount = 64;
    sizes[3].type = VK_DESCRIPTOR_TYPE_SAMPLER; sizes[3].descriptorCount = 64;
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = (uint32_t)g_passes.size() + 4;
    dpci.poolSizeCount = 4;
    dpci.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(dev, &dpci, nullptr, &g_dpool) != VK_SUCCESS) return false;

    for (auto &p : g_passes) {
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = g_dpool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &p.dsl;
        if (vkAllocateDescriptorSets(dev, &dsai, &p.ds) != VK_SUCCESS)
            p.ds = VK_NULL_HANDLE;
    }

    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = qfam;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (vkCreateCommandPool(dev, &pci, nullptr, &g_cp) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = g_cp;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = CMD_SLOTS;
    if (vkAllocateCommandBuffers(dev, &cai, g_cmd) != VK_SUCCESS) return false;
    for (int i = 0; i < CMD_SLOTS; i++) {
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (vkCreateFence(dev, &fci, nullptr, &g_fence[i]) != VK_SUCCESS) return false;
    }

    g_ready = 1;
    lsfg_log("I", "step3h-i: READY fg_live=%d light=%d throttle=%u present=%d",
             g_fg_live, g_light, g_throttle, lsfg_step3hh_present_enabled());
    if (!g_fg_live) {
        lsfg_log("I", "step3h-i: per-frame OFF — set fg_live=1 in port/lsfg.conf to enable");
        lsfg_log("I", "NOTE: fg_live uses LIGHT path (2 passes) + throttle (default 30)");
    } else {
        lsfg_log("W", "step3h-i: LIVE ON light=%d throttle=%u — full 25-pass disabled for FPS",
                 g_light, g_throttle);
    }
    return true;
}

static bool submit_frame(VkDevice dev) {
    if (!g_ready || !g_fg_live) return false;
    VkQueue q = (VkQueue)lsfg_step3d_get_queue();
    if (!q) return false;

    int s = g_slot % CMD_SLOTS;
    g_slot++;

    if (vkWaitForFences(dev, 1, &g_fence[s], VK_TRUE, 0) == VK_TIMEOUT)
        return false;
    vkResetFences(dev, 1, &g_fence[s]);
    vkResetCommandBuffer(g_cmd[s], 0);

    VkImageView h0 = (VkImageView)lsfg_step3d_get_view(0);
    VkImageView h1 = (VkImageView)lsfg_step3d_get_view(1);
    VkImageView gen0 = (VkImageView)lsfg_step3he_get_gen_view(0);
    if (!h0 || !h1 || !gen0) return false;

    VkImageView mips[7], flows[12];
    for (int i = 0; i < 7; i++) mips[i] = (VkImageView)lsfg_step3he_get_mip_view(i);
    for (int i = 0; i < 12; i++) flows[i] = (VkImageView)lsfg_step3he_get_flow_view(i);

    for (auto &p : g_passes)
        update_ds(dev, &p, h0, h1, mips, 7, flows, 12, gen0);

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(g_cmd[s], &bi);
    barrier(g_cmd[s], (VkImage)lsfg_step3d_get_image(0));
    barrier(g_cmd[s], (VkImage)lsfg_step3d_get_image(1));

    uint32_t w = lsfg_step3d_fb_width();
    uint32_t h = lsfg_step3d_fb_height();
    if (w < 8) w = 8; if (h < 8) h = 8;
    uint32_t fw = (w * 70) / 100; if (fw < 8) fw = 8;
    uint32_t fh = (h * 70) / 100; if (fh < 8) fh = 8;

    int dispatched = 0;
    for (auto &p : g_passes) {
        if (!p.ok || !p.pipe) continue;
        if (g_light && p.id != 255 && p.id != 256)
            continue;
        vkCmdBindPipeline(g_cmd[s], VK_PIPELINE_BIND_POINT_COMPUTE, p.pipe);
        if (p.ds && !p.binds.empty())
            vkCmdBindDescriptorSets(g_cmd[s], VK_PIPELINE_BIND_POINT_COMPUTE, p.pl, 0, 1, &p.ds, 0, nullptr);
        int is_gen = (p.id == 256);
        vkCmdDispatch(g_cmd[s],
            is_gen ? ((w + 7) / 8) : ((fw + 7) / 8),
            is_gen ? ((h + 7) / 8) : ((fh + 7) / 8), 1);
        dispatched++;
    }
    if (!dispatched) return false;
    vkEndCommandBuffer(g_cmd[s]);

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g_cmd[s];
    if (vkQueueSubmit(q, 1, &si, g_fence[s]) != VK_SUCCESS) return false;
    g_submit_n++;
    return true;
}

extern "C" void lsfg_step3hi_set_phys(void *phys) {
    g_phys = (VkPhysicalDevice)phys;
}

extern "C" void lsfg_step3hi_set_live(int live) {
    g_fg_live = live ? 1 : 0;
    if (g_fg_live && g_throttle < 1)
        g_throttle = 30;
}

extern "C" void lsfg_step3hi_set_throttle(int n) {
    if (n < 1) n = 1;
    if (n > 600) n = 600;
    g_throttle = (unsigned)n;
}

extern "C" void lsfg_step3hi_set_light(int light) {
    g_light = light ? 1 : 0;
}

extern "C" int lsfg_step3hi_live(void) { return g_fg_live; }

extern "C" void lsfg_step3hi_on_frame(void *device, unsigned qfam) {
    if (!device || !g_phys) return;
    if (!lsfg_step3he_pyramid_ready()) return;
    g_dev = (VkDevice)device;
    g_qfam = qfam;

    if (!g_ready) {
        if (!build(g_dev, g_phys, qfam)) return;
    }

    unsigned n = ++g_frame_n;
    if (!g_fg_live) {
        if (!g_logged_live && n >= 2) {
            g_logged_live = 1;
            lsfg_log("I", "step3h-i: idle (fg_live=0) — no per-frame cost");
        }
        return;
    }

    if (n < 3) return;

    if (g_throttle > 1 && (n % g_throttle) != 0) {
        g_skip_n++;
        return;
    }

    bool ok = submit_frame(g_dev);
    if (!ok) g_skip_n++;
    if (n <= 5 || n == 60 || n == 300 || (n % 1800) == 0) {
        lsfg_log("I", "step3h-i: frame #%u submit=%d ok=%u skip=%u light=%d throttle=%u",
                 n, ok ? 1 : 0, g_submit_n, g_skip_n, g_light, g_throttle);
    }
}

extern "C" void lsfg_step3hi_after_typed(void *device, unsigned qfam) {
    if (!device || !g_phys) return;
    g_dev = (VkDevice)device;
    g_qfam = qfam;
    if (!g_ready)
        build(g_dev, g_phys, qfam);
}

extern "C" void lsfg_step3hi_install(void) {
    lsfg_log("I", "LSFG: step3h-i per-frame pacing installer ready (fg_live default OFF)");
}
