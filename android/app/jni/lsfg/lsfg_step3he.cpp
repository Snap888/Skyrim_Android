/* Step 3h-e: allocate LSFG intermediate image pyramid (lsfg-vk layout).
 * One-time GPU allocations after probe — NO per-frame dispatch (FPS safe).
 */
#include "lsfg_log.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#ifdef __ANDROID__
#include <vulkan/vulkan.h>
#else
#include <vulkan/vulkan_core.h>
#endif

extern "C" int  lsfg_step3hc_chain_ready(void);
extern "C" unsigned lsfg_step3d_fb_width(void);
extern "C" unsigned lsfg_step3d_fb_height(void);
extern "C" int lsfg_role_perf_mode(void);
extern "C" void lsfg_step3hf_after_pyramid(void *device, unsigned qfam);
extern "C" unsigned lsfg_step3e_queue_family(void);

static VkDevice g_dev = VK_NULL_HANDLE;
static VkPhysicalDevice g_phys = VK_NULL_HANDLE;
static int g_ready;
static int g_logged;

static VkImage g_mip[7];
static VkDeviceMemory g_mip_mem[7];
static VkImageView g_mip_view[7];
static uint32_t g_mip_w[7], g_mip_h[7];

enum { FLOW_POOL = 12 };
static VkImage g_flow[FLOW_POOL];
static VkDeviceMemory g_flow_mem[FLOW_POOL];
static VkImageView g_flow_view[FLOW_POOL];

enum { GEN_OUT = 3 };
static VkImage g_gen[GEN_OUT];
static VkDeviceMemory g_gen_mem[GEN_OUT];
static VkImageView g_gen_view[GEN_OUT];

static uint32_t g_fw, g_fh, g_w, g_h;
static int g_mip_ok, g_flow_ok, g_gen_ok;

static uint32_t find_mem(VkPhysicalDevice phys, uint32_t bits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props)
            return i;
    return 0;
}

static bool create_img(VkDevice dev, VkPhysicalDevice phys,
                       uint32_t w, uint32_t h, VkFormat fmt,
                       VkImage *img, VkDeviceMemory *mem, VkImageView *view) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
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
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev, &ici, nullptr, img) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, *img, &mr);
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = find_mem(phys, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(dev, &mai, nullptr, mem) != VK_SUCCESS) return false;
    if (vkBindImageMemory(dev, *img, *mem, 0) != VK_SUCCESS) return false;
    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = *img;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = fmt;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    return vkCreateImageView(dev, &vci, nullptr, view) == VK_SUCCESS;
}

static bool build_pool(VkDevice dev, VkPhysicalDevice phys) {
    if (g_ready) return true;
    if (!dev || !phys) return false;

    g_w = lsfg_step3d_fb_width();
    g_h = lsfg_step3d_fb_height();
    if (g_w < 16) g_w = 16;
    if (g_h < 16) g_h = 16;
    g_fw = (g_w * 70) / 100;
    g_fh = (g_h * 70) / 100;
    if (g_fw < 8) g_fw = 8;
    if (g_fh < 8) g_fh = 8;

    lsfg_log("I", "=== LSFG step 3h-e: intermediate pyramid (one-time alloc) ===");
    lsfg_log("I", "step3h-e: full=%ux%u flow=%ux%u model=%s",
             g_w, g_h, g_fw, g_fh, lsfg_role_perf_mode() ? "3.1P" : "3.1");

    g_mip_ok = 0;
    for (int i = 0; i < 7; i++) {
        uint32_t mw = g_fw >> i;
        uint32_t mh = g_fh >> i;
        if (mw < 1) mw = 1;
        if (mh < 1) mh = 1;
        g_mip_w[i] = mw;
        g_mip_h[i] = mh;
        if (create_img(dev, phys, mw, mh, VK_FORMAT_R8_UNORM,
                       &g_mip[i], &g_mip_mem[i], &g_mip_view[i]))
            g_mip_ok++;
    }

    g_flow_ok = 0;
    for (int i = 0; i < FLOW_POOL; i++) {
        uint32_t ww = (i < 4) ? g_fw : (g_fw > 1 ? g_fw / 2 : 1);
        uint32_t hh = (i < 4) ? g_fh : (g_fh > 1 ? g_fh / 2 : 1);
        if (create_img(dev, phys, ww, hh, VK_FORMAT_R16G16B16A16_SFLOAT,
                       &g_flow[i], &g_flow_mem[i], &g_flow_view[i]))
            g_flow_ok++;
    }

    g_gen_ok = 0;
    for (int i = 0; i < GEN_OUT; i++) {
        if (create_img(dev, phys, g_w, g_h, VK_FORMAT_R8G8B8A8_UNORM,
                       &g_gen[i], &g_gen_mem[i], &g_gen_view[i]))
            g_gen_ok++;
    }

    lsfg_log("I", "step3h-e: mips %d/7 (L0 %ux%u .. L6 %ux%u)",
             g_mip_ok, g_mip_w[0], g_mip_h[0], g_mip_w[6], g_mip_h[6]);
    lsfg_log("I", "step3h-e: flow pool %d/%d (RGBA16F)", g_flow_ok, FLOW_POOL);
    lsfg_log("I", "step3h-e: generate outs %d/%d full-res", g_gen_ok, GEN_OUT);

    g_ready = (g_mip_ok >= 7 && g_flow_ok >= 8 && g_gen_ok >= 1) ? 1 : 0;
    if (g_ready) {
        lsfg_log("I", "step3h-e: PYRAMID READY (topology matches lsfg-vk Context)");
        lsfg_log("I", "NEXT step 3h-f: multi-pass dispatch on SECONDARY cmd buffer + present inject");
        lsfg_log("I", "NOTE: still no per-frame FG — pyramid is idle until 3h-f");
    } else {
        lsfg_log("W", "step3h-e: PYRAMID INCOMPLETE mip=%d flow=%d gen=%d",
                 g_mip_ok, g_flow_ok, g_gen_ok);
    }
    return g_ready != 0;
}

extern "C" void lsfg_step3he_set_phys(void *phys) {
    g_phys = (VkPhysicalDevice)phys;
}

extern "C" int lsfg_step3he_pyramid_ready(void) { return g_ready; }

extern "C" void *lsfg_step3he_get_gen_image(int idx) {
    if (!g_ready || idx < 0 || idx >= GEN_OUT) return nullptr;
    return (void*)g_gen[idx];
}

extern "C" void *lsfg_step3he_get_gen_view(int idx) {
    if (!g_ready || idx < 0 || idx >= GEN_OUT) return nullptr;
    return (void*)g_gen_view[idx];
}

extern "C" void *lsfg_step3he_get_mip_view(int idx) {
    if (!g_ready || idx < 0 || idx >= 7) return nullptr;
    return (void*)g_mip_view[idx];
}

extern "C" void *lsfg_step3he_get_flow_view(int idx) {
    if (!g_ready || idx < 0 || idx >= FLOW_POOL) return nullptr;
    return (void*)g_flow_view[idx];
}

extern "C" void lsfg_step3he_after_probe(void *device) {
    if (g_logged) return;
    if (!device || !g_phys) {
        lsfg_log("W", "step3h-e: missing device/phys");
        return;
    }
    if (!lsfg_step3hc_chain_ready()) return;
    g_dev = (VkDevice)device;
    g_logged = 1;
    if (build_pool(g_dev, g_phys)) {
        lsfg_step3hf_after_pyramid((void*)g_dev, lsfg_step3e_queue_family());
    }
}

extern "C" void lsfg_step3he_install(void) {
    lsfg_log("I", "LSFG: step3h-e intermediate pyramid installer ready");
}
