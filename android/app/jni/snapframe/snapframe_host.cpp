#include "snapframe_host.h"

#include <atomic>
#include <stdio.h>
#include <string.h>

#include "dxvk_context.h"
#include "dxvk_device.h"
#include "dxvk_image.h"

using namespace dxvk;

namespace {
std::atomic<int> g_initialized{0};
std::atomic<int> g_enabled{0};
std::atomic<uint64_t> g_capturedFrames{0};
std::atomic<uint64_t> g_skippedFrames{0};
std::atomic<int> g_resetHistoryRequested{0};

DxvkDevice* g_device = nullptr;
Rc<DxvkImage> g_history[2];
Rc<DxvkImageView> g_historyViews[2];
uint32_t g_width = 0;
uint32_t g_height = 0;
VkFormat g_format = VK_FORMAT_UNDEFINED;
uint64_t g_frameIndex = 0;
bool g_readyLogged = false;
bool g_skipReasonLogged = false;

void clearHistory() {
    g_historyViews[0] = Rc<DxvkImageView>();
    g_historyViews[1] = Rc<DxvkImageView>();
    g_history[0] = Rc<DxvkImage>();
    g_history[1] = Rc<DxvkImage>();
    g_device = nullptr;
    g_width = g_height = 0;
    g_format = VK_FORMAT_UNDEFINED;
    g_frameIndex = 0;
    g_readyLogged = false;
    g_skipReasonLogged = false;
}

bool createHistory(DxvkDevice* device, uint32_t width, uint32_t height, VkFormat format) {
    if (!device || !width || !height || format == VK_FORMAT_UNDEFINED)
        return false;

    for (unsigned i = 0; i < 2; ++i) {
        DxvkImageCreateInfo imageInfo{};
        imageInfo.type = VK_IMAGE_TYPE_2D;
        imageInfo.format = format;
        imageInfo.flags = 0;
        imageInfo.sampleCount = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.extent = { width, height, 1 };
        imageInfo.numLayers = 1;
        imageInfo.mipLevels = 1;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        imageInfo.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.layout = VK_IMAGE_LAYOUT_GENERAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.shared = VK_FALSE;

        g_history[i] = device->createImage(imageInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (g_history[i] == nullptr)
            return false;

        DxvkImageViewCreateInfo viewInfo{};
        viewInfo.type = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        viewInfo.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.minLevel = 0;
        viewInfo.numLevels = 1;
        viewInfo.minLayer = 0;
        viewInfo.numLayers = 1;
        g_historyViews[i] = device->createImageView(g_history[i], viewInfo);
        if (g_historyViews[i] == nullptr)
            return false;
    }

    g_device = device;
    g_width = width;
    g_height = height;
    g_format = format;
    g_frameIndex = 0;
    g_readyLogged = false;
    fprintf(stderr, "SnapFrame: allocated two-frame history %ux%u format=0x%x\n",
            width, height, static_cast<unsigned>(format));
    return true;
}

void snapframeCopyFrame(void* contextPtr, void* sourceViewPtr, void* devicePtr,
                        uint32_t width, uint32_t height, uint32_t formatValue) {
    if (!g_initialized.load(std::memory_order_acquire)
        || !g_enabled.load(std::memory_order_acquire))
        return;
    if (g_resetHistoryRequested.exchange(0, std::memory_order_acq_rel))
        clearHistory();

    auto* context = reinterpret_cast<DxvkContext*>(contextPtr);
    auto* sourceView = reinterpret_cast<DxvkImageView*>(sourceViewPtr);
    auto* device = reinterpret_cast<DxvkDevice*>(devicePtr);
    if (!context || !sourceView || !device || !width || !height) {
        g_skippedFrames.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    Rc<DxvkImage> sourceImage = sourceView->image();
    const char* skipReason = nullptr;
    if (sourceImage == nullptr)
        skipReason = "source image wrapper is null";
    else if (!(sourceImage->info().usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
        skipReason = "swapchain image lacks TRANSFER_SRC usage";
    else if (sourceImage->info().extent.width != width
          || sourceImage->info().extent.height != height)
        skipReason = "source extent mismatch";
    else if (sourceImage->info().format != static_cast<VkFormat>(formatValue))
        skipReason = "source format mismatch";
    if (skipReason) {
        g_skippedFrames.fetch_add(1, std::memory_order_relaxed);
        if (!g_skipReasonLogged) {
            g_skipReasonLogged = true;
            fprintf(stderr, "SnapFrame: capture skipped: %s\n", skipReason);
        }
        return;
    }

    const VkFormat format = sourceImage->info().format;
    if (g_device != device || g_width != width || g_height != height || g_format != format) {
        clearHistory();
        if (!createHistory(device, width, height, format)) {
            clearHistory();
            g_skippedFrames.fetch_add(1, std::memory_order_relaxed);
            fprintf(stderr, "SnapFrame: history allocation failed for %ux%u format=0x%x\n",
                    width, height, static_cast<unsigned>(format));
            return;
        }
    }

    const unsigned slot = static_cast<unsigned>(g_frameIndex & 1u);
    VkImageSubresourceLayers subresource{};
    subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    subresource.mipLevel = 0;
    subresource.baseArrayLayer = 0;
    subresource.layerCount = 1;

    context->copyImage(
        g_history[slot], subresource, VkOffset3D{ 0, 0, 0 },
        sourceImage, subresource, VkOffset3D{ 0, 0, 0 },
        VkExtent3D{ width, height, 1 });

    ++g_frameIndex;
    const uint64_t captured = g_capturedFrames.fetch_add(1, std::memory_order_relaxed) + 1;
    if (g_frameIndex >= 2 && !g_readyLogged) {
        g_readyLogged = true;
        fprintf(stderr, "SnapFrame: two real frames captured; history ready for compute prototype\n");
    }
    if (captured == 1 || captured == 60 || (captured % 1800) == 0)
        fprintf(stderr, "SnapFrame: captured=%llu skipped=%llu extent=%ux%u\n",
                static_cast<unsigned long long>(captured),
                static_cast<unsigned long long>(g_skippedFrames.load(std::memory_order_relaxed)),
                width, height);
}

void snapframeDeviceDestroyed(void* devicePtr) {
    if (g_device == reinterpret_cast<DxvkDevice*>(devicePtr))
        clearHistory();
}
}

extern "C" void snapframe_dxvk_set_copy_hook(
    void (*hook)(void*, void*, void*, uint32_t, uint32_t, uint32_t));
extern "C" void snapframe_dxvk_set_device_destroy_hook(void (*hook)(void*));

extern "C" void snapframe_config_defaults(SnapFrameConfig *config) {
    if (!config) return;
    memset(config, 0, sizeof(*config));
    config->enabled = 0;
    config->multiplier = 2;
    config->quality = SNAPFRAME_QUALITY_BALANCED;
    config->flow_scale_percent = 50;
    config->profiling_enabled = 1;
}

extern "C" int snapframe_host_init(const SnapFrameConfig *config) {
    if (!config) return 0;

    SnapFrameConfig validated = *config;
    if (validated.multiplier < 1 || validated.multiplier > 2)
        validated.multiplier = 2;
    if (validated.quality < SNAPFRAME_QUALITY_PERFORMANCE
        || validated.quality > SNAPFRAME_QUALITY_QUALITY)
        validated.quality = SNAPFRAME_QUALITY_BALANCED;
    if (validated.flow_scale_percent < 25)
        validated.flow_scale_percent = 25;
    if (validated.flow_scale_percent > 100)
        validated.flow_scale_percent = 100;

    g_enabled.store(validated.enabled ? 1 : 0, std::memory_order_release);
    g_capturedFrames.store(0, std::memory_order_relaxed);
    g_skippedFrames.store(0, std::memory_order_relaxed);
    g_resetHistoryRequested.store(1, std::memory_order_release);
    g_initialized.store(1, std::memory_order_release);

    snapframe_dxvk_set_copy_hook(snapframeCopyFrame);
    snapframe_dxvk_set_device_destroy_hook(snapframeDeviceDestroyed);
    fprintf(stderr, "SnapFrame: host initialized enabled=%d quality=%d flow_scale=%d%% (capture prototype only)\n",
            validated.enabled, validated.quality, validated.flow_scale_percent);
    return 1;
}

extern "C" void snapframe_host_shutdown(void) {
    g_enabled.store(0, std::memory_order_release);
    g_initialized.store(0, std::memory_order_release);
    g_resetHistoryRequested.store(1, std::memory_order_release);
    snapframe_dxvk_set_copy_hook(nullptr);
    /* Keep the device-destroy hook registered so owned images are released
     * before DXVK tears down the device allocator. */
}

extern "C" int snapframe_host_active(void) {
    return g_initialized.load(std::memory_order_acquire)
        && g_enabled.load(std::memory_order_acquire);
}

extern "C" int snapframe_host_set_enabled(int enabled) {
    if (!g_initialized.load(std::memory_order_acquire)) return 0;
    g_enabled.store(enabled ? 1 : 0, std::memory_order_release);
    g_resetHistoryRequested.store(1, std::memory_order_release);
    return 1;
}

extern "C" void snapframe_host_get_stats(SnapFrameStats *stats) {
    if (!stats) return;
    memset(stats, 0, sizeof(*stats));
    stats->captured_frames = g_capturedFrames.load(std::memory_order_relaxed);
    stats->skipped_frames = g_skippedFrames.load(std::memory_order_relaxed);
}
