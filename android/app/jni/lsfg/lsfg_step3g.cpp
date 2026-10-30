#include "lsfg_log.h"
#include "lsfg_host.h"

#include <atomic>
#include <stdint.h>

#include "dxvk_context.h"
#include "dxvk_device.h"
#include "dxvk_image.h"

extern "C" int  lsfg_step3d_frames_ready(void);
extern "C" void *lsfg_step3d_get_image(int idx);
extern "C" void lsfg_set_step3g_copy_hook(void (*fn)(void*, void*, void*, uint32_t, uint32_t, uint32_t));
extern "C" void lsfg_set_step3hj_inject_hook(void (*fn)(void*, void*, void*, uint32_t, uint32_t, uint32_t));
extern "C" void lsfg_step3hj_on_frame(void *device, unsigned qfam);
extern "C" int  lsfg_step3hj_should_inject(void);
extern "C" void *lsfg_step3hj_gen_image(void);
extern "C" void lsfg_step3hj_note_inject(void);
extern "C" unsigned lsfg_step3e_queue_family(void);

using namespace dxvk;

static std::atomic<unsigned> g_copy_n{0};
static int g_pp;
static int g_logged;

static void lsfg_step3g_copy_impl(
    void *ctx_v, void *src_view_v, void *device_v,
    uint32_t w, uint32_t h, uint32_t vkfmt) {

    if (!lsfg_host_active()) return;
    if (!ctx_v || !src_view_v || !device_v || !w || !h) return;
    if (!lsfg_step3d_frames_ready()) return;

    auto *ctx     = reinterpret_cast<DxvkContext*>(ctx_v);
    auto *srcView = reinterpret_cast<DxvkImageView*>(src_view_v);
    auto *device  = reinterpret_cast<DxvkDevice*>(device_v);

    VkImage histVk = reinterpret_cast<VkImage>(lsfg_step3d_get_image(g_pp & 1));
    if (!histVk) return;

    Rc<DxvkImage> srcImg = srcView->image();
    if (srcImg == nullptr) return;

    VkFormat histFmt = VK_FORMAT_R8G8B8A8_UNORM;
    if (srcImg->info().format != histFmt) {
        if (!g_logged) {
            g_logged = 1;
            lsfg_log("W", "step3g: format mismatch src=0x%x hist=0x%x — skip copy",
                     (unsigned)srcImg->info().format, (unsigned)histFmt);
        }
        return;
    }

    DxvkImageCreateInfo ii{};
    ii.type        = VK_IMAGE_TYPE_2D;
    ii.format      = histFmt;
    ii.flags       = 0;
    ii.sampleCount = VK_SAMPLE_COUNT_1_BIT;
    ii.extent      = { w, h, 1 };
    ii.numLayers   = 1;
    ii.mipLevels   = 1;
    ii.usage       = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                   | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.stages      = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    ii.access      = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    ii.tiling      = VK_IMAGE_TILING_OPTIMAL;
    ii.layout      = VK_IMAGE_LAYOUT_GENERAL;
    ii.shared      = VK_TRUE;

    Rc<DxvkImage> dstImg = new DxvkImage(device->vkd(), ii, histVk);

    VkImageSubresourceLayers sub{};
    sub.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    sub.mipLevel       = 0;
    sub.baseArrayLayer = 0;
    sub.layerCount     = 1;

    ctx->copyImage(
        dstImg, sub, VkOffset3D{ 0, 0, 0 },
        srcImg, sub, VkOffset3D{ 0, 0, 0 },
        VkExtent3D{ w, h, 1 });

    unsigned n = ++g_copy_n;
    if (!g_logged) {
        g_logged = 1;
        lsfg_log("I", "=== LSFG step 3g-a: DXVK cmd-stream copy LIVE ===");
        lsfg_log("I", "step3g: first copy %ux%u fmt=0x%x -> hist[%d]=%p",
                 w, h, vkfmt, g_pp & 1, (void*)histVk);
    }
    if (n == 1 || n == 60 || n == 300 || (n % 1800) == 0) {
        lsfg_log("I", "step3g: copy #%u %ux%u -> hist[%d] (in DXVK present cmd list)",
                 n, w, h, g_pp & 1);
    }
    g_pp++;
    /* Best path: async full FG (3h-j) — no probe cascade / WaitIdle */
    lsfg_step3hj_on_frame(reinterpret_cast<void*>(device->handle()),
                          lsfg_step3e_queue_family());
}

/* Blit generated frame onto WSI image inside the same DXVK present cmd list */
extern "C" void lsfg_step3hj_insert_inject(
    void *ctx_v, void *wsi_view_v, void *device_v,
    uint32_t w, uint32_t h, uint32_t vkfmt) {

    if (!lsfg_step3hj_should_inject()) return;
    if (!ctx_v || !wsi_view_v || !device_v || !w || !h) return;

    VkImage genVk = reinterpret_cast<VkImage>(lsfg_step3hj_gen_image());
    if (!genVk) return;

    if (vkfmt != 0 && vkfmt != VK_FORMAT_R8G8B8A8_UNORM &&
        vkfmt != VK_FORMAT_R8G8B8A8_SRGB &&
        vkfmt != VK_FORMAT_B8G8R8A8_UNORM &&
        vkfmt != VK_FORMAT_B8G8R8A8_SRGB) {
        static int once;
        if (!once) {
            once = 1;
            lsfg_log("W", "step3h-j: inject skip — WSI fmt 0x%x not RGBA8", vkfmt);
        }
        return;
    }

    auto *ctx     = reinterpret_cast<DxvkContext*>(ctx_v);
    auto *wsiView = reinterpret_cast<DxvkImageView*>(wsi_view_v);
    auto *device  = reinterpret_cast<DxvkDevice*>(device_v);
    Rc<DxvkImage> dstImg = wsiView->image();
    if (dstImg == nullptr) return;

    DxvkImageCreateInfo ii{};
    ii.type        = VK_IMAGE_TYPE_2D;
    ii.format      = VK_FORMAT_R8G8B8A8_UNORM;
    ii.flags       = 0;
    ii.sampleCount = VK_SAMPLE_COUNT_1_BIT;
    ii.extent      = { w, h, 1 };
    ii.numLayers   = 1;
    ii.mipLevels   = 1;
    ii.usage       = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                   | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.stages      = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    ii.access      = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    ii.tiling      = VK_IMAGE_TILING_OPTIMAL;
    ii.layout      = VK_IMAGE_LAYOUT_GENERAL;
    ii.shared      = VK_TRUE;

    Rc<DxvkImage> srcImg = new DxvkImage(device->vkd(), ii, genVk);

    VkImageSubresourceLayers sub{};
    sub.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    sub.mipLevel       = 0;
    sub.baseArrayLayer = 0;
    sub.layerCount     = 1;

    uint32_t cw = w, ch = h;
    if (dstImg->info().extent.width < cw) cw = dstImg->info().extent.width;
    if (dstImg->info().extent.height < ch) ch = dstImg->info().extent.height;

    ctx->copyImage(
        dstImg, sub, VkOffset3D{ 0, 0, 0 },
        srcImg, sub, VkOffset3D{ 0, 0, 0 },
        VkExtent3D{ cw, ch, 1 });

    lsfg_step3hj_note_inject();
}

extern "C" void lsfg_step3g_install(void) {
    lsfg_set_step3g_copy_hook(lsfg_step3g_copy_impl);
    lsfg_set_step3hj_inject_hook(lsfg_step3hj_insert_inject);
    lsfg_log("I", "LSFG: step3g copy + 3h-j inject hooks installed");
}
