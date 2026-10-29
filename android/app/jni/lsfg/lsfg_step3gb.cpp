/* Step 3g-b: descriptor set layout from SPIR-V bindings + one sample compute dispatch
 * on history images. Does NOT implement optical-flow FG — only proves descriptors
 * and dispatch path work on Adreno. */
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
extern "C" void *lsfg_step3d_get_view(int idx);
extern "C" void *lsfg_step3d_get_image(int idx);
extern "C" void *lsfg_step3d_get_queue(void);
extern "C" unsigned lsfg_step3d_fb_width(void);
extern "C" unsigned lsfg_step3d_fb_height(void);
extern "C" const char *lsfg_host_cache_dir(void);

static VkDevice g_dev = VK_NULL_HANDLE;
static uint32_t g_qfam;
static VkDescriptorSetLayout g_dsl = VK_NULL_HANDLE;
static VkPipelineLayout g_pl = VK_NULL_HANDLE;
static VkPipeline g_pipe = VK_NULL_HANDLE;
static VkDescriptorPool g_pool = VK_NULL_HANDLE;
static VkDescriptorSet g_ds = VK_NULL_HANDLE;
static VkCommandPool g_cp = VK_NULL_HANDLE;
static VkCommandBuffer g_cmd = VK_NULL_HANDLE;
static VkShaderModule g_sm = VK_NULL_HANDLE;
static std::vector<uint32_t> g_binds;
static int g_ready;
static int g_dispatched;
static std::atomic<unsigned> g_try_n{0};
static unsigned g_shader_id;

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

static bool is_compute(const std::vector<uint32_t> &c) {
    if (c.size() < 5) return false;
    for (size_t i = 5; i < c.size();) {
        uint32_t w = c[i], op = w & 0xffff, len = w >> 16;
        if (!len) break;
        if (op == 15 && i + 1 < c.size() && c[i + 1] == 5) return true;
        i += len;
    }
    return false;
}

static void reflect_bindings(const std::vector<uint32_t> &c, std::vector<uint32_t> &out) {
    out.clear();
    std::unordered_map<uint32_t, uint32_t> id_bind;
    for (size_t i = 5; i < c.size();) {
        uint32_t w = c[i], op = w & 0xffff, len = w >> 16;
        if (!len) break;
        if (op == 71 && len >= 4 && i + 3 < c.size()) {
            uint32_t tgt = c[i + 1], dec = c[i + 2], lit = c[i + 3];
            if (dec == 33) id_bind[tgt] = lit;
        }
        i += len;
    }
    for (auto &kv : id_bind) out.push_back(kv.second);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

static bool pick_shader(const char *cache, std::vector<uint32_t> &code, unsigned *out_id) {
    char man[600];
    snprintf(man, sizeof man, "%s/manifest_spirv.txt", cache);
    FILE *f = fopen(man, "r");
    if (!f) return false;
    char line[256];
    unsigned best_id = 0;
    size_t best_binds = 9999;
    std::vector<uint32_t> best_code;
    while (fgets(line, sizeof line, f)) {
        unsigned id = 0;
        if (sscanf(line, "%u", &id) != 1) continue;
        if (strstr(line, " fail ") || strstr(line, " skip ")) continue;
        char path[700];
        snprintf(path, sizeof path, "%s/%u.spv", cache, id);
        std::vector<uint32_t> c;
        if (!load_spv(path, c) || !is_compute(c)) continue;
        std::vector<uint32_t> b;
        reflect_bindings(c, b);
        if (b.empty()) continue;
        if (b.size() < best_binds) {
            best_binds = b.size();
            best_id = id;
            best_code = std::move(c);
        }
    }
    fclose(f);
    if (!best_id) return false;
    code = std::move(best_code);
    *out_id = best_id;
    return true;
}

static bool build(VkDevice dev, uint32_t qfam) {
    if (g_ready) return true;
    if (!dev || !lsfg_step3d_frames_ready()) return false;
    const char *cache = lsfg_host_cache_dir();
    if (!cache || !*cache) return false;

    std::vector<uint32_t> code;
    if (!pick_shader(cache, code, &g_shader_id)) {
        lsfg_log("W", "step3g-b: no compute shader with bindings");
        return false;
    }
    reflect_bindings(code, g_binds);
    lsfg_log("I", "=== LSFG step 3g-b: descriptor + sample dispatch ===");
    lsfg_log("I", "step3g-b: probe shader id=%u bindings=%zu", g_shader_id, g_binds.size());

    VkShaderModuleCreateInfo smci{};
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = code.size() * 4;
    smci.pCode = code.data();
    if (vkCreateShaderModule(dev, &smci, nullptr, &g_sm) != VK_SUCCESS) {
        lsfg_log("W", "step3g-b: ShaderModule fail");
        return false;
    }

    std::vector<VkDescriptorSetLayoutBinding> lbs;
    lbs.reserve(g_binds.size());
    for (uint32_t b : g_binds) {
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
    dlci.pBindings = lbs.data();
    if (vkCreateDescriptorSetLayout(dev, &dlci, nullptr, &g_dsl) != VK_SUCCESS) {
        lsfg_log("W", "step3g-b: DescriptorSetLayout fail");
        return false;
    }

    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &g_dsl;
    if (vkCreatePipelineLayout(dev, &plci, nullptr, &g_pl) != VK_SUCCESS) {
        lsfg_log("W", "step3g-b: PipelineLayout fail");
        return false;
    }

    VkComputePipelineCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pci.stage.module = g_sm;
    pci.stage.pName = "main";
    pci.layout = g_pl;
    VkResult pr = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pci, nullptr, &g_pipe);
    if (pr != VK_SUCCESS) {
        lsfg_log("W", "step3g-b: ComputePipeline fail %d (binding types may mismatch)", (int)pr);
        return false;
    }
    lsfg_log("I", "step3g-b: compute pipeline OK id=%u layout_bindings=%zu", g_shader_id, g_binds.size());

    VkDescriptorPoolSize dps{};
    dps.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    dps.descriptorCount = (uint32_t)g_binds.size() + 4;
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &dps;
    if (vkCreateDescriptorPool(dev, &dpci, nullptr, &g_pool) != VK_SUCCESS) {
        lsfg_log("W", "step3g-b: DescriptorPool fail");
        return false;
    }
    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = g_pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &g_dsl;
    if (vkAllocateDescriptorSets(dev, &dsai, &g_ds) != VK_SUCCESS) {
        lsfg_log("W", "step3g-b: AllocateDescriptorSets fail");
        return false;
    }

    VkCommandPoolCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.queueFamilyIndex = qfam;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (vkCreateCommandPool(dev, &cpci, nullptr, &g_cp) != VK_SUCCESS) {
        lsfg_log("W", "step3g-b: CommandPool fail");
        return false;
    }
    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = g_cp;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(dev, &cbai, &g_cmd) != VK_SUCCESS) {
        lsfg_log("W", "step3g-b: CommandBuffer fail");
        return false;
    }

    g_dev = dev;
    g_qfam = qfam;
    g_ready = 1;
    lsfg_log("I", "step3g-b: ready — will dispatch after history copies");
    return true;
}

static void update_descriptors(void) {
    VkImageView v0 = (VkImageView)lsfg_step3d_get_view(0);
    VkImageView v1 = (VkImageView)lsfg_step3d_get_view(1);
    if (!v0 || !v1 || !g_ds) return;

    std::vector<VkWriteDescriptorSet> writes;
    std::vector<VkDescriptorImageInfo> infos;
    writes.reserve(g_binds.size());
    infos.reserve(g_binds.size());
    for (size_t i = 0; i < g_binds.size(); i++) {
        VkDescriptorImageInfo ii{};
        ii.imageView = (i & 1) ? v1 : v0;
        ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        infos.push_back(ii);
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = g_ds;
        w.dstBinding = g_binds[i];
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w.pImageInfo = &infos.back();
        writes.push_back(w);
    }
    vkUpdateDescriptorSets(g_dev, (uint32_t)writes.size(), writes.data(), 0, nullptr);
}

extern "C" void lsfg_step3gb_try_dispatch(void) {
    if (!g_ready || g_dispatched) return;
    if (!lsfg_step3d_frames_ready()) return;
    VkQueue queue = (VkQueue)lsfg_step3d_get_queue();
    if (!queue || !g_cmd || !g_pipe) return;

    unsigned w = lsfg_step3d_fb_width();
    unsigned h = lsfg_step3d_fb_height();
    if (!w || !h) return;

    update_descriptors();

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkResetCommandBuffer(g_cmd, 0);
    if (vkBeginCommandBuffer(g_cmd, &bi) != VK_SUCCESS) {
        lsfg_log("W", "step3g-b: BeginCommandBuffer fail");
        return;
    }

    VkImageMemoryBarrier barriers[2]{};
    for (int i = 0; i < 2; i++) {
        barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[i].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        barriers[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barriers[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barriers[i].image = (VkImage)lsfg_step3d_get_image(i);
        barriers[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barriers[i].subresourceRange.levelCount = 1;
        barriers[i].subresourceRange.layerCount = 1;
    }
    vkCmdPipelineBarrier(g_cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 2, barriers);

    vkCmdBindPipeline(g_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_pipe);
    vkCmdBindDescriptorSets(g_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_pl, 0, 1, &g_ds, 0, nullptr);

    uint32_t gx = (w + 7) / 8;
    uint32_t gy = (h + 7) / 8;
    vkCmdDispatch(g_cmd, gx, gy, 1);

    if (vkEndCommandBuffer(g_cmd) != VK_SUCCESS) {
        lsfg_log("W", "step3g-b: EndCommandBuffer fail");
        return;
    }

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g_cmd;
    VkResult sr = vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    if (sr != VK_SUCCESS) {
        lsfg_log("W", "step3g-b: QueueSubmit fail %d", (int)sr);
        return;
    }
    vkQueueWaitIdle(queue);

    g_dispatched = 1;
    lsfg_log("I", "step3g-b: DISPATCH OK id=%u %ux%u groups=%ux%u bindings=%zu",
             g_shader_id, w, h, gx, gy, g_binds.size());
    lsfg_log("I", "NEXT step 3h: wire real LSFG pass graph (flow->generate) + present generated");
}

extern "C" void lsfg_step3gb_after_copy(void *device, unsigned qfam) {
    unsigned n = ++g_try_n;
    if (!g_ready && device)
        build((VkDevice)device, qfam);
    if (n == 2 || n == 60)
        lsfg_step3gb_try_dispatch();
}

extern "C" void lsfg_step3gb_install(void) {
    lsfg_log("I", "LSFG: step3g-b probe installed (descriptor+dispatch)");
}
