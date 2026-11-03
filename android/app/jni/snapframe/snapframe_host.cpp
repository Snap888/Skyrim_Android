#include "snapframe_host.h"

#include <atomic>
#include <mutex>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "dxvk_context.h"
#include "dxvk_device.h"
#include "dxvk_image.h"
#include "dxvk_sampler.h"
#include "dxvk_shader.h"
#include "spirv_code_buffer.h"

#if __has_include("generated/snapframe_shaders.h")
  #include "generated/snapframe_shaders.h"
  #define SNAPFRAME_HAS_LUMA_SHADER 1
#else
  #define SNAPFRAME_HAS_LUMA_SHADER 0
#endif

using namespace dxvk;

namespace {
std::mutex g_logMutex;
FILE* g_logFile = nullptr;

void snapframeLog(const char* format, ...) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    va_list args;
    va_start(args, format);
    va_list fileArgs;
    va_copy(fileArgs, args);
    vfprintf(stderr, format, args);
    fflush(stderr);
    if (g_logFile) {
        vfprintf(g_logFile, format, fileArgs);
        fflush(g_logFile);
    }
    va_end(fileArgs);
    va_end(args);
}

std::atomic<int> g_initialized{0};
std::atomic<int> g_enabled{0};
std::atomic<uint64_t> g_capturedFrames{0};
std::atomic<uint64_t> g_skippedFrames{0};
std::atomic<uint64_t> g_copyCallbackCalls{0};
std::atomic<int> g_resetHistoryRequested{0};

DxvkDevice* g_device = nullptr;
Rc<DxvkImage> g_history[2];
Rc<DxvkImageView> g_historyViews[2];
Rc<DxvkImage> g_lumaImage;
Rc<DxvkImageView> g_lumaView;
Rc<DxvkSampler> g_lumaSampler;
Rc<DxvkShader> g_lumaShader;
DxvkDevice* g_lumaShaderDevice = nullptr;
bool g_lumaAvailable = false;
uint64_t g_lumaDispatchCount = 0;
uint64_t g_lumaFailureCount = 0;
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
    g_lumaView = Rc<DxvkImageView>();
    g_lumaImage = Rc<DxvkImage>();
    g_lumaSampler = Rc<DxvkSampler>();
    g_lumaAvailable = false;
    g_lumaDispatchCount = 0;
    g_lumaFailureCount = 0;
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

    const VkFormatProperties formatProperties =
        device->adapter()->formatProperties(format);
    if (!(formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
        snapframeLog(
                "SnapFrame: history format unsupported: format=0x%x lacks sampled-image support\n",
                static_cast<unsigned>(format));
        return false;
    }

    // History images are transfer destinations and sampled compute inputs.
    // Query the exact usage combination and extent before allocating either
    // image, so unsupported WSI formats fail with a precise diagnostic.
    VkImageFormatProperties historyImageProperties{};
    const VkResult historyImageResult = device->adapter()->imageFormatProperties(
        format,
        VK_IMAGE_TYPE_2D,
        VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        0,
        historyImageProperties);
    if (historyImageResult != VK_SUCCESS
        || width > historyImageProperties.maxExtent.width
        || height > historyImageProperties.maxExtent.height) {
        snapframeLog(
                "SnapFrame: history image config unsupported format=0x%x result=%d extent=%ux%u max=%ux%u usage=TRANSFER_DST|SAMPLED\n",
                static_cast<unsigned>(format), static_cast<int>(historyImageResult),
                width, height, historyImageProperties.maxExtent.width,
                historyImageProperties.maxExtent.height);
        return false;
    }

    // The first preprocessing kernel writes half-resolution R16F luminance.
    // Check both format feature bits and the concrete image configuration before
    // attempting to create that output. Capture remains usable if compute is not.
    const VkFormatProperties lumaFormatProperties =
        device->adapter()->formatProperties(VK_FORMAT_R16_SFLOAT);
    const VkFormatFeatureFlags requiredLumaFeatures =
        VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    VkImageFormatProperties lumaImageProperties{};
    const VkResult lumaImageResult = device->adapter()->imageFormatProperties(
        VK_FORMAT_R16_SFLOAT,
        VK_IMAGE_TYPE_2D,
        VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        0,
        lumaImageProperties);
    const bool lumaExtendedFormatsEnabled =
        device->features().core.features.shaderStorageImageExtendedFormats;
    const bool lumaFormatSupported =
        (lumaFormatProperties.optimalTilingFeatures & requiredLumaFeatures)
            == requiredLumaFeatures
        && lumaImageResult == VK_SUCCESS
        && lumaExtendedFormatsEnabled
        && (width / 2u + width % 2u) <= lumaImageProperties.maxExtent.width
        && (height / 2u + height % 2u) <= lumaImageProperties.maxExtent.height;
    snapframeLog(
            "SnapFrame: R16F luma output %s (features=0x%x imageResult=%d extendedFormats=%d)\n",
            lumaFormatSupported ? "supported" : "unsupported",
            static_cast<unsigned>(lumaFormatProperties.optimalTilingFeatures),
            static_cast<int>(lumaImageResult),
            lumaExtendedFormatsEnabled ? 1 : 0);

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

#if SNAPFRAME_HAS_LUMA_SHADER
    if (lumaFormatSupported) {
        try {
            const uint32_t lumaWidth = width / 2u + width % 2u;
            const uint32_t lumaHeight = height / 2u + height % 2u;
            DxvkImageCreateInfo lumaInfo{};
            lumaInfo.type = VK_IMAGE_TYPE_2D;
            lumaInfo.format = VK_FORMAT_R16_SFLOAT;
            lumaInfo.flags = 0;
            lumaInfo.sampleCount = VK_SAMPLE_COUNT_1_BIT;
            lumaInfo.extent = { lumaWidth, lumaHeight, 1 };
            lumaInfo.numLayers = 1;
            lumaInfo.mipLevels = 1;
            lumaInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            lumaInfo.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            lumaInfo.access = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
            lumaInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            lumaInfo.layout = VK_IMAGE_LAYOUT_GENERAL;
            lumaInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            lumaInfo.shared = VK_FALSE;
            g_lumaImage = device->createImage(lumaInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (g_lumaImage != nullptr) {
                DxvkImageViewCreateInfo lumaViewInfo{};
                lumaViewInfo.type = VK_IMAGE_VIEW_TYPE_2D;
                lumaViewInfo.format = VK_FORMAT_R16_SFLOAT;
                lumaViewInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
                lumaViewInfo.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
                lumaViewInfo.minLevel = 0;
                lumaViewInfo.numLevels = 1;
                lumaViewInfo.minLayer = 0;
                lumaViewInfo.numLayers = 1;
                g_lumaView = device->createImageView(g_lumaImage, lumaViewInfo);
            }

            DxvkSamplerCreateInfo samplerInfo{};
            samplerInfo.magFilter = VK_FILTER_NEAREST;
            samplerInfo.minFilter = VK_FILTER_NEAREST;
            samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            samplerInfo.mipmapLodBias = 0.0f;
            samplerInfo.mipmapLodMin = 0.0f;
            samplerInfo.mipmapLodMax = 0.0f;
            samplerInfo.useAnisotropy = VK_FALSE;
            samplerInfo.maxAnisotropy = 1.0f;
            samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.compareToDepth = VK_FALSE;
            samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
            samplerInfo.borderColor = VkClearColorValue{ { 0.0f, 0.0f, 0.0f, 0.0f } };
            samplerInfo.usePixelCoord = VK_FALSE;
            g_lumaSampler = device->createSampler(samplerInfo);

            const bool needShaderRegistration =
                g_lumaShaderDevice != device || g_lumaShader == nullptr;
            if (needShaderRegistration) {
                g_lumaShader = Rc<DxvkShader>();
                g_lumaShaderDevice = nullptr;
                const DxvkResourceSlot slots[] = {
                    { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_IMAGE_VIEW_TYPE_2D, VK_ACCESS_SHADER_READ_BIT },
                    { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_IMAGE_VIEW_TYPE_2D, VK_ACCESS_SHADER_WRITE_BIT },
                };
                DxvkInterfaceSlots iface{};
                SpirvCodeBuffer shaderCode(snapframe::shaders::k_luma_downsample_word_count,
                                           snapframe::shaders::k_luma_downsample);
                g_lumaShader = device->createShader(VK_SHADER_STAGE_COMPUTE_BIT, 2, slots, iface, shaderCode);
            }

            g_lumaAvailable = g_lumaImage != nullptr && g_lumaView != nullptr
                           && g_lumaSampler != nullptr && g_lumaShader != nullptr;
            if (g_lumaAvailable && needShaderRegistration) {
                device->registerShader(g_lumaShader);
                g_lumaShaderDevice = device;
            }
            if (!g_lumaAvailable && needShaderRegistration)
                g_lumaShader = Rc<DxvkShader>();
        } catch (...) {
            g_lumaAvailable = false;
            g_lumaShader = Rc<DxvkShader>();
            g_lumaShaderDevice = nullptr;
            snapframeLog("SnapFrame: exception while creating luma compute resources\\n");
        }
        if (g_lumaAvailable) {
            snapframeLog("SnapFrame: luma compute resources ready output=%ux%u format=R16_SFLOAT\\n",
                         width / 2u + width % 2u, height / 2u + height % 2u);
        } else {
            snapframeLog("SnapFrame: luma resources image=%d view=%d sampler=%d shader=%d\\n",
                         g_lumaImage != nullptr, g_lumaView != nullptr,
                         g_lumaSampler != nullptr, g_lumaShader != nullptr);
            g_lumaView = Rc<DxvkImageView>();
            g_lumaImage = Rc<DxvkImage>();
            g_lumaSampler = Rc<DxvkSampler>();
            snapframeLog("SnapFrame: luma compute resource creation failed; capture remains active\\n");
        }
    } else {
        snapframeLog("SnapFrame: luma compute disabled by GPU format/feature limits\n");
    }
#else
    snapframeLog("SnapFrame: luma SPIR-V header missing; run tools/compile_snapframe_shaders.py before APK build\n");
#endif

    g_device = device;
    g_width = width;
    g_height = height;
    g_format = format;
    g_frameIndex = 0;
    g_readyLogged = false;
    snapframeLog( "SnapFrame: allocated two-frame history %ux%u format=0x%x\n",
            width, height, static_cast<unsigned>(format));
    return true;
}

void snapframeCopyFrame(void* contextPtr, void* sourceViewPtr, void* devicePtr,
                        uint32_t width, uint32_t height, uint32_t formatValue) {
    if (!g_initialized.load(std::memory_order_acquire))
        return;
    // Consume pending resets before checking enabled. This lets the next
    // present callback release GPU history immediately after the feature is
    // switched off, rather than retaining it until a later re-enable/device
    // destruction.
    if (g_resetHistoryRequested.exchange(0, std::memory_order_acq_rel)) {
        clearHistory();
        snapframeLog("SnapFrame: history reset requested; cached GPU resources released\n");
    }
    if (!g_enabled.load(std::memory_order_acquire))
        return;
    const uint64_t callbackCall =
        g_copyCallbackCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (callbackCall == 1)
        snapframeLog( "SnapFrame: host copy callback entered\n");

    auto* context = reinterpret_cast<DxvkContext*>(contextPtr);
    auto* sourceView = reinterpret_cast<DxvkImageView*>(sourceViewPtr);
    auto* device = reinterpret_cast<DxvkDevice*>(devicePtr);
    if (!context || !sourceView || !device || !width || !height) {
        g_skippedFrames.fetch_add(1, std::memory_order_relaxed);
        if (!g_skipReasonLogged) {
            g_skipReasonLogged = true;
            snapframeLog("SnapFrame: capture skipped: invalid callback arguments context=%d source=%d device=%d extent=%ux%u\\n",
                         context != nullptr, sourceView != nullptr, device != nullptr,
                         width, height);
        }
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
            snapframeLog( "SnapFrame: capture skipped: %s\n", skipReason);
        }
        return;
    }

    const VkFormat format = sourceImage->info().format;
    if (g_device != device || g_width != width || g_height != height || g_format != format) {
        clearHistory();
        bool historyCreated = false;
        try {
            historyCreated = createHistory(device, width, height, format);
        } catch (...) {
            snapframeLog("SnapFrame: exception while allocating frame history\n");
        }
        if (!historyCreated) {
            clearHistory();
            g_skippedFrames.fetch_add(1, std::memory_order_relaxed);
            snapframeLog("SnapFrame: history allocation failed for %ux%u format=0x%x\n",
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

    if (g_lumaAvailable) {
        const uint32_t lumaWidth = width / 2u + width % 2u;
        const uint32_t lumaHeight = height / 2u + height % 2u;
        const uint32_t groupsX = (lumaWidth + 7u) / 8u;
        const uint32_t groupsY = (lumaHeight + 7u) / 8u;
        const bool dispatched = context->dispatchSnapFrame(
            g_lumaShader, g_historyViews[slot], g_lumaSampler, g_lumaView, groupsX, groupsY);
        if (dispatched) {
            ++g_lumaDispatchCount;
            if (g_lumaDispatchCount == 1 || (g_lumaDispatchCount % 300u) == 0)
                snapframeLog("SnapFrame: luma dispatch accepted count=%llu groups=%ux%u\n",
                    static_cast<unsigned long long>(g_lumaDispatchCount), groupsX, groupsY);
        } else {
            ++g_lumaFailureCount;
            if (g_lumaFailureCount == 1 || (g_lumaFailureCount % 60u) == 0)
                snapframeLog("SnapFrame: luma dispatch failed count=%llu\n",
                    static_cast<unsigned long long>(g_lumaFailureCount));
        }
    }

    ++g_frameIndex;
    const uint64_t captured = g_capturedFrames.fetch_add(1, std::memory_order_relaxed) + 1;
    if (g_frameIndex >= 2 && !g_readyLogged) {
        g_readyLogged = true;
        snapframeLog( "SnapFrame: two real frames captured; history ring valid\n");
    }
    if (captured == 1 || captured == 60 || (captured % 1800) == 0)
        snapframeLog( "SnapFrame: captured=%llu skipped=%llu extent=%ux%u\n",
                static_cast<unsigned long long>(captured),
                static_cast<unsigned long long>(g_skippedFrames.load(std::memory_order_relaxed)),
                width, height);
}

void snapframeDeviceDestroyed(void* devicePtr) {
    auto* device = reinterpret_cast<DxvkDevice*>(devicePtr);
    if (g_device == device || g_lumaShaderDevice == device) {
        snapframeLog("SnapFrame: DXVK device destroyed; releasing history and shader state\n");
        clearHistory();
        g_lumaShader = Rc<DxvkShader>();
        g_lumaShaderDevice = nullptr;
    }
}

void snapframeForwardDxvkLog(const char* message) {
    if (message && *message)
        snapframeLog("%s\n", message);
}
}

extern "C" int snapframe_host_set_log_path(const char *path) {
    if (!path || !*path) return 0;
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFile) {
        fclose(g_logFile);
        g_logFile = nullptr;
    }
    g_logFile = fopen(path, "w");
    if (!g_logFile) {
        fprintf(stderr, "SnapFrame: cannot open dedicated log: %s\n", path);
        return 0;
    }
    fprintf(g_logFile, "SnapFrame diagnostic log\npath=%s\n", path);
    fflush(g_logFile);
    return 1;
}

extern "C" void snapframe_host_log(const char *message) {
    if (message && *message)
        snapframeLog("SnapFrame: %s\n", message);
}

extern "C" void snapframe_dxvk_set_copy_hook(
    void (*hook)(void*, void*, void*, uint32_t, uint32_t, uint32_t));
extern "C" void snapframe_dxvk_set_device_destroy_hook(void (*hook)(void*));
extern "C" void snapframe_dxvk_set_log_hook(void (*hook)(const char*));

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
    g_copyCallbackCalls.store(0, std::memory_order_relaxed);
    g_resetHistoryRequested.store(1, std::memory_order_release);
    g_initialized.store(1, std::memory_order_release);

    snapframe_dxvk_set_copy_hook(snapframeCopyFrame);
    snapframe_dxvk_set_device_destroy_hook(snapframeDeviceDestroyed);
    snapframe_dxvk_set_log_hook(snapframeForwardDxvkLog);
    snapframeLog( "SnapFrame: host initialized enabled=%d quality=%d flow_scale=%d%% mode=capture+luma-preprocess-test\n",
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
