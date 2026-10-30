#include "lsfg_log.h"
#include "lsfg_host.h"

#include <atomic>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <vector>

#include "dxvk_context.h"
#include "dxvk_device.h"
#include "dxvk_image.h"
#include "dxvk_buffer.h"
#include "dxvk_sampler.h"
#include "lsfg_dxvk_shader.h"

extern "C" int  lsfg_step3d_frames_ready(void);
extern "C" void *lsfg_step3d_get_image(int idx);
extern "C" unsigned lsfg_step3d_fb_width(void);
extern "C" unsigned lsfg_step3d_fb_height(void);
extern "C" unsigned lsfg_step3d_fb_generation(void);
extern "C" const char *lsfg_host_cache_dir(void);

using namespace dxvk;

static std::atomic<unsigned> g_copy_n{0};
static int g_pp;
static int g_logged;
static unsigned g_history_mask;
static std::atomic<unsigned> g_slot_copies[2]{};
static std::atomic<unsigned> g_slot_completed[2]{};
static std::atomic<unsigned> g_copy_completed{0};
static std::atomic<unsigned> g_copy_failed{0};
static Rc<DxvkImage> g_hist_wrap[2];
static Rc<DxvkImageView> g_hist_view[2];
static Rc<DxvkShader> g_mipmap_shader;
static Rc<DxvkSampler> g_mipmap_sampler;
static Rc<DxvkBuffer> g_mipmap_ubo;
static Rc<DxvkImage> g_flow_images[7];
static Rc<DxvkImageView> g_flow_views[7];
static DxvkDevice* g_mipmap_device;
static VkExtent2D g_mipmap_extent{};
static VkExtent2D g_flow_extent{};
static unsigned g_mipmap_generation;
static float g_mipmap_scale;
static bool g_mipmap_ready;
static bool g_mipmap_error_logged;
static char g_mipmap_cache_dir[512];
static VkImage g_hist_handle[2]{};
static DxvkDevice* g_wrap_device;
static unsigned g_wrap_generation;


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
static_assert(sizeof(LsfgConstants) == 48, "LSFG constant buffer layout");

static bool load_spirv(const char *path, std::vector<uint32_t>& code) {
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size < 20 || (size & 3)) { fclose(file); return false; }
    code.resize(size_t(size) / sizeof(uint32_t));
    bool ok = fread(code.data(), 1, size_t(size), file) == size_t(size);
    fclose(file);
    return ok && code[0] == 0x07230203u;
}

static void clear_mipmap_resources() {
    g_mipmap_shader = Rc<DxvkShader>();
    g_mipmap_sampler = Rc<DxvkSampler>();
    g_mipmap_ubo = Rc<DxvkBuffer>();
    for (unsigned i = 0; i < 7; i++) {
        g_flow_views[i] = Rc<DxvkImageView>();
        g_flow_images[i] = Rc<DxvkImage>();
    }
    g_mipmap_ready = false;
}

static void reset_mipmap_resources() {
    clear_mipmap_resources();
    g_mipmap_device = nullptr;
    g_mipmap_error_logged = false;
    g_mipmap_cache_dir[0] = 0;
}

static bool ensure_mipmap_resources(DxvkDevice *device, const char *cache_dir,
                                    uint32_t width, uint32_t height, unsigned generation) {
    const float scale = lsfg_host_flow_scale();
    if (g_mipmap_device != device || g_mipmap_generation != generation
        || g_mipmap_extent.width != width || g_mipmap_extent.height != height
        || g_mipmap_scale != scale
        || strncmp(g_mipmap_cache_dir, cache_dir ? cache_dir : "", sizeof(g_mipmap_cache_dir)) != 0) {
        reset_mipmap_resources();
        g_mipmap_device = device;
        g_mipmap_generation = generation;
        g_mipmap_extent = { width, height };
        g_mipmap_scale = scale;
        snprintf(g_mipmap_cache_dir, sizeof(g_mipmap_cache_dir), "%s", cache_dir ? cache_dir : "");
    }
    if (g_mipmap_ready) return true;
    if (!device || !cache_dir || !cache_dir[0] || width == 0 || height == 0
        || scale <= 0.0f || scale > 1.0f) return false;

    char shader_path[768];
    snprintf(shader_path, sizeof(shader_path), "%s/255.spv", cache_dir);
    std::vector<uint32_t> code;
    std::string error;
    if (!load_spirv(shader_path, code)) {
        error = "unable to load mipmaps SPIR-V";
    } else {
        g_mipmap_shader = createLsfgComputeShader(device, code, error);
    }
    if (g_mipmap_shader == nullptr && error.empty())
        error = "DXVK compute shader creation returned null";

    if (g_mipmap_shader != nullptr) {
        DxvkSamplerCreateInfo sampler_info{};
        sampler_info.magFilter = VK_FILTER_LINEAR;
        sampler_info.minFilter = VK_FILTER_LINEAR;
        sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sampler_info.mipmapLodBias = 0.0f;
        sampler_info.mipmapLodMin = 0.0f;
        sampler_info.mipmapLodMax = 0.0f;
        sampler_info.useAnisotropy = VK_FALSE;
        sampler_info.maxAnisotropy = 1.0f;
        sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        sampler_info.compareToDepth = VK_FALSE;
        sampler_info.compareOp = VK_COMPARE_OP_NEVER;
        sampler_info.borderColor.float32[0] = 0.0f;
        sampler_info.borderColor.float32[1] = 0.0f;
        sampler_info.borderColor.float32[2] = 0.0f;
        sampler_info.borderColor.float32[3] = 0.0f;
        sampler_info.usePixelCoord = VK_FALSE;
        g_mipmap_sampler = device->createSampler(sampler_info);
        if (g_mipmap_sampler == nullptr)
            error = "DXVK sampler creation failed";
    }

    if (error.empty()) {
        DxvkBufferCreateInfo buffer_info{};
        buffer_info.size = sizeof(LsfgConstants);
        buffer_info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        buffer_info.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        buffer_info.access = VK_ACCESS_SHADER_READ_BIT;
        g_mipmap_ubo = device->createBuffer(
            buffer_info, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (g_mipmap_ubo == nullptr || !g_mipmap_ubo->mapPtr(0)) {
            error = "DXVK host-visible uniform buffer creation failed";
        } else {
            LsfgConstants constants{};
            constants.resolution_inv_scale = 1.0f / scale;
            constants.ui_threshold = 0.5f;
            memcpy(g_mipmap_ubo->mapPtr(0), &constants, sizeof(constants));
        }
    }

    g_flow_extent = {
        std::max(1u, uint32_t(float(width) * scale)),
        std::max(1u, uint32_t(float(height) * scale)),
    };
    for (unsigned i = 0; i < 7 && error.empty(); i++) {
        DxvkImageCreateInfo image_info{};
        image_info.type = VK_IMAGE_TYPE_2D;
        image_info.format = VK_FORMAT_R8_UNORM;
        image_info.flags = 0;
        image_info.sampleCount = VK_SAMPLE_COUNT_1_BIT;
        image_info.extent = {
            std::max(1u, g_flow_extent.width >> i),
            std::max(1u, g_flow_extent.height >> i), 1 };
        image_info.numLayers = 1;
        image_info.mipLevels = 1;
        image_info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        image_info.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        image_info.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.layout = VK_IMAGE_LAYOUT_GENERAL;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        image_info.shared = VK_FALSE;
        g_flow_images[i] = device->createImage(image_info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (g_flow_images[i] == nullptr) {
            error = "DXVK flow image allocation failed";
            break;
        }

        DxvkImageViewCreateInfo view_info{};
        view_info.type = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = VK_FORMAT_R8_UNORM;
        view_info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        view_info.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.minLevel = 0;
        view_info.numLevels = 1;
        view_info.minLayer = 0;
        view_info.numLayers = 1;
        g_flow_views[i] = device->createImageView(g_flow_images[i], view_info);
        if (g_flow_views[i] == nullptr)
            error = "DXVK flow image view creation failed";
    }

    if (!error.empty()) {
        if (!g_mipmap_error_logged) {
            lsfg_log("E", "step3h: mipmaps resource creation failed: %s", error.c_str());
            g_mipmap_error_logged = true;
        }
        clear_mipmap_resources();
        return false;
    }

    g_mipmap_ready = true;
    lsfg_log("I", "step3h: mipmaps DXVK resources ready extent=%ux%u flow=%ux%u scale=%.3f",
             width, height, g_flow_extent.width, g_flow_extent.height, scale);
    return true;
}

static bool dispatch_mipmaps(DxvkContext *ctx, DxvkDevice *device,
                             const char *cache_dir, unsigned slot,
                             uint32_t width, uint32_t height, unsigned generation) {
    if (!g_hist_view[slot]) return false;
    if (!ensure_mipmap_resources(device, cache_dir, width, height, generation)) return false;

    ctx->bindResourceBuffer(0, DxvkBufferSlice(g_mipmap_ubo));
    ctx->bindResourceSampler(1, g_mipmap_sampler);
    ctx->bindResourceView(2, g_hist_view[slot], nullptr);
    for (unsigned i = 0; i < 7; i++)
        ctx->bindResourceView(3 + i, g_flow_views[i], nullptr);

    ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, g_mipmap_shader);
    const uint32_t groups_x = (g_flow_extent.width + 63u) >> 6;
    const uint32_t groups_y = (g_flow_extent.height + 63u) >> 6;
    ctx->dispatch(groups_x, groups_y, 1);
    return true;
}

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
        g_hist_view[0] = Rc<DxvkImageView>();
        g_hist_view[1] = Rc<DxvkImageView>();
        reset_mipmap_resources();
        g_hist_handle[0] = VK_NULL_HANDLE;
        g_hist_handle[1] = VK_NULL_HANDLE;
        g_wrap_device = device;
        g_wrap_generation = generation;
        g_history_mask = 0;
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
        DxvkImageViewCreateInfo history_view_info{};
        history_view_info.type = VK_IMAGE_VIEW_TYPE_2D;
        history_view_info.format = histFmt;
        history_view_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        history_view_info.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        history_view_info.minLevel = 0;
        history_view_info.numLevels = 1;
        history_view_info.minLayer = 0;
        history_view_info.numLayers = 1;
        g_hist_view[slot] = device->createImageView(g_hist_wrap[slot], history_view_info);
        if (g_hist_view[slot] == nullptr) {
            g_hist_wrap[slot] = Rc<DxvkImage>();
            lsfg_log("E", "step3g: failed to create DXVK history image view slot[%u]", slot);
            return;
        }
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

    const char *cache_dir = lsfg_host_cache_dir();
    if (dispatch_mipmaps(ctx, device, cache_dir, slot, w, h, generation)) {
        unsigned frame_index = unsigned(g_pp);
        if (frame_index < 2 || (frame_index % 300) == 0) {
            lsfg_log("I", "step3h: mipmaps dispatch frame=%u history_slot=%u groups=%ux%u",
                     frame_index, slot, (g_flow_extent.width + 63u) >> 6,
                     (g_flow_extent.height + 63u) >> 6);
        }
    }

    // The callback belongs to this DXVK command list. It runs only after the
    // command-list fence signals, before DXVK recycles its resources. This is
    // deliberately non-blocking and does not submit work to Vulkan directly.
    unsigned n = ++g_copy_n;
    ctx->addCompletionCallback([slot, n](VkResult status) {
        if (status == VK_SUCCESS) {
            unsigned completed = ++g_copy_completed;
            unsigned slotCompleted = ++g_slot_completed[slot];
            if (completed == 1 || completed == 60 || completed == 300
                || (completed % 1800) == 0) {
                lsfg_log("I", "step3g: GPU command-list completion #%u copy=%u slot[%u] slot_completed=%u",
                         completed, n, slot, slotCompleted);
            }
        } else {
            unsigned failed = ++g_copy_failed;
            lsfg_log("E", "step3g: GPU command-list completion failed copy=%u slot[%u] VkResult=%d failures=%u",
                     n, slot, (int)status, failed);
        }
    });

    // Completion counters above confirm fence retirement, not pixel values.
    // Pixel-content validation was performed separately by the earlier
    // readback diagnostic; no readback is scheduled on the present path.

    unsigned slot_n = ++g_slot_copies[slot];
    const unsigned slot_bit = 1u << slot;
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
