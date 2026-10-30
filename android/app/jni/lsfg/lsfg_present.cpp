#include "lsfg_present.h"
#include "lsfg_log.h"
#include "lsfg_host.h"

#include <stdio.h>
#include <string.h>
#include <vector>

#ifdef __ANDROID__
#include <vulkan/vulkan.h>
#else
#include <vulkan/vulkan_core.h>
#endif

extern "C" void lsfg_step3d_set_phys_queue(void *phys, void *queue);
extern "C" void lsfg_step3d_run(void *device, const char *cache_dir, unsigned w, unsigned h);
extern "C" int  lsfg_step3d_frames_ready(void);

static int g_spirv_count;
static unsigned g_present_n;
static int g_present_logged_init;
static int g_vk_validated;
static int g_pipe_built;
static char g_cache_dir[512];
static VkDevice g_vk_device = VK_NULL_HANDLE;
static VkPipelineLayout g_pipe_layout = VK_NULL_HANDLE;
static int g_compute_ok;
static int g_compute_fail;

void lsfg_host_set_spirv_count(int n) { g_spirv_count = n; }
int  lsfg_host_spirv_count(void) { return g_spirv_count; }

void lsfg_host_set_cache_dir(const char *path) {
    if (!path) { g_cache_dir[0] = 0; return; }
    snprintf(g_cache_dir, sizeof g_cache_dir, "%s", path);
}
const char *lsfg_host_cache_dir(void) { return g_cache_dir; }

extern "C" void lsfg_register_vk_device(void *dev) {
    g_vk_device = reinterpret_cast<VkDevice>(dev);
    lsfg_log("I", "LSFG: VkDevice registered %p", (void*)dev);
}

extern "C" void lsfg_register_vk_context(void *dev, void *phys, void *queue, unsigned qfam) {
    g_vk_device = reinterpret_cast<VkDevice>(dev);
    lsfg_step3d_set_phys_queue(phys, queue);
    extern void lsfg_step3e_set_queue(void *queue, unsigned family);
    lsfg_step3e_set_queue(queue, qfam);
    lsfg_log("I", "LSFG: Vk context registered device=%p phys=%p queue=%p family=%u",
             dev, phys, queue, qfam);
}

static bool load_spv_file(const char *path, std::vector<uint32_t> &out) {
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

static bool spirv_is_compute(const std::vector<uint32_t> &code) {
    if (code.size() < 5) return false;
    size_t i = 5;
    while (i < code.size()) {
        uint32_t word = code[i];
        uint32_t op = word & 0xffffu;
        uint32_t len = word >> 16;
        if (len == 0) break;
        if (op == 15 && i + 1 < code.size() && code[i + 1] == 5) return true;
        i += len;
    }
    return false;
}

static void validate_spirv_on_device(VkDevice device) {
    if (g_vk_validated || !device || !g_cache_dir[0]) return;
    char man[600];
    snprintf(man, sizeof man, "%s/manifest_spirv.txt", g_cache_dir);
    FILE *f = fopen(man, "r");
    if (!f) { lsfg_log("W", "vk-validate: no %s; will retry", man); return; }
    g_vk_validated = 1;
    lsfg_log("I", "=== LSFG step 3b: VkShaderModule validation ===");
    int tried = 0, ok = 0, fail = 0;
    char line[256];
    while (fgets(line, sizeof line, f) && tried < 12) {
        unsigned id = 0;
        if (sscanf(line, "%u", &id) != 1) continue;
        if (strstr(line, " fail ") || strstr(line, " skip ")) continue;
        char path[700];
        snprintf(path, sizeof path, "%s/%u.spv", g_cache_dir, id);
        std::vector<uint32_t> code;
        if (!load_spv_file(path, code)) continue;
        tried++;
        VkShaderModuleCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        ci.codeSize = code.size() * sizeof(uint32_t);
        ci.pCode = code.data();
        VkShaderModule mod = VK_NULL_HANDLE;
        VkResult r = vkCreateShaderModule(device, &ci, nullptr, &mod);
        if (r == VK_SUCCESS && mod) {
            ok++;
            lsfg_log("I", "vk-validate: id=%u OK (%zu dwords)%s",
                     id, code.size(), spirv_is_compute(code) ? " [compute]" : "");
            vkDestroyShaderModule(device, mod, nullptr);
        } else {
            fail++;
            lsfg_log("W", "vk-validate: id=%u FAILED VkResult=%d", id, (int)r);
        }
    }
    fclose(f);
    lsfg_log("I", "vk-validate done: tried=%d ok=%d fail=%d", tried, ok, fail);
    if (ok > 0)
        lsfg_log("I", "SPIR-V modules load on GPU — ready for compute pipeline (step 3c)");
}

static void try_build_compute_pipelines(VkDevice device) {
    if (g_pipe_built || !device || !g_cache_dir[0]) return;
    char man[600];
    snprintf(man, sizeof man, "%s/manifest_spirv.txt", g_cache_dir);
    FILE *f = fopen(man, "r");
    if (!f) {
        lsfg_log("W", "step3c: no %s; will retry", man);
        return;
    }
    lsfg_log("I", "=== LSFG step 3c: compute pipeline probe ===");
    VkPipelineLayoutCreateInfo lci{};
    lci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    VkResult lr = vkCreatePipelineLayout(device, &lci, nullptr, &g_pipe_layout);
    if (lr != VK_SUCCESS || !g_pipe_layout) {
        lsfg_log("W", "step3c: vkCreatePipelineLayout failed %d", (int)lr);
        fclose(f);
        return;
    }
    lsfg_log("I", "step3c: empty PipelineLayout OK %p", (void*)g_pipe_layout);
    int tried = 0;
    char line[256];
    while (fgets(line, sizeof line, f) && tried < 24) {
        unsigned id = 0;
        if (sscanf(line, "%u", &id) != 1) continue;
        if (strstr(line, " fail ") || strstr(line, " skip ")) continue;
        char path[700];
        snprintf(path, sizeof path, "%s/%u.spv", g_cache_dir, id);
        std::vector<uint32_t> code;
        if (!load_spv_file(path, code) || !spirv_is_compute(code)) continue;
        tried++;
        VkShaderModuleCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        sci.codeSize = code.size() * sizeof(uint32_t);
        sci.pCode = code.data();
        VkShaderModule mod = VK_NULL_HANDLE;
        if (vkCreateShaderModule(device, &sci, nullptr, &mod) != VK_SUCCESS || !mod) {
            g_compute_fail++; continue;
        }
        VkComputePipelineCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pci.stage.module = mod;
        pci.stage.pName = "main";
        pci.layout = g_pipe_layout;
        VkPipeline pipe = VK_NULL_HANDLE;
        VkResult pr = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pci, nullptr, &pipe);
        if (pr == VK_SUCCESS && pipe) {
            g_compute_ok++;
            lsfg_log("I", "step3c: id=%u pipeline creation probe OK (not dispatched)", id);
            vkDestroyPipeline(device, pipe, nullptr);
        } else {
            g_compute_fail++;
            lsfg_log("W", "step3c: id=%u FAIL %d", id, (int)pr);
        }
        vkDestroyShaderModule(device, mod, nullptr);
    }
    fclose(f);
    g_pipe_built = 1;
    lsfg_log("I", "step3c probe done: tried=%d pipeline_create_ok=%d fail=%d; all probe pipelines destroyed, no dispatch/output", tried, g_compute_ok, g_compute_fail);
}

void lsfg_host_on_present(uint32_t width, uint32_t height, uint32_t vk_format) {
    lsfg_host_on_present_ex(width, height, vk_format, nullptr);
}

void lsfg_host_on_present_ex(uint32_t width, uint32_t height, uint32_t vk_format, void *vk_device) {
    if (!lsfg_host_active()) return;
    g_present_n++;
    VkDevice dev = vk_device ? reinterpret_cast<VkDevice>(vk_device) : g_vk_device;
    if (!g_present_logged_init) {
        g_present_logged_init = 1;
        lsfg_log("I", "=== LSFG step 3: present hook LIVE ===");
        lsfg_log("I", "present: first frame %ux%u vkfmt=0x%x spirv_modules=%d device=%p",
                 width, height, vk_format, g_spirv_count, (void*)dev);
        lsfg_log("I", "STATUS: hook receives frames; dimensions from SDL window");
    }
    if (dev) {
        // step3d allocates the external frame-history images only. Shader
        // creation, compute dispatch and image hazards are handled by DXVK's
        // active command list from the step3g/step3j callbacks.
        if (false) {
            validate_spirv_on_device(dev);
            try_build_compute_pipelines(dev);
        }
        lsfg_step3d_run(dev, g_cache_dir, width, height);
    }
    if (g_present_n == 60 || g_present_n == 300 || (g_present_n % 1800) == 0) {
        lsfg_log("I", "present: frame #%u %ux%u history_images_ready=%d compute_path=DXVK-command-list",
                 g_present_n, width, height, lsfg_step3d_frames_ready());
    }
}
