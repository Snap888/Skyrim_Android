/* Step 3h-b: build LSFG pass graph structure
 *   mipmaps -> alpha/beta/gamma/delta (flow) -> generate
 * Loads SPIR-V for each role id, creates compute pipelines, allocates intermediate
 * images at flow_scale. Full correct descriptor wiring is 3h-c; here we prove
 * the graph object graph can be built on device.
 */
#include "lsfg_log.h"
#include "lsfg_host.h"

#include <stdio.h>
#include <string.h>
#include <vector>
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
extern "C" int lsfg_roles_ready(void);
extern "C" int lsfg_role_perf_mode(void);
extern "C" uint32_t lsfg_role_generate_id(void);
extern "C" uint32_t lsfg_role_mipmaps_id(void);

struct PassDef {
    const char *name;
    uint32_t id;
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

struct PassPipe {
    const char *name;
    uint32_t id;
    VkShaderModule sm;
    VkPipeline pipe;
    int ok;
};

static VkDevice g_dev = VK_NULL_HANDLE;
static uint32_t g_qfam;
static VkPipelineLayout g_empty_pl = VK_NULL_HANDLE;
static std::vector<PassPipe> g_passes;
static int g_graph_ready;
static int g_logged;

static VkImage g_flow_img[4] = {};
static VkDeviceMemory g_flow_mem[4] = {};
static VkImageView g_flow_view[4] = {};
static VkImage g_out_img = VK_NULL_HANDLE;
static VkDeviceMemory g_out_mem = VK_NULL_HANDLE;
static VkImageView g_out_view = VK_NULL_HANDLE;
static uint32_t g_fw, g_fh;
static VkPhysicalDevice g_phys = VK_NULL_HANDLE;

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
    for (size_t i = 5; i < c.size();) {
        uint32_t w = c[i], op = w & 0xffff, len = w >> 16;
        if (!len) break;
        if (op == 15 && i + 1 < c.size() && c[i + 1] == 5) return true;
        i += len;
    }
    return false;
}

static uint32_t find_mem_type(VkPhysicalDevice phys, uint32_t bits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props)
            return i;
    return 0;
}

static bool create_img(VkDevice dev, VkPhysicalDevice phys,
                       uint32_t w, uint32_t h, VkFormat fmt,
                       VkImage *out_img, VkDeviceMemory *out_mem, VkImageView *out_view) {
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = fmt;
    ici.extent = { w, h, 1 };
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
              | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev, &ici, nullptr, out_img) != VK_SUCCESS) return false;

    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, *out_img, &mr);
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = find_mem_type(phys, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(dev, &mai, nullptr, out_mem) != VK_SUCCESS) return false;
    if (vkBindImageMemory(dev, *out_img, *out_mem, 0) != VK_SUCCESS) return false;

    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = *out_img;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = fmt;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    if (vkCreateImageView(dev, &vci, nullptr, out_view) != VK_SUCCESS) return false;
    return true;
}

static bool build_graph(VkDevice dev, VkPhysicalDevice phys, unsigned qfam) {
    if (g_graph_ready) return true;
    if (!lsfg_roles_ready()) {
        lsfg_log("W", "step3h-b: roles not pinned");
        return false;
    }
    const char *cache = lsfg_host_cache_dir();
    if (!cache || !*cache) return false;

    int perf = lsfg_role_perf_mode();
    const PassDef *defs = perf ? k_perf : k_quality;
    size_t ndefs = perf ? (sizeof(k_perf)/sizeof(k_perf[0])) : (sizeof(k_quality)/sizeof(k_quality[0]));

    lsfg_log("I", "=== LSFG step 3h-b: build pass graph (%s, %zu stages) ===",
             perf ? "3.1P" : "3.1", ndefs);

    if (!g_empty_pl) {
        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        if (vkCreatePipelineLayout(dev, &plci, nullptr, &g_empty_pl) != VK_SUCCESS) {
            lsfg_log("W", "step3h-b: empty PipelineLayout fail");
            return false;
        }
    }

    g_passes.clear();
    g_passes.reserve(ndefs);
    int ok = 0, fail = 0, skip = 0;

    for (size_t i = 0; i < ndefs; i++) {
        PassPipe pp{};
        pp.name = defs[i].name;
        pp.id = defs[i].id;
        pp.sm = VK_NULL_HANDLE;
        pp.pipe = VK_NULL_HANDLE;
        pp.ok = 0;

        char path[700];
        snprintf(path, sizeof path, "%s/%u.spv", cache, pp.id);
        std::vector<uint32_t> code;
        if (!load_spv(path, code)) {
            lsfg_log("W", "step3h-b: missing SPV id=%u (%s)", pp.id, pp.name);
            fail++;
            g_passes.push_back(pp);
            continue;
        }
        if (!is_compute(code)) {
            skip++;
            g_passes.push_back(pp);
            continue;
        }

        VkShaderModuleCreateInfo smci{};
        smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smci.codeSize = code.size() * 4;
        smci.pCode = code.data();
        if (vkCreateShaderModule(dev, &smci, nullptr, &pp.sm) != VK_SUCCESS) {
            fail++;
            g_passes.push_back(pp);
            continue;
        }

        VkComputePipelineCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pci.stage.module = pp.sm;
        pci.stage.pName = "main";
        pci.layout = g_empty_pl;
        if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pci, nullptr, &pp.pipe) != VK_SUCCESS) {
            lsfg_log("W", "step3h-b: pipeline id=%u (%s) need descriptors (deferred to 3h-c)",
                     pp.id, pp.name);
            pp.ok = 1;
            ok++;
            g_passes.push_back(pp);
            continue;
        }
        pp.ok = 2;
        ok++;
        g_passes.push_back(pp);
    }

    uint32_t w = lsfg_step3d_fb_width();
    uint32_t h = lsfg_step3d_fb_height();
    if (w < 16) w = 16;
    if (h < 16) h = 16;
    g_fw = (w * 70) / 100;
    g_fh = (h * 70) / 100;
    if (g_fw < 8) g_fw = 8;
    if (g_fh < 8) g_fh = 8;

    int inter_ok = 0;
    if (phys) {
        for (int i = 0; i < 4; i++) {
            if (create_img(dev, phys, g_fw, g_fh, VK_FORMAT_R16G16B16A16_SFLOAT,
                           &g_flow_img[i], &g_flow_mem[i], &g_flow_view[i]))
                inter_ok++;
        }
        if (create_img(dev, phys, w, h, VK_FORMAT_R8G8B8A8_UNORM,
                       &g_out_img, &g_out_mem, &g_out_view))
            inter_ok++;
    }

    int pipe_full = 0, mod_only = 0;
    for (auto &p : g_passes) {
        if (p.ok == 2) pipe_full++;
        else if (p.ok == 1) mod_only++;
    }

    lsfg_log("I", "step3h-b: stages=%zu modules=%d full_pipe=%d layout_deferred=%d fail=%d skip=%d",
             ndefs, ok, pipe_full, mod_only, fail, skip);
    lsfg_log("I", "step3h-b: intermediates flow=%ux%u imgs=%d/5 (4x flow + 1x out %ux%u)",
             g_fw, g_fh, inter_ok, w, h);
    lsfg_log("I", "step3h-b: graph order mipmaps -> alpha/beta/gamma/delta -> generate");
    lsfg_log("I", "step3h-b: generate_id=%u mipmaps_id=%u",
             lsfg_role_generate_id(), lsfg_role_mipmaps_id());

    int has_gen = 0, has_mip = 0;
    for (auto &p : g_passes) {
        if (p.id == lsfg_role_generate_id() && p.ok) has_gen = 1;
        if (p.id == lsfg_role_mipmaps_id() && p.ok) has_mip = 1;
    }
    g_graph_ready = (has_gen && has_mip && inter_ok >= 1) ? 1 : 0;

    if (g_graph_ready) {
        lsfg_log("I", "step3h-b: GRAPH READY (modules loaded + intermediates)");
        lsfg_log("I", "NEXT step 3h-c: per-pass descriptor layouts from SPIR-V + real dispatch chain");
    } else {
        lsfg_log("W", "step3h-b: GRAPH INCOMPLETE gen=%d mip=%d inter=%d", has_gen, has_mip, inter_ok);
    }

    (void)qfam;
    return g_graph_ready != 0;
}

extern "C" void lsfg_step3hb_set_phys(void *phys) {
    g_phys = (VkPhysicalDevice)phys;
}

extern "C" int lsfg_step3hb_graph_ready(void) { return g_graph_ready; }

extern "C" void lsfg_step3hb_after_copy(void *device, unsigned qfam) {
    if (!device) return;
    g_dev = (VkDevice)device;
    g_qfam = qfam;
    if (g_graph_ready) {
        if (!g_logged) {
            g_logged = 1;
            lsfg_log("I", "step3h-b: graph already ready (skip rebuild)");
        }
        return;
    }
    if (!lsfg_step3d_frames_ready()) return;
    if (!lsfg_step3d_get_image(0) || !lsfg_step3d_get_image(1)) return;

    build_graph(g_dev, g_phys, qfam);
}

extern "C" void lsfg_step3hb_install(void) {
    lsfg_log("I", "LSFG: step3h-b pass-graph builder installed");
}
