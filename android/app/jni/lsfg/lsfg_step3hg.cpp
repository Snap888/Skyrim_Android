/* Step 3h-g: full LSFG pass chain (mipmaps -> alpha/beta/gamma/delta -> generate).
 * One async probe on dedicated cmd buffer. Present inject still OFF.
 */
#include "lsfg_log.h"

#include <stdio.h>
#include <string.h>
#include <vector>
#include <algorithm>

#ifdef __ANDROID__
#include <vulkan/vulkan.h>
#else
#include <vulkan/vulkan_core.h>
#endif

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
extern "C" int lsfg_role_perf_mode(void);
extern "C" void lsfg_step3hh_after_chain(void *device, unsigned qfam);

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

static const PassDef k_quality[] = {
    { "mipmaps",  255 },
    { "alpha0",   267 }, { "alpha1", 268 }, { "alpha2", 269 }, { "alpha3", 270 },
    { "beta0",    275 }, { "beta1",  276 }, { "beta2",  277 }, { "beta3",  278 }, { "beta4", 279 },
    { "gamma0",   257 }, { "gamma1", 259 }, { "gamma2", 260 }, { "gamma3", 261 }, { "gamma4", 262 },
    { "delta5",   258 },
    { "delta1",   263 }, { "delta2", 264 }, { "delta3", 265 }, { "delta4", 266 },
    { "delta6",   271 }, { "delta7", 272 }, { "delta8", 273 }, { "delta9", 274 },
    { "generate", 256 },
};

struct PassRt {
    const char *name;
    uint32_t id;
    VkShaderModule sm;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout pl;
    VkPipeline pipe;
    VkDescriptorSet ds;
    std::vector<uint32_t> binds;
    int ok;
};

static VkDevice g_dev = VK_NULL_HANDLE;
static VkPhysicalDevice g_phys = VK_NULL_HANDLE;
static uint32_t g_qfam;
static VkCommandPool g_cp = VK_NULL_HANDLE;
static VkCommandBuffer g_cmd = VK_NULL_HANDLE;
static VkFence g_fence = VK_NULL_HANDLE;
static VkDescriptorPool g_dpool = VK_NULL_HANDLE;
static std::vector<PassRt> g_passes;
static int g_ready;
static int g_probe_done;

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

static void reflect_binds(const std::vector<uint32_t> &c, std::vector<uint32_t> &out) {
    out.clear();
    std::vector<uint32_t> tmp;
    for (size_t i = 5; i < c.size();) {
        uint32_t w = c[i], op = w & 0xffff, len = w >> 16;
        if (!len) break;
        if (op == 71 && len >= 4 && c[i+2] == 33)
            tmp.push_back(c[i+3]);
        i += len;
    }
    std::sort(tmp.begin(), tmp.end());
    tmp.erase(std::unique(tmp.begin(), tmp.end()), tmp.end());
    out = tmp;
}

static bool build_pass(VkDevice dev, const char *cache, const PassDef &def, PassRt *p) {
    memset(p, 0, sizeof(*p));
    p->name = def.name;
    p->id = def.id;
    char path[700];
    snprintf(path, sizeof path, "%s/%u.spv", cache, def.id);
    std::vector<uint32_t> code;
    if (!load_spv(path, code)) {
        lsfg_log("W", "step3h-g: missing %s id=%u", def.name, def.id);
        return false;
    }
    reflect_binds(code, p->binds);

    VkShaderModuleCreateInfo smci{};
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = code.size() * 4;
    smci.pCode = code.data();
    if (vkCreateShaderModule(dev, &smci, nullptr, &p->sm) != VK_SUCCESS) return false;

    std::vector<VkDescriptorSetLayoutBinding> lbs;
    for (uint32_t b : p->binds) {
        VkDescriptorSetLayoutBinding lb{};
        lb.binding = b;
        lb.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
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
        lsfg_log("W", "step3h-g: pipeline fail %s id=%u", def.name, def.id);
        return false;
    }
    p->ok = 1;
    return true;
}

static void update_ds(VkDevice dev, PassRt *p,
                      VkImageView h0, VkImageView h1,
                      VkImageView mip0, VkImageView flow0, VkImageView gen0) {
    if (!p->ok || !p->ds || p->binds.empty()) return;
    VkImageView seq[5] = { h0, h1, mip0, flow0, gen0 };
    std::vector<VkWriteDescriptorSet> writes;
    std::vector<VkDescriptorImageInfo> imgs;
    imgs.reserve(p->binds.size());
    writes.reserve(p->binds.size());
    for (size_t i = 0; i < p->binds.size(); i++) {
        VkDescriptorImageInfo ii{};
        ii.imageView = seq[i % 5] ? seq[i % 5] : h0;
        ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        imgs.push_back(ii);
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = p->ds;
        w.dstBinding = p->binds[i];
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w.pImageInfo = &imgs.back();
        writes.push_back(w);
    }
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

static bool build(VkDevice dev, uint32_t qfam) {
    if (g_ready) return true;
    if (!lsfg_step3he_pyramid_ready()) return false;
    const char *cache = lsfg_host_cache_dir();
    if (!cache || !*cache) return false;

    lsfg_log("I", "=== LSFG step 3h-g: full flow chain (alpha/beta/gamma/delta) ===");

    int perf = lsfg_role_perf_mode();
    const PassDef *defs = perf ? k_perf : k_quality;
    size_t ndefs = perf ? (sizeof(k_perf)/sizeof(k_perf[0])) : (sizeof(k_quality)/sizeof(k_quality[0]));

    int ok = 0, fail = 0;
    g_passes.clear();
    g_passes.reserve(ndefs);
    for (size_t i = 0; i < ndefs; i++) {
        PassRt p{};
        if (build_pass(dev, cache, defs[i], &p)) {
            ok++;
            g_passes.push_back(std::move(p));
        } else {
            fail++;
        }
    }
    lsfg_log("I", "step3h-g: pipelines ok=%d fail=%d total=%zu model=%s",
             ok, fail, ndefs, perf ? "3.1P" : "3.1");
    if (ok < 2) {
        lsfg_log("W", "step3h-g: too few pipelines");
        return false;
    }

    uint32_t max_binds = 16;
    for (auto &p : g_passes)
        if (p.binds.size() > max_binds) max_binds = (uint32_t)p.binds.size();

    VkDescriptorPoolSize psz{};
    psz.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    psz.descriptorCount = (uint32_t)g_passes.size() * max_binds + 16;
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = (uint32_t)g_passes.size() + 2;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &psz;
    if (vkCreateDescriptorPool(dev, &dpci, nullptr, &g_dpool) != VK_SUCCESS) return false;

    for (auto &p : g_passes) {
        if (!p.ok) continue;
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = g_dpool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &p.dsl;
        if (vkAllocateDescriptorSets(dev, &dsai, &p.ds) != VK_SUCCESS) {
            lsfg_log("W", "step3h-g: ds alloc fail %s", p.name);
            p.ds = VK_NULL_HANDLE;
        }
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
    cai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(dev, &cai, &g_cmd) != VK_SUCCESS) return false;
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkCreateFence(dev, &fci, nullptr, &g_fence) != VK_SUCCESS) return false;

    g_ready = 1;
    lsfg_log("I", "step3h-g: FULL CHAIN READY stages=%d", ok);
    lsfg_log("I", "step3h-g: present inject still DISABLED");
    return true;
}

static bool submit_full_chain(VkDevice dev) {
    if (!g_ready || !g_cmd) return false;
    VkQueue q = (VkQueue)lsfg_step3d_get_queue();
    if (!q) return false;

    if (vkWaitForFences(dev, 1, &g_fence, VK_TRUE, 0) == VK_TIMEOUT) {
        lsfg_log("W", "step3h-g: fence busy — skip");
        return false;
    }
    vkResetFences(dev, 1, &g_fence);
    vkResetCommandBuffer(g_cmd, 0);

    VkImageView h0 = (VkImageView)lsfg_step3d_get_view(0);
    VkImageView h1 = (VkImageView)lsfg_step3d_get_view(1);
    VkImageView mip0 = (VkImageView)lsfg_step3he_get_mip_view(0);
    VkImageView flow0 = (VkImageView)lsfg_step3he_get_flow_view(0);
    VkImageView gen0 = (VkImageView)lsfg_step3he_get_gen_view(0);
    if (!h0 || !h1 || !gen0) return false;

    for (auto &p : g_passes)
        update_ds(dev, &p, h0, h1, mip0, flow0, gen0);

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(g_cmd, &bi);

    barrier(g_cmd, (VkImage)lsfg_step3d_get_image(0));
    barrier(g_cmd, (VkImage)lsfg_step3d_get_image(1));

    uint32_t w = lsfg_step3d_fb_width();
    uint32_t h = lsfg_step3d_fb_height();
    if (w < 8) w = 8; if (h < 8) h = 8;
    uint32_t fw = (w * 70) / 100; if (fw < 8) fw = 8;
    uint32_t fh = (h * 70) / 100; if (fh < 8) fh = 8;

    int dispatched = 0;
    for (auto &p : g_passes) {
        if (!p.ok || !p.pipe) continue;
        vkCmdBindPipeline(g_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipe);
        if (p.ds && !p.binds.empty())
            vkCmdBindDescriptorSets(g_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pl, 0, 1, &p.ds, 0, nullptr);
        int is_gen = (p.id == 256);
        uint32_t gx = is_gen ? ((w + 7) / 8) : ((fw + 7) / 8);
        uint32_t gy = is_gen ? ((h + 7) / 8) : ((fh + 7) / 8);
        vkCmdDispatch(g_cmd, gx, gy, 1);
        dispatched++;
    }

    vkEndCommandBuffer(g_cmd);

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g_cmd;
    if (vkQueueSubmit(q, 1, &si, g_fence) != VK_SUCCESS) {
        lsfg_log("W", "step3h-g: QueueSubmit fail");
        return false;
    }
    lsfg_log("I", "step3h-g: FULL CHAIN SUBMIT OK stages=%d (async) %ux%u",
             dispatched, w, h);
    return true;
}

extern "C" void lsfg_step3hg_set_phys(void *phys) {
    g_phys = (VkPhysicalDevice)phys;
}

extern "C" int lsfg_step3hg_chain_ready(void) { return g_ready; }

extern "C" void lsfg_step3hg_after_secondary(void *device, unsigned qfam) {
    if (!device || !g_phys) return;
    if (!lsfg_step3he_pyramid_ready()) return;
    g_dev = (VkDevice)device;
    g_qfam = qfam;

    if (!g_ready) {
        if (!build(g_dev, qfam)) return;
    }
    if (g_probe_done) return;
    g_probe_done = 1;

    if (submit_full_chain(g_dev)) {
        lsfg_log("I", "step3h-g: order mipmaps->alpha->beta->gamma->delta->generate");
        lsfg_log("I", "NEXT step 3h-h: correct per-pass bindings + present inject (optional)");
        lsfg_log("I", "NOTE: per-frame FG still OFF — FPS safe");
        lsfg_step3hh_after_chain((void*)g_dev, g_qfam);
    }
}

extern "C" void lsfg_step3hg_install(void) {
    lsfg_log("I", "LSFG: step3h-g full flow chain installer ready");
}
