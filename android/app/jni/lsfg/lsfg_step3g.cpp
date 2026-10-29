#include "lsfg_log.h"
#include "lsfg_host.h"

#include <atomic>
#include <stdint.h>

#include "dxvk_context.h"
#include "dxvk_device.h"
#include "dxvk_image.h"

extern "C" int  lsfg_step3d_frames_ready(void);
extern "C" void *lsfg_step3d_get_image(int idx);
extern "C" unsigned lsfg_step3d_fb_width(void);
extern "C" unsigned lsfg_step3d_fb_height(void);
extern "C" unsigned lsfg_step3d_fb_generation(void);

using namespace dxvk;

static std::atomic<unsigned> g_copy_n{0};
static int g_pp;
static int g_logged;
static unsigned g_history_mask;
static std::atomic<unsigned> g_slot_copies[2]{};
static Rc<DxvkImage> g_hist_wrap[2];
static VkImage g_hist_handle[2]{};
static DxvkDevice* g_wrap_device;
static unsigned g_wrap_generation;
static unsigned g_readback_mask;

extern "C" void lsfg_step3g_copy_impl(
    void *ctx_v, void *src_view_v, void *device_v,
    uint32_t w, uint32_t h, uint32_t vkfmt) {

    if (!lsfg_host_active()) return;
    if (!ctx_v || !src_view_v || !device_v || !w || !h) return;
    if (!lsfg_step3d_frames_ready()) return;

    auto *ctx     = reinterpret_cast<DxvkContext*>(ctx_v);
    auto *srcView = reinterpret_cast<DxvkImageView*>(src_view_v);
    auto *device  = reinterpret_cast<DxvkDevice*>(device_v);

    unsigned slot = (unsigned)(g_pp & 1);
    unsigned fbw = lsfg_step3d_fb_width();
    unsigned fbh = lsfg_step3d_fb_height();
    if (w != fbw || h != fbh) {
        if (!g_logged) {
            g_logged = 1;
            lsfg_log("W", "step3g: extent mismatch present=%ux%u history=%ux%u; skip copy", w, h, fbw, fbh);
        }
        return;
    }
    VkImage histVk = reinterpret_cast<VkImage>(lsfg_step3d_get_image((int)slot));
    if (!histVk) return;

    Rc<DxvkImage> srcImg = srcView->image();
    if (srcImg == nullptr) return;

    VkFormat histFmt = VK_FORMAT_R8G8B8A8_UNORM;
    if (srcImg->info().extent.width != w || srcImg->info().extent.height != h) {
        if (!g_logged) {
            g_logged = 1;
            lsfg_log("W", "step3g: source extent mismatch image=%ux%u args=%ux%u; skip copy",
                     srcImg->info().extent.width, srcImg->info().extent.height, w, h);
        }
        return;
    }
    if (srcImg->info().format != histFmt) {
        if (!g_logged) {
            g_logged = 1;
            lsfg_log("W", "step3g: format mismatch src=0x%x hist=0x%x \u2014 skip copy",
                     (unsigned)srcImg->info().format, (unsigned)histFmt);
        }
        return;
    }

    unsigned generation = lsfg_step3d_fb_generation();
    if (g_wrap_device != device || g_wrap_generation != generation) {
        g_hist_wrap[0] = Rc<DxvkImage>();
        g_hist_wrap[1] = Rc<DxvkImage>();
        g_hist_handle[0] = VK_NULL_HANDLE;
        g_hist_handle[1] = VK_NULL_HANDLE;
        g_wrap_device = device;
        g_wrap_generation = generation;
        g_history_mask = 0;
        g_readback_mask = 0;
        g_slot_copies[0] = 0;
        g_slot_copies[1] = 0;
        lsfg_log("I", "step3g: reset persistent history wrappers generation=%u", generation);
    }

    if (g_hist_wrap[slot] == nullptr || g_hist_handle[slot] != histVk) {
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

        g_hist_wrap[slot] = new DxvkImage(device->vkd(), ii, histVk);
        g_hist_handle[slot] = histVk;
        lsfg_log("I", "step3g: created persistent wrapper slot[%u] image=%p generation=%u",
                 slot, (void*)histVk, generation);
    }
    Rc<DxvkImage> dstImg = g_hist_wrap[slot];

    VkImageSubresourceLayers sub{};
    sub.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    sub.mipLevel       = 0;
    sub.baseArrayLayer = 0;
    sub.layerCount     = 1;

    ctx->copyImage(
        dstImg, sub, VkOffset3D{ 0, 0, 0 },
        srcImg, sub, VkOffset3D{ 0, 0, 0 },
        VkExtent3D{ w, h, 1 });

    // One 2x2 RGBA readback per history slot/generation. The readback copy is
    // recorded in this same DXVK command list and inspected only after its fence.
    unsigned slot_bit = 1u << slot;
    if (!(g_readback_mask & slot_bit)) {
        DxvkBufferCreateInfo bi{};
        bi.size   = 16;
        bi.usage  = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bi.stages = VK_PIPELINE_STAGE_HOST_BIT;
        bi.access = VK_ACCESS_HOST_READ_BIT;

        Rc<DxvkBuffer> readback = device->createBuffer(bi,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        VkMemoryPropertyFlags required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                       | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if (readback != nullptr && (readback->memFlags() & required) == required) {
            ctx->copyImageToBuffer(
                readback, 0, 4, 4, dstImg, sub,
                VkOffset3D{ 0, 0, 0 }, VkExtent3D{ 2, 2, 1 });

            ctx->addCompletionCallback([readback, slot, generation](VkResult status) {
                if (status != VK_SUCCESS) {
                    lsfg_log("W", "step3g: history[%u] readback completion failed status=%d",
                             slot, (int)status);
                    return;
                }

                const uint8_t* p = static_cast<const uint8_t*>(readback->mapPtr(0));
                uint32_t checksum = 0;
                for (unsigned i = 0; i < 16; i++)
                    checksum += p[i];

                lsfg_log("I", "step3g: GPU readback slot[%u] gen=%u checksum=%u "
                         "p0=%u,%u,%u,%u p1=%u,%u,%u,%u "
                         "p2=%u,%u,%u,%u p3=%u,%u,%u,%u",
                         slot, generation, checksum,
                         (unsigned)p[0], (unsigned)p[1], (unsigned)p[2], (unsigned)p[3],
                         (unsigned)p[4], (unsigned)p[5], (unsigned)p[6], (unsigned)p[7],
                         (unsigned)p[8], (unsigned)p[9], (unsigned)p[10], (unsigned)p[11],
                         (unsigned)p[12], (unsigned)p[13], (unsigned)p[14], (unsigned)p[15]);
            });
            g_readback_mask |= slot_bit;
            lsfg_log("I", "step3g: scheduled 2x2 GPU readback for history[%u]", slot);
        } else {
            g_readback_mask |= slot_bit;
            lsfg_log("W", "step3g: readback skipped for history[%u]: host-coherent memory unavailable", slot);
        }
    }

    unsigned n = ++g_copy_n;
    unsigned slot_n = ++g_slot_copies[slot];
    if (!(g_history_mask & slot_bit)) {
        g_history_mask |= slot_bit;
        lsfg_log("I", "step3g: first copy command recorded for history[%u]; mask=0x%x",
                 slot, g_history_mask);
        if (g_history_mask == 0x3u)
            lsfg_log("I", "step3g: both history slots have copy commands recorded (GPU pixel contents not yet read back)");
    }
    if (!g_logged) {
        g_logged = 1;
        lsfg_log("I", "=== LSFG step 3g-a: DXVK cmd-stream copy LIVE ===");
        lsfg_log("I", "step3g: first copy %ux%u fmt=0x%x -> hist[%d]=%p",
                 w, h, vkfmt, g_pp & 1, (void*)histVk);
    }
    if (n == 1 || n == 60 || n == 300 || (n % 1800) == 0 || slot_n == 1) {
        lsfg_log("I", "step3g: copy #%u slot[%u] slot_copy=%u %ux%u fmt=0x%x (command recorded)",
                 n, slot, slot_n, w, h, vkfmt);
    }
    g_pp++;
}
