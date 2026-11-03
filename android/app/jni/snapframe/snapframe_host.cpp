#include "snapframe_host.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <string>

#include "dxvk_context.h"
#include "dxvk_device.h"
#include "dxvk_gpu_query.h"
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
std::mutex g_gpuLogMutex;
FILE* g_gpuLogFile = nullptr;
std::mutex g_flowLogMutex;
FILE* g_flowLogFile = nullptr;
std::mutex g_vectorLogMutex;
FILE* g_vectorLogFile = nullptr;
bool g_profilingEnabled = false;
bool g_motionAnalysisEnabled = false;
bool g_flowReadbackFailureLogged = false;

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
Rc<DxvkImage> g_lumaImage[2];
Rc<DxvkImageView> g_lumaView[2];
// [0] current->previous, [1] previous->current motion fields.
Rc<DxvkImage> g_flowImage[2];
Rc<DxvkImageView> g_flowView[2];
Rc<DxvkImage> g_consistencyImage;
Rc<DxvkImageView> g_consistencyView;
Rc<DxvkSampler> g_lumaSampler;
Rc<DxvkShader> g_lumaShader;
Rc<DxvkShader> g_flowShader;
Rc<DxvkShader> g_consistencyShader;
DxvkDevice* g_lumaShaderDevice = nullptr;
DxvkDevice* g_flowShaderDevice = nullptr;
DxvkDevice* g_consistencyShaderDevice = nullptr;
bool g_lumaAvailable = false;
bool g_flowAvailable = false;
bool g_consistencyAvailable = false;
bool g_lumaValid[2] = { false, false };
uint64_t g_lumaDispatchCount = 0;
uint64_t g_lumaFailureCount = 0;
uint64_t g_flowDispatchCount = 0;
uint64_t g_flowFailureCount = 0;
uint64_t g_consistencyDispatchCount = 0;
uint64_t g_consistencyFailureCount = 0;
uint32_t g_width = 0;
uint32_t g_height = 0;
VkFormat g_format = VK_FORMAT_UNDEFINED;
uint64_t g_frameIndex = 0;
bool g_readyLogged = false;
bool g_skipReasonLogged = false;
bool g_diagnosticSuiteEnabled = false;
bool g_diagnosticSuiteComplete = false;
uint32_t g_diagnosticPhaseFrames = 1800;
uint32_t g_diagnosticPhase = UINT32_MAX;
uint64_t g_phaseStartCaptured = 0, g_phaseStartLuma = 0, g_phaseStartFlow = 0;
uint64_t g_phaseStartConsistency = 0, g_phaseStartSkipped = 0;
uint64_t g_phaseLastProgress = 0;

const char* diagnosticPhaseName(uint32_t phase) {
    static const char* names[] = { "static_scene", "slow_camera_pan", "fast_camera_rotation",
        "moving_npc_combat", "foliage_particles_water", "hud_inventory_map", "long_run_stress" };
    return phase < 7 ? names[phase] : "invalid";
}

void logDiagnosticPhaseResult(uint32_t phase, uint64_t frames) {
    snapframeLog("SnapFrame: TEST_PHASE_RESULT phase=%s frames=%llu luma_ok=%llu flow_pairs_ok=%llu consistency_ok=%llu skipped=%llu luma_fail_total=%llu flow_fail_total=%llu consistency_fail_total=%llu\n",
        diagnosticPhaseName(phase),
        static_cast<unsigned long long>(frames),
        static_cast<unsigned long long>(g_lumaDispatchCount - g_phaseStartLuma),
        static_cast<unsigned long long>(g_flowDispatchCount - g_phaseStartFlow),
        static_cast<unsigned long long>(g_consistencyDispatchCount - g_phaseStartConsistency),
        static_cast<unsigned long long>(g_skippedFrames.load(std::memory_order_relaxed) - g_phaseStartSkipped),
        static_cast<unsigned long long>(g_lumaFailureCount),
        static_cast<unsigned long long>(g_flowFailureCount),
        static_cast<unsigned long long>(g_consistencyFailureCount));
}

void beginDiagnosticPhase(uint32_t phase, uint64_t captured) {
    static const char* actions[] = {
        "remain still; do not move the camera",
        "slowly pan the camera horizontally and vertically",
        "rotate the camera quickly in several directions",
        "observe moving NPCs/combat without rapid camera movement",
        "capture foliage, water, particles, smoke or fire",
        "open HUD, inventory and map; move selection/cursor",
        "continue normal gameplay for a sustained stability run"
    };
    g_diagnosticPhase = phase;
    g_phaseStartCaptured = captured - 1u;
    g_phaseStartLuma = g_lumaDispatchCount;
    g_phaseStartFlow = g_flowDispatchCount;
    g_phaseStartConsistency = g_consistencyDispatchCount;
    g_phaseStartSkipped = g_skippedFrames.load(std::memory_order_relaxed);
    g_phaseLastProgress = 0;
    snapframeLog("SnapFrame: TEST_PHASE_BEGIN index=%u name=%s target_frames=%u\n",
        phase + 1u, diagnosticPhaseName(phase), g_diagnosticPhaseFrames);
    snapframeLog("SnapFrame: TEST_ACTION %s\n", actions[phase]);
    snapframeLog("SnapFrame: TEST_PHASE_PROGRESS phase=%s elapsed_frames=0 target_frames=%u\n",
        diagnosticPhaseName(phase), g_diagnosticPhaseFrames);
}

void logDiagnosticPhase(uint64_t captured) {
    if (!g_diagnosticSuiteEnabled || g_diagnosticSuiteComplete
        || !g_diagnosticPhaseFrames || !captured)
        return;

    const uint64_t totalFrames = uint64_t(g_diagnosticPhaseFrames) * 7u;
    if (captured > totalFrames) {
        if (g_diagnosticPhase == 6u) {
            logDiagnosticPhaseResult(6u, totalFrames - g_phaseStartCaptured);
            snapframeLog("SnapFrame: TEST_SUITE_COMPLETE total_frames=%llu\n",
                static_cast<unsigned long long>(totalFrames));
            g_diagnosticSuiteComplete = true;
        }
        return;
    }

    const uint32_t phase = static_cast<uint32_t>(
        std::min<uint64_t>((captured - 1u) / g_diagnosticPhaseFrames, 6u));
    if (phase == g_diagnosticPhase) {
        const uint64_t elapsed = captured - g_phaseStartCaptured;
        if (elapsed >= g_phaseLastProgress + 300u) {
            g_phaseLastProgress = (elapsed / 300u) * 300u;
            snapframeLog("SnapFrame: TEST_PHASE_PROGRESS phase=%s elapsed_frames=%llu target_frames=%u\n",
                diagnosticPhaseName(phase),
                static_cast<unsigned long long>(g_phaseLastProgress),
                g_diagnosticPhaseFrames);
        }
        return;
    }

    if (g_diagnosticPhase != UINT32_MAX) {
        logDiagnosticPhaseResult(g_diagnosticPhase,
            captured - 1u - g_phaseStartCaptured);
    }
    beginDiagnosticPhase(phase, captured);
}

struct SnapFrameGpuTimer {
    Rc<DxvkGpuQuery> begin;
    Rc<DxvkGpuQuery> end;
    double periodNs = 0.0;
    uint64_t frame = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::string stage;
};

SnapFrameGpuTimer beginGpuTimer(
    DxvkContext* context, DxvkDevice* device, const char* stage,
    uint64_t frame, uint32_t width, uint32_t height, bool sample) {
    SnapFrameGpuTimer timer;
    if (!g_profilingEnabled || !sample || !context || !device || !stage)
        return timer;
    timer.begin = device->createGpuQuery(VK_QUERY_TYPE_TIMESTAMP, 0, 0);
    timer.end = device->createGpuQuery(VK_QUERY_TYPE_TIMESTAMP, 0, 0);
    timer.periodNs = device->adapter()->deviceProperties().limits.timestampPeriod;
    timer.frame = frame;
    timer.width = width;
    timer.height = height;
    timer.stage = stage;
    if (timer.begin == nullptr || timer.end == nullptr || timer.periodNs <= 0.0) {
        timer.begin = Rc<DxvkGpuQuery>();
        timer.end = Rc<DxvkGpuQuery>();
        return timer;
    }
    context->writeTimestamp(timer.begin);
    return timer;
}

void endGpuTimer(DxvkContext* context, const SnapFrameGpuTimer& timer) {
    if (!context || timer.begin == nullptr || timer.end == nullptr)
        return;
    context->writeTimestamp(timer.end);
    context->addCompletionCallback([timer](VkResult result) {
        DxvkQueryData beginData{}, endData{};
        const DxvkGpuQueryStatus beginStatus = timer.begin->getData(beginData);
        const DxvkGpuQueryStatus endStatus = timer.end->getData(endData);
        const bool valid = result == VK_SUCCESS
            && beginStatus == DxvkGpuQueryStatus::Available
            && endStatus == DxvkGpuQueryStatus::Available
            && endData.timestamp.time >= beginData.timestamp.time
            && timer.periodNs > 0.0;
        std::lock_guard<std::mutex> lock(g_gpuLogMutex);
        if (!g_gpuLogFile) return;
        if (valid) {
            const double gpuMs = double(endData.timestamp.time - beginData.timestamp.time)
                * timer.periodNs / 1000000.0;
            fprintf(g_gpuLogFile, "%llu,%s,%.6f,%u,%u,ok\n",
                static_cast<unsigned long long>(timer.frame), timer.stage.c_str(),
                gpuMs, timer.width, timer.height);
        } else {
            fprintf(g_gpuLogFile, "%llu,%s,0,%u,%u,unavailable:%d:%d:%d\n",
                static_cast<unsigned long long>(timer.frame), timer.stage.c_str(),
                timer.width, timer.height, int(result), int(beginStatus), int(endStatus));
        }
        fflush(g_gpuLogFile);
    });
}

float snapFrameHalfToFloat(uint16_t value) {
    const uint32_t sign = uint32_t(value & 0x8000u) << 16;
    uint32_t exponent = (value >> 10) & 0x1fu;
    uint32_t mantissa = value & 0x03ffu;
    uint32_t bits = 0;
    if (exponent == 0) {
        if (mantissa == 0) bits = sign;
        else {
            int e = -14;
            while ((mantissa & 0x0400u) == 0) { mantissa <<= 1; --e; }
            mantissa &= 0x03ffu;
            bits = sign | (uint32_t(e + 127) << 23) | (mantissa << 13);
        }
    } else if (exponent == 0x1fu) {
        bits = sign | 0x7f800000u | (mantissa << 13);
    } else {
        bits = sign | ((exponent - 15u + 127u) << 23) | (mantissa << 13);
    }
    float result = 0.0f;
    memcpy(&result, &bits, sizeof(result));
    return result;
}

struct SnapFrameFlowStats {
    double dx = 0.0, dy = 0.0, magnitude = 0.0, confidence = 0.0, cost = 0.0;
    uint64_t finite = 0, nonFinite = 0;
    uint64_t confidenceLt01 = 0, confidence01To05 = 0;
    uint64_t confidenceGe05 = 0, confidenceGe075 = 0, confidenceGe09 = 0;
    uint64_t highConfidenceNonZero = 0, boundarySaturated = 0, boundaryLowConfidence = 0;
};

SnapFrameFlowStats summarizeFlow(const uint16_t* values, uint64_t count) {
    SnapFrameFlowStats stats;
    for (uint64_t i = 0; i < count; ++i) {
        const float dx = snapFrameHalfToFloat(values[i * 4u + 0u]);
        const float dy = snapFrameHalfToFloat(values[i * 4u + 1u]);
        const float confidence = snapFrameHalfToFloat(values[i * 4u + 2u]);
        const float cost = snapFrameHalfToFloat(values[i * 4u + 3u]);
        if (!std::isfinite(dx) || !std::isfinite(dy)
            || !std::isfinite(confidence) || !std::isfinite(cost)) {
            ++stats.nonFinite;
            continue;
        }
        stats.dx += dx; stats.dy += dy;
        const double magnitude = std::sqrt(double(dx) * dx + double(dy) * dy);
        stats.magnitude += magnitude;
        stats.confidence += confidence; stats.cost += cost;
        if (confidence < 0.1f) ++stats.confidenceLt01;
        else if (confidence < 0.5f) ++stats.confidence01To05;
        if (confidence >= 0.5f) ++stats.confidenceGe05;
        if (confidence >= 0.75f) ++stats.confidenceGe075;
        if (confidence >= 0.9f) ++stats.confidenceGe09;
        if (confidence >= 0.5f && magnitude > 0.5) ++stats.highConfidenceNonZero;
        const bool boundary = std::fabs(dx) >= 3.5f || std::fabs(dy) >= 3.5f;
        if (boundary) {
            ++stats.boundarySaturated;
            if (confidence < 0.1f) ++stats.boundaryLowConfidence;
        }
        ++stats.finite;
    }
    if (stats.finite) {
        const double inv = 1.0 / double(stats.finite);
        stats.dx *= inv; stats.dy *= inv; stats.magnitude *= inv;
        stats.confidence *= inv; stats.cost *= inv;
    }
    return stats;
}

void recordFlowReadback(
    DxvkContext* context, DxvkDevice* device, uint64_t frame,
    uint32_t flowWidth, uint32_t flowHeight, bool dumpVectors) {
    if (!g_profilingEnabled || !g_diagnosticSuiteEnabled || g_diagnosticSuiteComplete
        || !context || !device || g_flowImage[0] == nullptr
        || g_flowImage[1] == nullptr || g_consistencyImage == nullptr)
        return;

    const VkDeviceSize pixels = VkDeviceSize(flowWidth) * flowHeight;
    const VkDeviceSize flowBytes = pixels * 8u; // RGBA16F
    const VkDeviceSize maskOffset = flowBytes * 2u;
    const VkDeviceSize totalBytes = maskOffset + pixels * 2u; // R16F mask
    DxvkBufferCreateInfo bufferInfo{};
    bufferInfo.size = totalBytes;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.stages = VK_PIPELINE_STAGE_HOST_BIT;
    bufferInfo.access = VK_ACCESS_HOST_READ_BIT;

    Rc<DxvkBuffer> readback;
    try {
        readback = device->createBuffer(bufferInfo,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    } catch (...) {
        if (!g_flowReadbackFailureLogged) {
            g_flowReadbackFailureLogged = true;
            snapframeLog("SnapFrame: flow readback buffer allocation failed (HOST_VISIBLE|HOST_COHERENT unavailable or allocation error)\n");
        }
        return;
    }
    if (readback == nullptr || readback->mapPtr(0) == nullptr) {
        if (!g_flowReadbackFailureLogged) {
            g_flowReadbackFailureLogged = true;
            snapframeLog("SnapFrame: flow readback buffer is not host mapped\n");
        }
        return;
    }

    VkImageSubresourceLayers subresource{};
    subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    subresource.mipLevel = 0;
    subresource.baseArrayLayer = 0;
    subresource.layerCount = 1;
    const VkExtent3D extent{ flowWidth, flowHeight, 1 };
    context->copyImageToBuffer(readback, 0, 0, 0, g_flowImage[0],
        subresource, VkOffset3D{ 0, 0, 0 }, extent);
    context->copyImageToBuffer(readback, flowBytes, 0, 0, g_flowImage[1],
        subresource, VkOffset3D{ 0, 0, 0 }, extent);
    context->copyImageToBuffer(readback, maskOffset, 0, 0, g_consistencyImage,
        subresource, VkOffset3D{ 0, 0, 0 }, extent);

    context->addCompletionCallback([readback, frame, flowWidth, flowHeight,
                                    pixels, flowBytes, maskOffset, dumpVectors](VkResult result) {
        if (result != VK_SUCCESS) {
            std::lock_guard<std::mutex> lock(g_flowLogMutex);
            if (g_flowLogFile) {
                fprintf(g_flowLogFile,
                    "%llu,%u,%u,0,0,0,0,0,0,0,0,0,0,0,0,0,completion_error:%d\n",
                    static_cast<unsigned long long>(frame), flowWidth, flowHeight, int(result));
                fflush(g_flowLogFile);
            }
            return;
        }
        const uint16_t* data = reinterpret_cast<const uint16_t*>(readback->mapPtr(0));
        if (!data) {
            std::lock_guard<std::mutex> lock(g_flowLogMutex);
            if (g_flowLogFile) {
                fprintf(g_flowLogFile,
                    "%llu,%u,%u,0,0,0,0,0,0,0,0,0,0,0,0,0,readback_mapping_failed\n",
                    static_cast<unsigned long long>(frame), flowWidth, flowHeight);
                fflush(g_flowLogFile);
            }
            return;
        }
        const SnapFrameFlowStats forward = summarizeFlow(data, pixels);
        const SnapFrameFlowStats backward = summarizeFlow(
            reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(data) + flowBytes),
            pixels);
        const uint16_t* mask = reinterpret_cast<const uint16_t*>(
            reinterpret_cast<const uint8_t*>(data) + maskOffset);
        double consistencySum = 0.0;
        uint64_t consistencyFinite = 0, consistencyValid = 0;
        uint64_t nonFinite = forward.nonFinite + backward.nonFinite;
        for (uint64_t i = 0; i < pixels; ++i) {
            const float value = snapFrameHalfToFloat(mask[i]);
            if (!std::isfinite(value)) { ++nonFinite; continue; }
            consistencySum += value;
            ++consistencyFinite;
            if (value > 0.5f) ++consistencyValid;
        }
        const double consistencyMean = consistencyFinite
            ? consistencySum / double(consistencyFinite) : 0.0;
        const double consistencyPercent = consistencyFinite
            ? 100.0 * double(consistencyValid) / double(consistencyFinite) : 0.0;

        {
            std::lock_guard<std::mutex> lock(g_flowLogMutex);
            if (g_flowLogFile) {
                fprintf(g_flowLogFile,
                    "%llu,%u,%u,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,"
                    "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
                    "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%.6f,%.3f,%llu,ok\n",
                    static_cast<unsigned long long>(frame), flowWidth, flowHeight,
                    forward.dx, forward.dy, forward.magnitude, forward.confidence, forward.cost,
                    backward.dx, backward.dy, backward.magnitude, backward.confidence, backward.cost,
                    static_cast<unsigned long long>(forward.confidenceLt01),
                    static_cast<unsigned long long>(forward.confidence01To05),
                    static_cast<unsigned long long>(forward.confidenceGe05),
                    static_cast<unsigned long long>(forward.confidenceGe075),
                    static_cast<unsigned long long>(forward.confidenceGe09),
                    static_cast<unsigned long long>(forward.highConfidenceNonZero),
                    static_cast<unsigned long long>(forward.boundarySaturated),
                    static_cast<unsigned long long>(forward.boundaryLowConfidence),
                    static_cast<unsigned long long>(backward.confidenceLt01),
                    static_cast<unsigned long long>(backward.confidence01To05),
                    static_cast<unsigned long long>(backward.confidenceGe05),
                    static_cast<unsigned long long>(backward.confidenceGe075),
                    static_cast<unsigned long long>(backward.confidenceGe09),
                    static_cast<unsigned long long>(backward.highConfidenceNonZero),
                    static_cast<unsigned long long>(backward.boundarySaturated),
                    static_cast<unsigned long long>(backward.boundaryLowConfidence),
                    consistencyMean, consistencyPercent, static_cast<unsigned long long>(nonFinite));
                fflush(g_flowLogFile);
            }
        }

        if (dumpVectors) {
            const uint16_t* forwardData = data;
            const uint16_t* backwardData = reinterpret_cast<const uint16_t*>(
                reinterpret_cast<const uint8_t*>(data) + flowBytes);
            std::lock_guard<std::mutex> vectorLock(g_vectorLogMutex);
            if (g_vectorLogFile) {
                for (uint32_t y = 0; y < flowHeight; ++y) {
                    for (uint32_t x = 0; x < flowWidth; ++x) {
                        const uint64_t index = uint64_t(y) * flowWidth + x;
                        const uint16_t* fwd = forwardData + index * 4u;
                        const uint16_t* bwd = backwardData + index * 4u;
                        fprintf(g_vectorLogFile,
                            "%llu,%u,%u,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f\n",
                            static_cast<unsigned long long>(frame), x, y,
                            snapFrameHalfToFloat(fwd[0]), snapFrameHalfToFloat(fwd[1]),
                            snapFrameHalfToFloat(fwd[2]), snapFrameHalfToFloat(fwd[3]),
                            snapFrameHalfToFloat(bwd[0]), snapFrameHalfToFloat(bwd[1]),
                            snapFrameHalfToFloat(bwd[2]), snapFrameHalfToFloat(bwd[3]),
                            snapFrameHalfToFloat(mask[index]));
                    }
                }
                fflush(g_vectorLogFile);
            }
        }
    });
}

void clearHistory() {
    g_historyViews[0] = Rc<DxvkImageView>();
    g_historyViews[1] = Rc<DxvkImageView>();
    g_history[0] = Rc<DxvkImage>();
    g_history[1] = Rc<DxvkImage>();
    g_lumaView[0] = Rc<DxvkImageView>();
    g_lumaView[1] = Rc<DxvkImageView>();
    g_lumaImage[0] = Rc<DxvkImage>();
    g_lumaImage[1] = Rc<DxvkImage>();
    g_flowView[0] = Rc<DxvkImageView>();
    g_flowView[1] = Rc<DxvkImageView>();
    g_flowImage[0] = Rc<DxvkImage>();
    g_flowImage[1] = Rc<DxvkImage>();
    g_consistencyView = Rc<DxvkImageView>();
    g_consistencyImage = Rc<DxvkImage>();
    g_lumaSampler = Rc<DxvkSampler>();
    g_lumaAvailable = false;
    g_flowAvailable = false;
    g_consistencyAvailable = false;
    g_lumaValid[0] = g_lumaValid[1] = false;
    g_lumaDispatchCount = 0;
    g_lumaFailureCount = 0;
    g_flowDispatchCount = 0;
    g_flowFailureCount = 0;
    g_consistencyDispatchCount = 0;
    g_consistencyFailureCount = 0;
    g_device = nullptr;
    g_width = g_height = 0;
    g_format = VK_FORMAT_UNDEFINED;
    g_frameIndex = 0;
    g_readyLogged = false;
    g_skipReasonLogged = false;
    g_diagnosticPhase = UINT32_MAX;
    g_phaseStartCaptured = g_phaseStartLuma = g_phaseStartFlow = 0;
    g_phaseStartConsistency = g_phaseStartSkipped = 0;
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
    if (g_motionAnalysisEnabled && lumaFormatSupported) {
        bool createdShader = false;
        bool createdFlowShader = false;
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
            DxvkImageViewCreateInfo lumaViewInfo{};
            lumaViewInfo.type = VK_IMAGE_VIEW_TYPE_2D;
            lumaViewInfo.format = VK_FORMAT_R16_SFLOAT;
            lumaViewInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            lumaViewInfo.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
            lumaViewInfo.minLevel = 0;
            lumaViewInfo.numLevels = 1;
            lumaViewInfo.minLayer = 0;
            lumaViewInfo.numLayers = 1;
            for (unsigned i = 0; i < 2; ++i) {
                g_lumaImage[i] = device->createImage(lumaInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                if (g_lumaImage[i] != nullptr)
                    g_lumaView[i] = device->createImageView(g_lumaImage[i], lumaViewInfo);
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
                createdShader = true;
            }

            g_lumaAvailable = g_lumaImage[0] != nullptr && g_lumaImage[1] != nullptr
                           && g_lumaView[0] != nullptr && g_lumaView[1] != nullptr
                           && g_lumaSampler != nullptr && g_lumaShader != nullptr;
            if (g_lumaAvailable && needShaderRegistration) {
                device->registerShader(g_lumaShader);
                g_lumaShaderDevice = device;
            }
            if (!g_lumaAvailable && needShaderRegistration)
                g_lumaShader = Rc<DxvkShader>();

            const VkFormat flowFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
            const VkFormatProperties flowProperties = device->adapter()->formatProperties(flowFormat);
            const VkFormatFeatureFlags flowRequired = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
            VkImageFormatProperties flowImageProperties{};
            const VkResult flowImageResult = device->adapter()->imageFormatProperties(
                flowFormat, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, flowImageProperties);
            const uint32_t flowWidth = (lumaWidth + 7u) / 8u;
            const uint32_t flowHeight = (lumaHeight + 7u) / 8u;
            const bool flowSupported =
                (flowProperties.optimalTilingFeatures & flowRequired) == flowRequired
                && flowImageResult == VK_SUCCESS
                && flowWidth <= flowImageProperties.maxExtent.width
                && flowHeight <= flowImageProperties.maxExtent.height
                && device->features().core.features.shaderStorageImageExtendedFormats;

            if (g_lumaAvailable && flowSupported) {
                DxvkImageCreateInfo flowInfo{};
                flowInfo.type = VK_IMAGE_TYPE_2D;
                flowInfo.format = flowFormat;
                flowInfo.flags = 0;
                flowInfo.sampleCount = VK_SAMPLE_COUNT_1_BIT;
                flowInfo.extent = { flowWidth, flowHeight, 1 };
                flowInfo.numLayers = 1;
                flowInfo.mipLevels = 1;
                flowInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
                flowInfo.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
                flowInfo.access = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
                flowInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
                flowInfo.layout = VK_IMAGE_LAYOUT_GENERAL;
                flowInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                flowInfo.shared = VK_FALSE;
                DxvkImageViewCreateInfo flowViewInfo{};
                flowViewInfo.type = VK_IMAGE_VIEW_TYPE_2D;
                flowViewInfo.format = flowFormat;
                flowViewInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
                flowViewInfo.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
                flowViewInfo.minLevel = 0;
                flowViewInfo.numLevels = 1;
                flowViewInfo.minLayer = 0;
                flowViewInfo.numLayers = 1;
                for (unsigned i = 0; i < 2; ++i) {
                    g_flowImage[i] = device->createImage(flowInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                    if (g_flowImage[i] != nullptr)
                        g_flowView[i] = device->createImageView(g_flowImage[i], flowViewInfo);
                }

                const bool needFlowRegistration = g_flowShaderDevice != device || g_flowShader == nullptr;
                if (needFlowRegistration) {
                    g_flowShader = Rc<DxvkShader>();
                    g_flowShaderDevice = nullptr;
                    const DxvkResourceSlot flowSlots[] = {
                        { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_IMAGE_VIEW_TYPE_2D, VK_ACCESS_SHADER_READ_BIT },
                        { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_IMAGE_VIEW_TYPE_2D, VK_ACCESS_SHADER_READ_BIT },
                        { 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_IMAGE_VIEW_TYPE_2D, VK_ACCESS_SHADER_WRITE_BIT },
                    };
                    DxvkInterfaceSlots flowIface{};
                    SpirvCodeBuffer flowCode(snapframe::shaders::k_block_match_flow_word_count,
                                             snapframe::shaders::k_block_match_flow);
                    g_flowShader = device->createShader(VK_SHADER_STAGE_COMPUTE_BIT, 3, flowSlots, flowIface, flowCode);
                    createdFlowShader = true;
                }
                g_flowAvailable = g_flowImage[0] != nullptr && g_flowImage[1] != nullptr
                               && g_flowView[0] != nullptr && g_flowView[1] != nullptr
                               && g_flowShader != nullptr;
                if (g_flowAvailable && needFlowRegistration) {
                    device->registerShader(g_flowShader);
                    g_flowShaderDevice = device;
                }
                if (g_flowAvailable) {
                    DxvkImageCreateInfo consistencyInfo = flowInfo;
                    consistencyInfo.format = VK_FORMAT_R16_SFLOAT;
                    g_consistencyImage = device->createImage(
                        consistencyInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                    DxvkImageViewCreateInfo consistencyViewInfo = flowViewInfo;
                    consistencyViewInfo.format = VK_FORMAT_R16_SFLOAT;
                    if (g_consistencyImage != nullptr)
                        g_consistencyView = device->createImageView(
                            g_consistencyImage, consistencyViewInfo);

                    const bool needConsistencyRegistration =
                        g_consistencyShaderDevice != device || g_consistencyShader == nullptr;
                    if (needConsistencyRegistration) {
                        g_consistencyShader = Rc<DxvkShader>();
                        g_consistencyShaderDevice = nullptr;
                        const DxvkResourceSlot consistencySlots[] = {
                            { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_IMAGE_VIEW_TYPE_2D, VK_ACCESS_SHADER_READ_BIT },
                            { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_IMAGE_VIEW_TYPE_2D, VK_ACCESS_SHADER_READ_BIT },
                            { 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_IMAGE_VIEW_TYPE_2D, VK_ACCESS_SHADER_WRITE_BIT },
                        };
                        DxvkInterfaceSlots consistencyIface{};
                        SpirvCodeBuffer consistencyCode(
                            snapframe::shaders::k_flow_consistency_word_count,
                            snapframe::shaders::k_flow_consistency);
                        g_consistencyShader = device->createShader(
                            VK_SHADER_STAGE_COMPUTE_BIT, 3, consistencySlots,
                            consistencyIface, consistencyCode);
                    }
                    g_consistencyAvailable = g_consistencyImage != nullptr
                        && g_consistencyView != nullptr && g_consistencyShader != nullptr;
                    if (g_consistencyAvailable && needConsistencyRegistration) {
                        device->registerShader(g_consistencyShader);
                        g_consistencyShaderDevice = device;
                    }
                    if (g_consistencyAvailable)
                        snapframeLog("SnapFrame: forward/backward consistency resources ready format=R16_SFLOAT extent=%ux%u\n",
                            flowWidth, flowHeight);
                    if (!g_consistencyAvailable)
                        snapframeLog("SnapFrame: forward/backward consistency resources unavailable; flow remains diagnostic-only\n");
                }
            } else {
                g_flowAvailable = false;
                snapframeLog("SnapFrame: block-matching flow unsupported result=%d features=0x%x extent=%ux%u\n",
                    static_cast<int>(flowImageResult), static_cast<unsigned>(flowProperties.optimalTilingFeatures),
                    flowWidth, flowHeight);
            }
        } catch (...) {
            g_lumaAvailable = false;
            if (createdShader) {
                g_lumaShader = Rc<DxvkShader>();
                g_lumaShaderDevice = nullptr;
            }
            if (createdFlowShader) {
                g_flowShader = Rc<DxvkShader>();
                g_flowShaderDevice = nullptr;
            }
            g_flowView[0] = Rc<DxvkImageView>();
            g_flowView[1] = Rc<DxvkImageView>();
            g_flowImage[0] = Rc<DxvkImage>();
            g_flowImage[1] = Rc<DxvkImage>();
            g_consistencyView = Rc<DxvkImageView>();
            g_consistencyImage = Rc<DxvkImage>();
            g_consistencyAvailable = false;
            g_consistencyShader = Rc<DxvkShader>();
            g_consistencyShaderDevice = nullptr;
            snapframeLog("SnapFrame: exception while creating luma/flow compute resources\n");
        }
        if (g_motionAnalysisEnabled && g_lumaAvailable) {
            snapframeLog("SnapFrame: luma compute resources ready outputs=2 extent=%ux%u format=R16_SFLOAT\n",
                         width / 2u + width % 2u, height / 2u + height % 2u);
        } else {
            snapframeLog("SnapFrame: luma resources image=%d view=%d sampler=%d shader=%d\n",
                         g_lumaImage[0] != nullptr && g_lumaImage[1] != nullptr,
                         g_lumaView[0] != nullptr && g_lumaView[1] != nullptr,
                         g_lumaSampler != nullptr, g_lumaShader != nullptr);
            g_lumaView[0] = Rc<DxvkImageView>();
            g_lumaView[1] = Rc<DxvkImageView>();
            g_lumaImage[0] = Rc<DxvkImage>();
            g_lumaImage[1] = Rc<DxvkImage>();
            g_lumaSampler = Rc<DxvkSampler>();
            snapframeLog("SnapFrame: luma compute resource creation failed; capture remains active\n");
        }
        if (g_flowAvailable) {
            snapframeLog("SnapFrame: bidirectional block-matching flow ready outputs=2 extent=%ux%u format=RGBA16F search=9x9 patch=3x3 scale=1/16\n",
                ((width / 2u + width % 2u + 7u) / 8u),
                ((height / 2u + height % 2u + 7u) / 8u));
        } else {
            g_flowView[0] = Rc<DxvkImageView>();
            g_flowView[1] = Rc<DxvkImageView>();
            g_flowImage[0] = Rc<DxvkImage>();
            g_flowImage[1] = Rc<DxvkImage>();
        }
    } else {
        snapframeLog("SnapFrame: motion analysis disabled or luma compute unsupported; capture-only mode\n");
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
            snapframeLog("SnapFrame: capture skipped: invalid callback arguments context=%d source=%d device=%d extent=%ux%u\n",
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
    const uint64_t nextCapturedFrame = g_capturedFrames.load(std::memory_order_relaxed) + 1u;
    logDiagnosticPhase(nextCapturedFrame);
    const bool profileThisFrame = g_profilingEnabled
        && (!g_diagnosticSuiteEnabled || !g_diagnosticSuiteComplete)
        && (nextCapturedFrame % 30u) == 0u;
    const SnapFrameGpuTimer totalTimer = beginGpuTimer(
        context, device, "total_chain", nextCapturedFrame, width, height, profileThisFrame);
    bool totalTimerEnded = false;
    const SnapFrameGpuTimer captureTimer = beginGpuTimer(
        context, device, "capture_copy", nextCapturedFrame, width, height, profileThisFrame);
    VkImageSubresourceLayers subresource{};
    subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    subresource.mipLevel = 0;
    subresource.baseArrayLayer = 0;
    subresource.layerCount = 1;

    context->copyImage(
        g_history[slot], subresource, VkOffset3D{ 0, 0, 0 },
        sourceImage, subresource, VkOffset3D{ 0, 0, 0 },
        VkExtent3D{ width, height, 1 });
    if (profileThisFrame) context->flushComputeBarriers();
    endGpuTimer(context, captureTimer);

    if (g_lumaAvailable) {
        const uint32_t lumaWidth = width / 2u + width % 2u;
        const uint32_t lumaHeight = height / 2u + height % 2u;
        const uint32_t groupsX = (lumaWidth + 7u) / 8u;
        const uint32_t groupsY = (lumaHeight + 7u) / 8u;
        const SnapFrameGpuTimer lumaTimer = beginGpuTimer(
            context, device, "luma", nextCapturedFrame, width, height, profileThisFrame);
        const bool dispatched = context->dispatchSnapFrame(
            g_lumaShader, g_historyViews[slot], g_lumaSampler, g_lumaView[slot], groupsX, groupsY);
        if (dispatched && profileThisFrame) context->flushComputeBarriers();
        endGpuTimer(context, lumaTimer);
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

        g_lumaValid[slot] = dispatched;
        if (dispatched && g_flowAvailable && g_lumaValid[slot ^ 1u]) {
            const unsigned previousSlot = slot ^ 1u;
            const uint32_t flowWidth = (lumaWidth + 7u) / 8u;
            const uint32_t flowHeight = (lumaHeight + 7u) / 8u;
            const uint32_t flowGroupsX = (flowWidth + 7u) / 8u;
            const uint32_t flowGroupsY = (flowHeight + 7u) / 8u;
            // Slot 0 stores current->previous; slot 1 stores previous->current.
            const SnapFrameGpuTimer forwardTimer = beginGpuTimer(
                context, device, "flow_forward", nextCapturedFrame, width, height, profileThisFrame);
            const bool currentToPrevious = context->dispatchSnapFrameFlow(
                g_flowShader, g_lumaView[previousSlot], g_lumaView[slot],
                g_lumaSampler, g_flowView[0], flowGroupsX, flowGroupsY);
            if (currentToPrevious && profileThisFrame) context->flushComputeBarriers();
            endGpuTimer(context, forwardTimer);
            SnapFrameGpuTimer backwardTimer;
            bool previousToCurrent = false;
            if (currentToPrevious) {
                backwardTimer = beginGpuTimer(
                    context, device, "flow_backward", nextCapturedFrame, width, height, profileThisFrame);
                previousToCurrent = context->dispatchSnapFrameFlow(
                    g_flowShader, g_lumaView[slot], g_lumaView[previousSlot],
                    g_lumaSampler, g_flowView[1], flowGroupsX, flowGroupsY);
                if (previousToCurrent && profileThisFrame) context->flushComputeBarriers();
                endGpuTimer(context, backwardTimer);
            }
            if (currentToPrevious && previousToCurrent) {
                ++g_flowDispatchCount;
                if (g_flowDispatchCount == 1 || (g_flowDispatchCount % 120u) == 0)
                    snapframeLog("SnapFrame: bidirectional flow pair accepted count=%llu groups=%ux%u\n",
                        static_cast<unsigned long long>(g_flowDispatchCount), flowGroupsX, flowGroupsY);
                if (g_consistencyAvailable) {
                    const SnapFrameGpuTimer consistencyTimer = beginGpuTimer(
                        context, device, "consistency", nextCapturedFrame, width, height, profileThisFrame);
                    const bool consistencyDispatched =
                        context->dispatchSnapFrameConsistency(
                            g_consistencyShader, g_flowView[0], g_flowView[1],
                            g_lumaSampler, g_consistencyView,
                            (flowWidth + 7u) / 8u, (flowHeight + 7u) / 8u);
                    if (consistencyDispatched && profileThisFrame) context->flushComputeBarriers();
                    endGpuTimer(context, consistencyTimer);
                    if (consistencyDispatched) {
                        ++g_consistencyDispatchCount;
                        if (g_profilingEnabled && (g_flowDispatchCount % 120u) == 0u) {
                            endGpuTimer(context, totalTimer);
                            totalTimerEnded = true;
                            recordFlowReadback(context, device, nextCapturedFrame, flowWidth, flowHeight,
                                (g_flowDispatchCount % 600u) == 0u);
                        }
                        if (g_consistencyDispatchCount == 1
                         || (g_consistencyDispatchCount % 120u) == 0)
                            snapframeLog("SnapFrame: forward/backward consistency accepted count=%llu\n",
                                static_cast<unsigned long long>(g_consistencyDispatchCount));
                    } else {
                        ++g_consistencyFailureCount;
                        if (g_consistencyFailureCount == 1
                         || (g_consistencyFailureCount % 30u) == 0)
                            snapframeLog("SnapFrame: forward/backward consistency failed count=%llu\n",
                                static_cast<unsigned long long>(g_consistencyFailureCount));
                    }
                }
            } else {
                ++g_flowFailureCount;
                if (g_flowFailureCount == 1 || (g_flowFailureCount % 30u) == 0)
                    snapframeLog("SnapFrame: bidirectional flow pair failed count=%llu current_to_previous=%d previous_to_current=%d\n",
                        static_cast<unsigned long long>(g_flowFailureCount),
                        currentToPrevious, previousToCurrent);
            }
        }
    }
    if (!totalTimerEnded)
        endGpuTimer(context, totalTimer);
    ++g_frameIndex;
    const uint64_t captured = g_capturedFrames.fetch_add(1, std::memory_order_relaxed) + 1;
    if (g_diagnosticSuiteEnabled && !g_diagnosticSuiteComplete
        && captured == uint64_t(g_diagnosticPhaseFrames) * 7u)
        logDiagnosticPhase(captured + 1u);
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
        g_flowShader = Rc<DxvkShader>();
        g_flowShaderDevice = nullptr;
        g_consistencyShader = Rc<DxvkShader>();
        g_consistencyShaderDevice = nullptr;
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

extern "C" int snapframe_host_set_gpu_log_path(const char *path) {
    if (!path || !*path) return 0;
    std::lock_guard<std::mutex> lock(g_gpuLogMutex);
    if (g_gpuLogFile) fclose(g_gpuLogFile);
    g_gpuLogFile = fopen(path, "w");
    if (!g_gpuLogFile) {
        fprintf(stderr, "SnapFrame: cannot open GPU timing CSV: %s\n", path);
        return 0;
    }
    fprintf(g_gpuLogFile, "frame,stage,gpu_ms,width,height,status\n");
    fflush(g_gpuLogFile);
    return 1;
}

extern "C" int snapframe_host_set_flow_log_path(const char *path) {
    if (!path || !*path) return 0;
    std::lock_guard<std::mutex> lock(g_flowLogMutex);
    if (g_flowLogFile) fclose(g_flowLogFile);
    g_flowLogFile = fopen(path, "w");
    if (!g_flowLogFile) {
        fprintf(stderr, "SnapFrame: cannot open flow readback CSV: %s\n", path);
        return 0;
    }
    fprintf(g_flowLogFile, "frame,grid_width,grid_height,forward_mean_dx,forward_mean_dy,forward_mean_magnitude,forward_mean_confidence,forward_mean_cost,backward_mean_dx,backward_mean_dy,backward_mean_magnitude,backward_mean_confidence,backward_mean_cost,"
        "forward_conf_lt_0_1,forward_conf_0_1_to_0_5,forward_conf_ge_0_5,forward_conf_ge_0_75,forward_conf_ge_0_9,forward_high_conf_nonzero,forward_boundary_saturated,forward_boundary_low_conf,"
        "backward_conf_lt_0_1,backward_conf_0_1_to_0_5,backward_conf_ge_0_5,backward_conf_ge_0_75,backward_conf_ge_0_9,backward_high_conf_nonzero,backward_boundary_saturated,backward_boundary_low_conf,"
        "consistency_mean,consistency_gt_0_5_pct,nonfinite_values,status\n");
    fflush(g_flowLogFile);
    return 1;
}

extern "C" int snapframe_host_set_flow_vectors_log_path(const char *path) {
    if (!path || !*path) return 0;
    std::lock_guard<std::mutex> lock(g_vectorLogMutex);
    if (g_vectorLogFile) fclose(g_vectorLogFile);
    g_vectorLogFile = fopen(path, "w");
    if (!g_vectorLogFile) {
        fprintf(stderr, "SnapFrame: cannot open flow vector CSV: %s\n", path);
        return 0;
    }
    fprintf(g_vectorLogFile, "frame,x,y,forward_dx,forward_dy,forward_confidence,forward_cost,backward_dx,backward_dy,backward_confidence,backward_cost,consistency\n");
    fflush(g_vectorLogFile);
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
    config->diagnostic_suite = 0;
    config->diagnostic_phase_frames = 1800;
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
    if (validated.diagnostic_phase_frames < 300) validated.diagnostic_phase_frames = 300;
    if (validated.diagnostic_phase_frames > 36000) validated.diagnostic_phase_frames = 36000;
    g_motionAnalysisEnabled = validated.motion_analysis != 0;
    g_diagnosticSuiteEnabled = validated.diagnostic_suite != 0 && g_motionAnalysisEnabled;
    if (validated.diagnostic_suite != 0 && !g_motionAnalysisEnabled)
        snapframeLog("SnapFrame: diagnostic_suite requested but skipped because motion_analysis=0\n");
    g_diagnosticSuiteComplete = false;
    g_diagnosticPhaseFrames = static_cast<uint32_t>(validated.diagnostic_phase_frames);
    g_diagnosticPhase = UINT32_MAX;
    g_profilingEnabled = validated.profiling_enabled != 0;
    g_flowReadbackFailureLogged = false;

    g_enabled.store(validated.enabled ? 1 : 0, std::memory_order_release);
    g_capturedFrames.store(0, std::memory_order_relaxed);
    g_skippedFrames.store(0, std::memory_order_relaxed);
    g_copyCallbackCalls.store(0, std::memory_order_relaxed);
    g_resetHistoryRequested.store(1, std::memory_order_release);
    g_initialized.store(1, std::memory_order_release);

    snapframe_dxvk_set_copy_hook(snapframeCopyFrame);
    snapframe_dxvk_set_device_destroy_hook(snapframeDeviceDestroyed);
    snapframe_dxvk_set_log_hook(snapframeForwardDxvkLog);
    snapframeLog("SnapFrame: diagnostic_suite=%d phase_frames=%u phases=7 profiling=%d motion_analysis=%d timestamp_interval=30 flow_readback_interval=120\n",
        g_diagnosticSuiteEnabled ? 1 : 0, g_diagnosticPhaseFrames,
        g_profilingEnabled ? 1 : 0, g_motionAnalysisEnabled ? 1 : 0);
    snapframeLog( "SnapFrame: host initialized enabled=%d quality=%d flow_scale=%d%% motion_analysis=%d mode=%s\n",
            validated.enabled, validated.quality, validated.flow_scale_percent,
            g_motionAnalysisEnabled ? 1 : 0,
            g_motionAnalysisEnabled ? "capture+luma+bidirectional-flow+fb-consistency" : "capture-only");
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
