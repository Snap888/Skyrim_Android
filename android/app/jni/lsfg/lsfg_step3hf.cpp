/* Step 3h-f: multi-pass on dedicated cmd buffers (not DXVK present list).
 *
 * FPS rules:
 *  - Never block present path with long vkWaitForFences
 *  - Probe-only by default (one multi-pass submit)
 *  - Double-buffered cmd buffers for future per-frame FG without stall
 *  - Present inject installed but DISABLED
 */
#include "lsfg_log.h"

#include <stdio.h>
#include <string.h>
#include <vector>
#include <algorithm>
#include <atomic>

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
extern "C" uint32_t lsfg_role_mipmaps_id(void);
extern "C" uint32_t lsfg_role_generate_id(void);

enum { CMD_SLOTS = 2 };

static VkDevice g_dev = VK_NULL_HANDLE;
static VkPhysicalDevice g_phys = VK_NULL_HANDLE;
static uint32_t g_qfam;
static VkCommandPool g_pool = VK_NULL_HANDLE;
static VkCommandBuffer g_cmd[CMD_SLOTS] = {};
static VkFence g_fence[CMD_SLOTS] = {};
static int g_slot;
static int g_ready;
static int g_probe_done;
static int g_inject_enabled;

struct Pass {
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
static Pass g_mip{}, g_gen{};
static VkDescriptorPool g_dpool = VK_NULL_HANDLE;

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

static bool build_pass(VkDevice dev, const char *cache, uint32_t id, const char *name, Pass *p) {
    memset(p, 0, sizeof(*p));
    p->id = id; p->name = name;
    char path[700];
    snprintf(path, sizeof path, "%s/%u.spv", cache, id);
    std::vector<uint32_t> code;
    if (!load_spv(path, code)) return false;
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
    if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pci, nullptr, &p->pipe) != VK_SUCCESS)
        return false;
    p->ok = 1;
    return true;
}

static void update_ds(VkDevice dev, Pass *p, VkImageView a, VkImageView b, VkImageView c, VkImageView d) {
    if (!p->ok || !p->ds || p->binds.empty()) return;
    VkImageView seq[4] = { a, b, c, d };
    std::vector<VkWriteDescriptorSet> writes;
    std::vector<VkDescriptorImageInfo> imgs;
    imgs.reserve(p->binds.size());
    writes.reserve(p->binds.size());
    for (size_t i = 0; i < p->binds.size(); i++) {
        VkDescriptorImageInfo ii{};
        ii.imageView = seq[i % 4] ? seq[i % 4] : a;
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

static bool ensure_cmd(VkDevice dev, uint32_t qfam) {
    if (g_pool) return true;
    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = qfam;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (vkCreateCommandPool(dev, &pci, nullptr, &g_pool) != VK_SUCCESS) return false;

    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = g_pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = CMD_SLOTS;
    if (vkAllocateCommandBuffers(dev, &cai, g_cmd) != VK_SUCCESS) return false;

    for (int i = 0; i < CMD_SLOTS; i++) {
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (vkCreateFence(dev, &fci, nullptr, &g_fence[i]) != VK_SUCCESS) return false;
    }
    return true;
}

static bool build(VkDevice dev, VkPhysicalDevice phys, uint32_t qfam) {
    if (g_ready) return true;
    if (!lsfg_step3he_pyramid_ready()) return false;
    const char *cache = lsfg_host_cache_dir();
    if (!cache || !*cache) return false;
    if (!ensure_cmd(dev, qfam)) {
        lsfg_log("W", "step3h-f: cmd pool failed");
        return false;
    }

    lsfg_log("I", "=== LSFG step 3h-f: secondary cmd multi-pass ===");
    lsfg_log("I", "step3h-f: cmd slots=%d (double-buffer, non-blocking present path)", CMD_SLOTS);

    if (!build_pass(dev, cache, lsfg_role_mipmaps_id(), "mipmaps", &g_mip) ||
        !build_pass(dev, cache, lsfg_role_generate_id(), "generate", &g_gen)) {
        lsfg_log("W", "step3h-f: pass build failed");
        return false;
    }

    VkDescriptorPoolSize psz{};
    psz.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    psz.descriptorCount = 64;
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 4;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &psz;
    if (vkCreateDescriptorPool(dev, &dpci, nullptr, &g_dpool) != VK_SUCCESS) return false;

    VkDescriptorSetLayout lays[2] = { g_mip.dsl, g_gen.dsl };
    VkDescriptorSet sets[2];
    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = g_dpool;
    dsai.descriptorSetCount = 2;
    dsai.pSetLayouts = lays;
    if (vkAllocateDescriptorSets(dev, &dsai, sets) != VK_SUCCESS) {
        lsfg_log("W", "step3h-f: descriptor alloc fail");
        return false;
    }
    g_mip.ds = sets[0];
    g_gen.ds = sets[1];

    g_ready = 1;
    lsfg_log("I", "step3h-f: SECONDARY READY mipmaps+generate binds=%zu/%zu",
             g_mip.binds.size(), g_gen.binds.size());
    lsfg_log("I", "step3h-f: present inject DISABLED (safe)");
    return true;
}

static bool submit_multipass(VkDevice dev) {
    if (!g_ready) return false;
    VkQueue q = (VkQueue)lsfg_step3d_get_queue();
    if (!q) return false;

    int s = g_slot % CMD_SLOTS;
    g_slot++;

    VkResult wr = vkWaitForFences(dev, 1, &g_fence[s], VK_TRUE, 0);
    if (wr == VK_TIMEOUT) {
        lsfg_log("W", "step3h-f: slot %d busy — skip (non-blocking)", s);
        return false;
    }
    vkResetFences(dev, 1, &g_fence[s]);
    vkResetCommandBuffer(g_cmd[s], 0);

    VkImageView h0 = (VkImageView)lsfg_step3d_get_view(0);
    VkImageView h1 = (VkImageView)lsfg_step3d_get_view(1);
    VkImageView mip0 = (VkImageView)lsfg_step3he_get_mip_view(0);
    VkImageView flow0 = (VkImageView)lsfg_step3he_get_flow_view(0);
    VkImageView gen0 = (VkImageView)lsfg_step3he_get_gen_view(0);
    if (!h0 || !h1 || !gen0) return false;

    update_ds(dev, &g_mip, h0, h1, mip0, flow0);
    update_ds(dev, &g_gen, h0, h1, flow0, gen0);

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

    if (g_mip.ok) {
        vkCmdBindPipeline(g_cmd[s], VK_PIPELINE_BIND_POINT_COMPUTE, g_mip.pipe);
        if (!g_mip.binds.empty())
            vkCmdBindDescriptorSets(g_cmd[s], VK_PIPELINE_BIND_POINT_COMPUTE, g_mip.pl, 0, 1, &g_mip.ds, 0, nullptr);
        vkCmdDispatch(g_cmd[s], (fw + 7) / 8, (fh + 7) / 8, 1);
    }
    if (g_gen.ok) {
        vkCmdBindPipeline(g_cmd[s], VK_PIPELINE_BIND_POINT_COMPUTE, g_gen.pipe);
        if (!g_gen.binds.empty())
            vkCmdBindDescriptorSets(g_cmd[s], VK_PIPELINE_BIND_POINT_COMPUTE, g_gen.pl, 0, 1, &g_gen.ds, 0, nullptr);
        vkCmdDispatch(g_cmd[s], (w + 7) / 8, (h + 7) / 8, 1);
    }

    vkEndCommandBuffer(g_cmd[s]);

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g_cmd[s];
    if (vkQueueSubmit(q, 1, &si, g_fence[s]) != VK_SUCCESS) {
        lsfg_log("W", "step3h-f: QueueSubmit fail");
        return false;
    }
    return true;
}

extern "C" void lsfg_step3hf_set_phys(void *phys) {
    g_phys = (VkPhysicalDevice)phys;
}

extern "C" int lsfg_step3hf_inject_enabled(void) { return g_inject_enabled; }

extern "C" void lsfg_step3hf_on_present(void *device, void *swapchain_image,
                                        uint32_t w, uint32_t h) {
    (void)device; (void)swapchain_image; (void)w; (void)h;
    if (!g_inject_enabled) return;
}

extern "C" void lsfg_step3hf_after_pyramid(void *device, unsigned qfam) {
    if (!device || !g_phys) return;
    if (!lsfg_step3he_pyramid_ready()) return;
    g_dev = (VkDevice)device;
    g_qfam = qfam;

    if (!g_ready) {
        if (!build(g_dev, g_phys, qfam)) return;
    }
    if (g_probe_done) return;
    g_probe_done = 1;

    if (submit_multipass(g_dev)) {
        lsfg_log("I", "step3h-f: MULTI-PASS SUBMIT OK (async, no wait) %ux%u slot=0",
                 lsfg_step3d_fb_width(), lsfg_step3d_fb_height());
        lsfg_log("I", "step3h-f: order mipmaps -> generate on secondary cmd buffer");
        lsfg_log("I", "NEXT step 3h-g: full alpha/beta/gamma/delta chain + optional present inject");
        lsfg_log("I", "NOTE: per-frame FG still OFF — FPS safe");
    }
}

extern "C" void lsfg_step3hf_install(void) {
    lsfg_log("I", "LSFG: step3h-f secondary multi-pass installer ready (inject off)");
}
