#include "snapframe_host.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <new>
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
#include "snap_frame/motion_v2.h"
#include "snap_ai/snap_ai.h"

#if __has_include("generated/snapframe_shaders.h")
  #include "generated/snapframe_shaders.h"
  #define SNAPFRAME_HAS_LUMA_SHADER 1
#else
  #define SNAPFRAME_HAS_LUMA_SHADER 0
#endif

using namespace dxvk;

extern "C" int snapframe_host_apply_backend_config(int, int);
extern "C" int snapframe_engine_adapter_init(const SnapFrameConfig*);
extern "C" void snapframe_engine_adapter_shutdown(void);
extern "C" int snapframe_engine_adapter_update_settings(int, int);
extern "C" int snapframe_engine_adapter_available(void);
extern "C" int snapframe_engine_adapter_process_frame(
    void*, void*, void*, uint32_t, uint32_t, uint32_t);

namespace {
std::mutex g_logMutex;
FILE* g_logFile = nullptr;
std::mutex g_gpuLogMutex;
FILE* g_gpuLogFile = nullptr;
std::mutex g_flowLogMutex;
FILE* g_flowLogFile = nullptr;
std::mutex g_vectorLogMutex;
FILE* g_vectorLogFile = nullptr;
std::mutex g_eventLogMutex;
FILE* g_eventLogFile = nullptr;
uint64_t g_eventLogWriteCount = 0;
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

bool snapframeEventIsRoutineSuccess(const char* event) {
    if (!event) return false;
    static const char* const routineEvents[] = {
        "capture_copy_recorded", "luma_dispatch_ok", "v2_luma_dispatch_ok", "v2_flow_pair_ok",
        "v2_downsample_dispatch_ok", "v2_occlusion_mask_ok", "flow_pair_ok",
        "consistency_dispatch_ok", "midpoint_warp_ok",
        "midpoint_present_requested", "midpoint_present_accepted",
        "midpoint_present_skipped_backpressure"
    };
    for (const char* name : routineEvents)
        if (strcmp(event, name) == 0) return true;
    return false;
}

bool snapframeEventIsCritical(const char* event) {
    if (!event) return false;
    return strstr(event, "failed") != nullptr
        || strstr(event, "rejected") != nullptr
        || strcmp(event, "host_init") == 0
        || strstr(event, "test_phase") != nullptr;
}

void snapframeEvent(uint64_t frame, const char* event, uint32_t width,
                    uint32_t height, const char* detail) {
    /* Per-frame success events used to fflush the CSV on the render path.
     * Keep sparse progress samples, but always preserve failures and lifecycle
     * events. A 64 KiB stdio buffer plus bounded flush cadence avoids turning
     * diagnostic logging into a per-frame storage-I/O bottleneck. */
    const bool routineSuccess = snapframeEventIsRoutineSuccess(event);
    if (routineSuccess && frame != 1u && (frame % 120u) != 0u)
        return;
    if (event && strcmp(event, "frame_complete") == 0
        && frame != 1u && (frame % 300u) != 0u)
        return;

    std::lock_guard<std::mutex> lock(g_eventLogMutex);
    if (!g_eventLogFile) return;
    const int written = fprintf(g_eventLogFile, "%llu,%s,%u,%u,\"%s\"\n",
        static_cast<unsigned long long>(frame), event ? event : "unknown",
        width, height, detail ? detail : "");
    ++g_eventLogWriteCount;
    const bool flushNow = snapframeEventIsCritical(event)
        || (g_eventLogWriteCount % 64u) == 0u;
    if (flushNow) {
        fflush(g_eventLogFile);
        if (written < 0 || ferror(g_eventLogFile))
            snapframeLog("SnapFrame: event CSV write failed event=%s frame=%llu\n",
                event ? event : "unknown", static_cast<unsigned long long>(frame));
    }
}

std::atomic<int> g_initialized{0};
std::atomic<int> g_enabled{0};
std::atomic<uint64_t> g_capturedFrames{0};
std::atomic<uint64_t> g_generatedFrames{0};
std::atomic<uint64_t> g_skippedFrames{0};
std::atomic<int> g_midpointFrameReady{0};
std::atomic<uint64_t> g_copyCallbackCalls{0};
std::atomic<int> g_resetHistoryRequested{0};

DxvkDevice* g_device = nullptr;
Rc<DxvkImage> g_history[2];
Rc<DxvkImageView> g_historyViews[2];
Rc<DxvkImage> g_lumaImage[2];
Rc<DxvkImageView> g_lumaView[2];
// V2 keeps a distinct three-level luminance pyramid per temporal history slot.
// L0 is half source resolution; L1/L2 are successive ceil-divided halves.
Rc<DxvkImage> g_v2PyramidImage[2][3];
Rc<DxvkImageView> g_v2PyramidView[2][3];
Rc<DxvkImage> g_v2FlowImage[2][3];
Rc<DxvkImageView> g_v2FlowView[2][3];
Rc<DxvkImage> g_v2RefinedFlowImage[2];
Rc<DxvkImageView> g_v2RefinedFlowView[2];
Rc<DxvkImage> g_v2ValidationImage;
Rc<DxvkImageView> g_v2ValidationView;
bool g_v2PyramidValid[2] = { false, false };
bool g_v2FlowAvailable = false;
bool g_v2RefinementAvailable = false;
uint64_t g_v2FlowDispatchCount = 0;
uint64_t g_v2RefinementDispatchCount = 0;
Rc<DxvkShader> g_v2LumaShader;
Rc<DxvkShader> g_v2DownsampleShader;
Rc<DxvkShader> g_v2SearchShader;
Rc<DxvkShader> g_v2RefineShader;
Rc<DxvkShader> g_v2ValidationShader;
DxvkDevice* g_v2ShaderDevice = nullptr;
bool g_v2PyramidAvailable = false;
uint64_t g_v2LumaDispatchCount = 0;
uint64_t g_v2DownsampleDispatchCount = 0;
// [0] current->previous, [1] previous->current motion fields.
Rc<DxvkImage> g_flowImage[2];
Rc<DxvkImageView> g_flowView[2];
Rc<DxvkImage> g_consistencyImage;
Rc<DxvkImageView> g_consistencyView;
Rc<DxvkImage> g_midpointImage;
Rc<DxvkImageView> g_midpointView;
Rc<DxvkSampler> g_lumaSampler;
Rc<DxvkSampler> g_midpointSampler;
Rc<DxvkShader> g_lumaShader;
Rc<DxvkShader> g_flowShader;
Rc<DxvkShader> g_consistencyShader;
Rc<DxvkShader> g_midpointShader;
Rc<DxvkShader> g_metricsPartialShader;
Rc<DxvkShader> g_metricsFinalShader;
DxvkDevice* g_metricsShaderDevice = nullptr;
Rc<DxvkBuffer> g_metricsPartialBuffer;
Rc<DxvkBuffer> g_metricsResultBuffer;
static constexpr uint32_t kMetricsReadbackSlots = 3;
static constexpr uint64_t kAiTelemetryIntervalFrames = 30;
Rc<DxvkBuffer> g_metricsReadbackBuffers[kMetricsReadbackSlots];
std::atomic<int> g_metricsReadbackBusy[kMetricsReadbackSlots] = {};
std::atomic<uint64_t> g_metricsReadbackGeneration{1};
std::atomic<uint64_t> g_metricsPublishedFrame{0};
std::mutex g_motionV2Mutex;
SnapMotionV2* g_motionV2 = nullptr;
std::mutex g_snapAiMutex;
SnapAIHandle* g_snapAi = nullptr;
std::mutex g_snapAiMetricsMutex;
struct SnapAIPendingMetrics {
    uint64_t frame = 0;
    float motionMagnitude = 0.0f;
    float motionComplexity = 0.0f;
    float flowConfidence = 0.0f;
    float artifactRisk = 0.0f;
};
SnapAIPendingMetrics g_snapAiPendingMetrics;
std::atomic<uint64_t> g_aiDecisionCount{0};
uint64_t g_aiLastScaleChangeFrame = 0;
static constexpr uint64_t kAiScaleChangeCooldownFrames = 120;

struct alignas(16) SnapFrameMetricsGpuResult {
    uint32_t identity[4];
    uint32_t extent[4];
    uint32_t flags[4];
    float motion[4];
    float quality[4];
    uint32_t debug[4];
};
static_assert(sizeof(SnapFrameMetricsGpuResult) == 96,
    "SnapFrame metrics result ABI must remain 96 bytes");

struct alignas(16) SnapFrameMetricsPush {
    uint32_t identity[4];
    uint32_t extent[4];
    uint32_t counts[4];
};
static_assert(sizeof(SnapFrameMetricsPush) == 48,
    "SnapFrame metrics push-constant ABI must remain 48 bytes");
DxvkDevice* g_lumaShaderDevice = nullptr;
DxvkDevice* g_flowShaderDevice = nullptr;
DxvkDevice* g_consistencyShaderDevice = nullptr;
DxvkDevice* g_midpointShaderDevice = nullptr;
bool g_lumaAvailable = false;
bool g_flowAvailable = false;
bool g_consistencyAvailable = false;
bool g_midpointAvailable = false;
bool g_metricsAvailable = false;
uint32_t g_metricsPartialCount = 0;
uint64_t g_midpointDispatchCount = 0;
uint64_t g_midpointFailureCount = 0;
std::atomic<uint32_t> g_flowScalePercent{50};
std::atomic<int> g_presentMode{SNAPFRAME_PRESENT_MODE_AUTO};
std::atomic<uint64_t> g_lastGpuTimingFrame{0};
std::atomic<float> g_lastGpuTimingMs{0.0f};
std::atomic<float> g_lastGpuFrameIntervalMs{0.0f};
std::atomic<int> g_lastGpuTimingValid{0};
std::atomic<uint32_t> g_v2SearchIntervalFrames{1};
std::atomic<uint32_t> g_v2GpuPressureHighSamples{0};
std::atomic<uint32_t> g_v2GpuPressureLowSamples{0};
uint32_t g_v2SearchIntervalLogged = 1;
std::atomic<float> g_lastFrameIntervalMs{0.0f};
std::atomic<uint64_t> g_lastFrameTimestampNs{0};

uint32_t flowScalePercent() {
    return g_flowScalePercent.load(std::memory_order_acquire);
}

uint32_t flowBlockStride() {
    const uint32_t scale = flowScalePercent();
    return (800u + scale - 1u) / scale;
}

uint32_t flowExtent(uint32_t lumaExtent) {
    const uint32_t stride = flowBlockStride();
    return (lumaExtent + stride - 1u) / stride;
}
uint32_t v2FlowGridExtent(uint32_t lumaExtent) {
    // snap_motion_v2_search.comp uses a fixed 8x8 luma block.
    // Do not reuse flowExtent(): that helper applies the adaptive V1 stride.
    return (lumaExtent + 7u) / 8u;
}
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
uint64_t g_phaseStartMidpoint = 0;
uint64_t g_phaseLastProgress = 0;

const char* diagnosticPhaseName(uint32_t phase) {
    static const char* names[] = { "static_scene", "slow_camera_pan", "fast_camera_rotation",
        "moving_npc_combat", "foliage_particles_water", "hud_inventory_map", "long_run_stress" };
    return phase < 7 ? names[phase] : "invalid";
}

void logDiagnosticPhaseResult(uint32_t phase, uint64_t frames) {
    snapframeEvent(g_capturedFrames.load(std::memory_order_relaxed), "test_phase_result", g_width, g_height, diagnosticPhaseName(phase));
    snapframeLog("SnapFrame: TEST_PHASE_RESULT phase=%s frames=%llu luma_ok=%llu flow_pairs_ok=%llu consistency_ok=%llu midpoint_warp_ok=%llu skipped=%llu luma_fail_total=%llu flow_fail_total=%llu consistency_fail_total=%llu midpoint_warp_fail_total=%llu\n",
        diagnosticPhaseName(phase),
        static_cast<unsigned long long>(frames),
        static_cast<unsigned long long>(g_lumaDispatchCount - g_phaseStartLuma),
        static_cast<unsigned long long>(g_flowDispatchCount - g_phaseStartFlow),
        static_cast<unsigned long long>(g_consistencyDispatchCount - g_phaseStartConsistency),
        static_cast<unsigned long long>(g_midpointDispatchCount - g_phaseStartMidpoint),
        static_cast<unsigned long long>(g_skippedFrames.load(std::memory_order_relaxed) - g_phaseStartSkipped),
        static_cast<unsigned long long>(g_lumaFailureCount),
        static_cast<unsigned long long>(g_flowFailureCount),
        static_cast<unsigned long long>(g_consistencyFailureCount),
        static_cast<unsigned long long>(g_midpointFailureCount));
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
    g_phaseStartMidpoint = g_midpointDispatchCount;
    g_phaseStartSkipped = g_skippedFrames.load(std::memory_order_relaxed);
    g_phaseLastProgress = 0;
    snapframeEvent(captured, "test_phase_begin", g_width, g_height, diagnosticPhaseName(phase));
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
    float frameTimeMs = 0.0f;
    std::string stage;
};

SnapFrameGpuTimer beginGpuTimer(
    DxvkContext* context, DxvkDevice* device, const char* stage,
    uint64_t frame, uint32_t width, uint32_t height, bool sample) {
    SnapFrameGpuTimer timer;
    /* Sparse total-chain samples also feed the runtime budget governor when
     * verbose profiling is disabled. The caller decides which stages sample. */
    if (!sample || !context || !device || !stage)
        return timer;
    timer.begin = device->createGpuQuery(VK_QUERY_TYPE_TIMESTAMP, 0, 0);
    timer.end = device->createGpuQuery(VK_QUERY_TYPE_TIMESTAMP, 0, 0);
    timer.periodNs = device->adapter()->deviceProperties().limits.timestampPeriod;
    timer.frame = frame;
    timer.width = width;
    timer.height = height;
    timer.frameTimeMs = g_lastFrameIntervalMs.load(std::memory_order_acquire);
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
        double gpuMs = 0.0;
        if (valid) {
            gpuMs = double(endData.timestamp.time - beginData.timestamp.time)
                * timer.periodNs / 1000000.0;
            if (timer.stage == "total_chain") {
                g_lastGpuTimingFrame.store(timer.frame, std::memory_order_release);
                g_lastGpuTimingMs.store(static_cast<float>(gpuMs), std::memory_order_release);
                g_lastGpuFrameIntervalMs.store(timer.frameTimeMs, std::memory_order_release);
                g_lastGpuTimingValid.store(1, std::memory_order_release);

                /* V2 search/refine/validation are diagnostic-only: the V1
                 * midpoint path does not consume these fields. Decimate V2
                 * work under sustained GPU pressure without reducing source
                 * frame rate or changing the generated-frame image. */
                uint32_t high = g_v2GpuPressureHighSamples.load(std::memory_order_relaxed);
                uint32_t low = g_v2GpuPressureLowSamples.load(std::memory_order_relaxed);
                if (gpuMs > 6.0) {
                    high = std::min(high + 1u, 8u);
                    g_v2GpuPressureHighSamples.store(high, std::memory_order_relaxed);
                    g_v2GpuPressureLowSamples.store(0u, std::memory_order_relaxed);
                    if (high >= 2u)
                        g_v2SearchIntervalFrames.store(4u, std::memory_order_release);
                } else if (gpuMs < 4.0) {
                    low = std::min(low + 1u, 8u);
                    g_v2GpuPressureLowSamples.store(low, std::memory_order_relaxed);
                    g_v2GpuPressureHighSamples.store(0u, std::memory_order_relaxed);
                    if (low >= 3u)
                        g_v2SearchIntervalFrames.store(1u, std::memory_order_release);
                } else {
                    g_v2GpuPressureHighSamples.store(0u, std::memory_order_relaxed);
                    g_v2GpuPressureLowSamples.store(0u, std::memory_order_relaxed);
                }
            }
        } else if (timer.stage == "total_chain") {
            g_lastGpuTimingFrame.store(timer.frame, std::memory_order_release);
            g_lastGpuTimingMs.store(0.0f, std::memory_order_release);
            g_lastGpuFrameIntervalMs.store(timer.frameTimeMs, std::memory_order_release);
            g_lastGpuTimingValid.store(0, std::memory_order_release);
        }

        std::lock_guard<std::mutex> lock(g_gpuLogMutex);
        if (!g_gpuLogFile) return;
        if (valid) {
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
        const bool boundary = std::fabs(dx) >= 9.5f || std::fabs(dy) >= 9.5f;
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
    /* Motion readback is controlled by motion_analysis, not by the optional
     * seven-phase diagnostic suite. Keeping these switches independent lets
     * normal profiling collect sparse flow statistics without starting a
     * scripted test run. This remains a low-frequency diagnostic readback;
     * Snap AI must consume GPU-reduced scalars, never these pixel buffers. */
    if (!g_profilingEnabled || !g_motionAnalysisEnabled
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
            snapframeEvent(frame, "flow_readback_allocation_failed", flowWidth, flowHeight, "host_visible_coherent_buffer_allocation_failed");
        }
        return;
    }
    if (readback == nullptr || readback->mapPtr(0) == nullptr) {
        if (!g_flowReadbackFailureLogged) {
            g_flowReadbackFailureLogged = true;
            snapframeLog("SnapFrame: flow readback buffer is not host mapped\n");
            snapframeEvent(frame, "flow_readback_map_failed", flowWidth, flowHeight, "staging_buffer_map_ptr_null");
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
    snapframeEvent(frame, "flow_readback_recorded", flowWidth, flowHeight, "two_flow_images_and_consistency_mask_copied_to_staging");

    context->addCompletionCallback([readback, frame, flowWidth, flowHeight,
                                    pixels, flowBytes, maskOffset, dumpVectors](VkResult result) {
        if (result != VK_SUCCESS) {
            snapframeEvent(frame, "flow_readback_completion_failed", flowWidth, flowHeight, "command_list_completion_error");
            if (dumpVectors)
                snapframeLog("SnapFrame: flow vector readback completion failed frame=%llu result=%d\n",
                    static_cast<unsigned long long>(frame), int(result));
            std::lock_guard<std::mutex> lock(g_flowLogMutex);
            if (g_flowLogFile) {
                fprintf(g_flowLogFile,
                    "%llu,%u,%u,"
                    "0,0,0,0,0,0,0,0,0,0,"
                    "0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,"
                    "0,0,0,completion_error:%d\n",
                    static_cast<unsigned long long>(frame), flowWidth, flowHeight, int(result));
                fflush(g_flowLogFile);
            }
            return;
        }
        const uint16_t* data = reinterpret_cast<const uint16_t*>(readback->mapPtr(0));
        if (!data) {
            snapframeEvent(frame, "flow_readback_map_failed", flowWidth, flowHeight, "completion_callback_map_ptr_null");
            if (dumpVectors)
                snapframeLog("SnapFrame: flow vector readback mapping failed frame=%llu\n",
                    static_cast<unsigned long long>(frame));
            std::lock_guard<std::mutex> lock(g_flowLogMutex);
            if (g_flowLogFile) {
                fprintf(g_flowLogFile,
                    "%llu,%u,%u,"
                    "0,0,0,0,0,0,0,0,0,0,"
                    "0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,"
                    "0,0,0,readback_mapping_failed\n",
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
        snapframeEvent(frame, "flow_readback_complete", flowWidth, flowHeight,
            nonFinite ? "readback_ok_contains_nonfinite_values" : "readback_ok_all_samples_finite");

        {
            std::lock_guard<std::mutex> lock(g_flowLogMutex);
            if (g_flowLogFile) {
                fprintf(g_flowLogFile,
                    "%llu,%u,%u,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,"
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
                const bool writeFailed = ferror(g_vectorLogFile) != 0;
                if (writeFailed) snapframeEvent(frame, "flow_vector_csv_write_failed", flowWidth, flowHeight, "ferror_after_vector_dump");
                snapframeLog("SnapFrame: flow vector dump %s frame=%llu rows=%llu grid=%ux%u\n",
                    writeFailed ? "write-error" : "complete",
                    static_cast<unsigned long long>(frame),
                    static_cast<unsigned long long>(pixels), flowWidth, flowHeight);
            } else {
                snapframeLog("SnapFrame: flow vector dump skipped frame=%llu reason=file-not-open\n",
                    static_cast<unsigned long long>(frame));
            }
        }
    });
}

bool recordV1GpuMetrics(
    DxvkContext* context, uint64_t frame,
    uint32_t width, uint32_t height,
    uint32_t flowWidth, uint32_t flowHeight) {
    if (!g_metricsAvailable || !context || g_metricsPartialBuffer == nullptr
        || g_metricsResultBuffer == nullptr || g_flowView[0] == nullptr
        || g_flowView[1] == nullptr || g_consistencyView == nullptr
        || g_lumaSampler == nullptr || !g_metricsPartialCount)
        return false;

    uint32_t slot = kMetricsReadbackSlots;
    for (uint32_t i = 0; i < kMetricsReadbackSlots; ++i) {
        int expected = 0;
        if (g_metricsReadbackBuffers[i] != nullptr
            && g_metricsReadbackBusy[i].compare_exchange_strong(
                expected, 1, std::memory_order_acq_rel)) {
            slot = i;
            break;
        }
    }
    if (slot == kMetricsReadbackSlots) {
        snapframeEvent(frame, "gpu_metrics_skipped", width, height,
            "readback_ring_busy");
        return false;
    }

    const uint64_t generation =
        g_metricsReadbackGeneration.load(std::memory_order_acquire);
    const uint64_t timestampNs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    SnapFrameMetricsPush push{};
    push.identity[0] = static_cast<uint32_t>(frame & 0xffffffffu);
    push.identity[1] = static_cast<uint32_t>(frame >> 32);
    push.identity[2] = static_cast<uint32_t>(timestampNs & 0xffffffffu);
    push.identity[3] = static_cast<uint32_t>(timestampNs >> 32);
    push.extent[0] = width;
    push.extent[1] = height;
    push.extent[2] = flowWidth;
    push.extent[3] = flowHeight;
    push.counts[0] = g_metricsPartialCount;
    push.counts[1] = flowWidth * flowHeight;

    const uint32_t groupsX = (flowWidth + 7u) / 8u;
    const uint32_t groupsY = (flowHeight + 7u) / 8u;
    // The partial buffer is device-local and reused across telemetry samples.
    // Clear it before each reduction so missing workgroup writes cannot be
    // masked by stale values left by an earlier frame.
    context->clearBuffer(g_metricsPartialBuffer, 0,
        VkDeviceSize(g_metricsPartialCount) * 32u, 0u);
    const bool partialDispatched = context->dispatchSnapFrameMetricsPartial(
        g_metricsPartialShader, g_flowView[0], g_flowView[1],
        g_consistencyView, g_lumaSampler,
        DxvkBufferSlice(g_metricsPartialBuffer), groupsX, groupsY);
    if (!partialDispatched) {
        g_metricsReadbackBusy[slot].store(0, std::memory_order_release);
        snapframeEvent(frame, "gpu_metrics_partial_failed", width, height,
            "v1_partial_reduction_dispatch_rejected");
        return false;
    }

    context->flushComputeBarriers();
    const bool finalDispatched = context->dispatchSnapFrameMetricsFinal(
        g_metricsFinalShader, DxvkBufferSlice(g_metricsPartialBuffer),
        DxvkBufferSlice(g_metricsResultBuffer), sizeof(push), &push);
    if (!finalDispatched) {
        g_metricsReadbackBusy[slot].store(0, std::memory_order_release);
        snapframeEvent(frame, "gpu_metrics_final_failed", width, height,
            "v1_final_reduction_dispatch_rejected");
        return false;
    }

    Rc<DxvkBuffer> readback = g_metricsReadbackBuffers[slot];
    context->copyBuffer(readback, 0, g_metricsResultBuffer, 0,
        sizeof(SnapFrameMetricsGpuResult));
    context->addCompletionCallback([readback, slot, generation, frame,
                                    width, height, flowWidth, flowHeight,
                                    timestampNs](VkResult completion) {
        bool accepted = false;
        float loggedConfidence = 0.0f;
        float loggedComplexity = 0.0f;
        float loggedArtifactRisk = 0.0f;
        const char* rejectReason = "command_list_completion_error";
        const bool currentGeneration = generation == g_metricsReadbackGeneration.load(
            std::memory_order_acquire);
        if (!currentGeneration) {
            rejectReason = "stale_resource_generation";
        } else if (completion == VK_SUCCESS && readback != nullptr) {
            const auto* gpu = reinterpret_cast<const SnapFrameMetricsGpuResult*>(
                readback->mapPtr(0));
            if (gpu != nullptr) {
                const uint64_t gpuFrame = uint64_t(gpu->identity[0])
                    | (uint64_t(gpu->identity[1]) << 32);
                const uint64_t gpuTimestamp = uint64_t(gpu->identity[2])
                    | (uint64_t(gpu->identity[3]) << 32);
                loggedConfidence = gpu->motion[2];
                loggedComplexity = gpu->motion[1];
                loggedArtifactRisk = gpu->quality[2];
                const bool finite = std::isfinite(gpu->motion[0])
                    && std::isfinite(gpu->motion[1])
                    && std::isfinite(gpu->motion[2])
                    && std::isfinite(gpu->motion[3])
                    && std::isfinite(gpu->quality[0])
                    && std::isfinite(gpu->quality[1])
                    && std::isfinite(gpu->quality[2])
                    && std::isfinite(gpu->quality[3]);
                const bool identityValid = gpuFrame == frame
                    && gpuTimestamp == timestampNs
                    && gpu->extent[0] == width && gpu->extent[1] == height
                    && gpu->extent[2] == flowWidth && gpu->extent[3] == flowHeight;
                const float reportedCoverage = gpu->flags[2] > 0u
                    ? float(gpu->flags[3]) / float(gpu->flags[2]) : 0.0f;
                const bool partialDispatchValid = gpu->debug[3] == gpu->flags[1]
                    && gpu->debug[0] == gpu->debug[3]
                    && gpu->debug[1] > 0u
                    && gpu->debug[1] <= gpu->debug[2]
                    && gpu->debug[2] <= 64u;
                const bool coverageValid = gpu->quality[3] >= 0.90f
                    && gpu->quality[3] <= 1.001f
                    && gpu->flags[3] <= gpu->flags[2]
                    && std::abs(gpu->quality[3] - reportedCoverage) <= 0.02f
                    && partialDispatchValid;
                if (finite && identityValid && coverageValid
                    && gpu->flags[0] == 1u) {
                    const uint64_t previous = g_metricsPublishedFrame.load(
                        std::memory_order_acquire);
                    if (frame > previous) {
                        SnapMotionV2Metrics metrics{};
                        metrics.struct_size = sizeof(metrics);
                        metrics.frame_id = frame;
                        metrics.timestamp_ns = timestampNs;
                        metrics.width = width;
                        metrics.height = height;
                        metrics.motion_magnitude_px = gpu->motion[0];
                        metrics.motion_complexity = gpu->motion[1];
                        metrics.flow_confidence = gpu->motion[2];
                        metrics.low_confidence_ratio = gpu->motion[3];
                        metrics.occlusion_ratio = gpu->quality[0];
                        metrics.static_ratio = 0.0f; // V1 has no static-region mask.
                        metrics.artifact_risk = gpu->quality[2];
                        metrics.scene_cut = 0; // No calibrated scene-cut detector yet.
                        metrics.telemetry_valid = 1;

                        /* Defer AI evaluation until the next serialized render
                         * callback. The total-chain timestamp completion callback
                         * is registered after this readback callback, so evaluating
                         * AI here would race and usually see no same-frame timing. */
                        {
                            std::lock_guard<std::mutex> aiMetricsLock(g_snapAiMetricsMutex);
                            if (frame > g_snapAiPendingMetrics.frame) {
                                g_snapAiPendingMetrics.frame = frame;
                                g_snapAiPendingMetrics.motionMagnitude = gpu->motion[0];
                                g_snapAiPendingMetrics.motionComplexity = gpu->motion[1];
                                g_snapAiPendingMetrics.flowConfidence = gpu->motion[2];
                                g_snapAiPendingMetrics.artifactRisk = gpu->quality[2];
                            }
                        }

                        std::lock_guard<std::mutex> lock(g_motionV2Mutex);
                        if (g_motionV2 != nullptr
                            && snap_motion_v2_publish_metrics(g_motionV2, &metrics)
                                == SNAP_MOTION_V2_OK) {
                            g_metricsPublishedFrame.store(frame,
                                std::memory_order_release);
                            accepted = true;
                            rejectReason = nullptr;
                        } else {
                            rejectReason = "motion_v2_publish_rejected";
                        }
                    } else {
                        rejectReason = "out_of_order_frame";
                    }
                } else if (!finite) {
                    rejectReason = "non_finite_metrics";
                } else if (!identityValid) {
                    rejectReason = "frame_identity_or_extent_mismatch";
                } else if (!coverageValid || gpu->flags[0] != 1u) {
                    rejectReason = gpu->flags[3] > gpu->flags[2]
                        || std::abs(gpu->quality[3] - reportedCoverage) > 0.02f
                        || !partialDispatchValid
                        ? "metrics_grid_coverage_mismatch"
                        : "low_valid_flow_coverage";
                    if (frame == 30u || (frame % 300u) == 0u) {
                        snapframeLog("SnapFrame: GPU metrics rejected frame=%llu partial_count=%u valid_cells=%u expected_cells=%u coverage=%.3f confidence=%.3f active_partials=%u/%u cells_per_partial=%u..%u\n",
                            static_cast<unsigned long long>(frame),
                            gpu->flags[1], gpu->flags[3], gpu->flags[2],
                            gpu->quality[3], gpu->motion[2],
                            gpu->debug[0], gpu->debug[3],
                            gpu->debug[1], gpu->debug[2]);
                    }
                }
            } else {
                rejectReason = "readback_map_failed";
            }
        }

        if (currentGeneration) {
            g_metricsReadbackBusy[slot].store(0, std::memory_order_release);
        }
        if (accepted) {
            if (frame == 1u || (frame % 120u) == 0u)
                snapframeLog("SnapFrame: GPU metrics published frame=%llu confidence=%.3f complexity=%.3f artifact=%.3f\n",
                    static_cast<unsigned long long>(frame),
                    loggedConfidence, loggedComplexity, loggedArtifactRisk);
            snapframeEvent(frame, "gpu_metrics_published", width, height,
                "v1_scalar_record_validated_and_motion_v2_accepted");
        } else {
            snapframeEvent(frame, "gpu_metrics_rejected", width, height,
                rejectReason ? rejectReason : "unknown");
        }
    });
    snapframeEvent(frame, "gpu_metrics_copy_recorded", width, height,
        "96_byte_frame_tagged_scalar_record_with_partial_dispatch_diagnostics_async_readback");
    return true;
}


void clearHistory() {
    g_midpointFrameReady.store(0, std::memory_order_release);
    g_historyViews[0] = Rc<DxvkImageView>();
    g_historyViews[1] = Rc<DxvkImageView>();
    g_history[0] = Rc<DxvkImage>();
    g_history[1] = Rc<DxvkImage>();
    g_lumaView[0] = Rc<DxvkImageView>();
    g_lumaView[1] = Rc<DxvkImageView>();
    g_lumaImage[0] = Rc<DxvkImage>();
    g_lumaImage[1] = Rc<DxvkImage>();
    for (unsigned slot = 0; slot < 2; ++slot) {
        for (unsigned level = 0; level < 3; ++level) {
            g_v2PyramidView[slot][level] = Rc<DxvkImageView>();
            g_v2PyramidImage[slot][level] = Rc<DxvkImage>();
        }
    }
    g_v2PyramidAvailable = false;
    g_v2PyramidValid[0] = g_v2PyramidValid[1] = false;
    g_v2FlowAvailable = false;
    g_v2FlowDispatchCount = 0;
    g_v2SearchIntervalFrames.store(1u, std::memory_order_relaxed);
    g_v2GpuPressureHighSamples.store(0u, std::memory_order_relaxed);
    g_v2GpuPressureLowSamples.store(0u, std::memory_order_relaxed);
    g_v2SearchIntervalLogged = 1u;
    g_v2RefinementAvailable = false;
    g_v2RefinementDispatchCount = 0;
    g_v2ValidationView = Rc<DxvkImageView>();
    g_v2ValidationImage = Rc<DxvkImage>();
    for (unsigned direction = 0; direction < 2; ++direction) {
        g_v2RefinedFlowView[direction] = Rc<DxvkImageView>();
        g_v2RefinedFlowImage[direction] = Rc<DxvkImage>();
        for (unsigned level = 0; level < 3; ++level) {
            g_v2FlowView[direction][level] = Rc<DxvkImageView>();
            g_v2FlowImage[direction][level] = Rc<DxvkImage>();
        }
    }
    g_v2LumaDispatchCount = 0;
    g_v2DownsampleDispatchCount = 0;
    g_flowView[0] = Rc<DxvkImageView>();
    g_flowView[1] = Rc<DxvkImageView>();
    g_flowImage[0] = Rc<DxvkImage>();
    g_flowImage[1] = Rc<DxvkImage>();
    g_consistencyView = Rc<DxvkImageView>();
    g_consistencyImage = Rc<DxvkImage>();
    g_midpointView = Rc<DxvkImageView>();
    g_midpointImage = Rc<DxvkImage>();
    g_metricsPartialBuffer = Rc<DxvkBuffer>();
    g_metricsResultBuffer = Rc<DxvkBuffer>();
    for (uint32_t i = 0; i < kMetricsReadbackSlots; ++i) {
        g_metricsReadbackBuffers[i] = Rc<DxvkBuffer>();
        g_metricsReadbackBusy[i].store(0, std::memory_order_release);
    }
    g_metricsReadbackGeneration.fetch_add(1, std::memory_order_acq_rel);
    g_metricsPublishedFrame.store(0, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(g_motionV2Mutex);
        if (g_motionV2)
            snap_motion_v2_reset(g_motionV2, SNAP_MOTION_V2_RESET_SWAPCHAIN);
    }
    g_midpointAvailable = false;
    g_metricsAvailable = false;
    g_metricsPartialCount = 0;
    g_metricsPartialShader = Rc<DxvkShader>();
    g_metricsFinalShader = Rc<DxvkShader>();
    g_metricsShaderDevice = nullptr;
    g_midpointDispatchCount = 0;
    g_midpointFailureCount = 0;
    g_lumaSampler = Rc<DxvkSampler>();
    g_midpointSampler = Rc<DxvkSampler>();
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
    g_phaseStartConsistency = g_phaseStartSkipped = g_phaseStartMidpoint = 0;
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
            DxvkSamplerCreateInfo midpointSamplerInfo = samplerInfo;
            midpointSamplerInfo.magFilter = VK_FILTER_LINEAR;
            midpointSamplerInfo.minFilter = VK_FILTER_LINEAR;
            g_midpointSampler = device->createSampler(midpointSamplerInfo);

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

            // V2 pyramid resources are deliberately separate from the V1 luma
            // and flow images. Every level has its own image and view so that
            // storage writes and sampled reads have explicit per-image tracking.
            if (lumaFormatSupported) {
                const uint32_t v2Widths[3] = {
                    lumaWidth, lumaWidth / 2u + lumaWidth % 2u,
                    (lumaWidth / 2u + lumaWidth % 2u) / 2u
                        + ((lumaWidth / 2u + lumaWidth % 2u) % 2u)
                };
                const uint32_t v2Heights[3] = {
                    lumaHeight, lumaHeight / 2u + lumaHeight % 2u,
                    (lumaHeight / 2u + lumaHeight % 2u) / 2u
                        + ((lumaHeight / 2u + lumaHeight % 2u) % 2u)
                };
                bool v2ImagesCreated = true;
                DxvkImageCreateInfo v2Info{};
                v2Info.type = VK_IMAGE_TYPE_2D;
                v2Info.format = VK_FORMAT_R16_SFLOAT;
                v2Info.flags = 0;
                v2Info.sampleCount = VK_SAMPLE_COUNT_1_BIT;
                v2Info.numLayers = 1;
                v2Info.mipLevels = 1;
                v2Info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
                v2Info.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
                v2Info.access = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
                v2Info.tiling = VK_IMAGE_TILING_OPTIMAL;
                v2Info.layout = VK_IMAGE_LAYOUT_GENERAL;
                v2Info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                v2Info.shared = VK_FALSE;
                DxvkImageViewCreateInfo v2ViewInfo{};
                v2ViewInfo.type = VK_IMAGE_VIEW_TYPE_2D;
                v2ViewInfo.format = VK_FORMAT_R16_SFLOAT;
                v2ViewInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
                v2ViewInfo.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
                v2ViewInfo.minLevel = 0;
                v2ViewInfo.numLevels = 1;
                v2ViewInfo.minLayer = 0;
                v2ViewInfo.numLayers = 1;
                for (unsigned slot = 0; slot < 2; ++slot) {
                    for (unsigned level = 0; level < 3; ++level) {
                        v2Info.extent = { v2Widths[level], v2Heights[level], 1 };
                        g_v2PyramidImage[slot][level] = device->createImage(
                            v2Info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                        if (g_v2PyramidImage[slot][level] != nullptr)
                            g_v2PyramidView[slot][level] = device->createImageView(
                                g_v2PyramidImage[slot][level], v2ViewInfo);
                        v2ImagesCreated = v2ImagesCreated
                            && g_v2PyramidImage[slot][level] != nullptr
                            && g_v2PyramidView[slot][level] != nullptr;
                    }
                }

                /* Initialize shader objects once per device. A failed optional
                 * stage must not trigger repeated shader creation on every Present. */
                const bool needV2ShaderRegistration = g_v2ShaderDevice != device;
                if (needV2ShaderRegistration) {
                    g_v2LumaShader = Rc<DxvkShader>();
                    g_v2DownsampleShader = Rc<DxvkShader>();
                    g_v2SearchShader = Rc<DxvkShader>();
                    g_v2RefineShader = Rc<DxvkShader>();
                    g_v2ValidationShader = Rc<DxvkShader>();
                    g_v2ShaderDevice = nullptr;
                    const DxvkResourceSlot v2LumaSlots[] = {
                        { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                          VK_IMAGE_VIEW_TYPE_2D, VK_ACCESS_SHADER_READ_BIT },
                        { 4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                          VK_IMAGE_VIEW_TYPE_2D, VK_ACCESS_SHADER_WRITE_BIT },
                    };
                    DxvkInterfaceSlots v2LumaIface{};
                    v2LumaIface.pushConstOffset = 0;
                    v2LumaIface.pushConstSize = 16;
                    SpirvCodeBuffer v2LumaCode(
                        snapframe::shaders::k_snap_motion_v2_luma_word_count,
                        snapframe::shaders::k_snap_motion_v2_luma);
                    g_v2LumaShader = device->createShader(
                        VK_SHADER_STAGE_COMPUTE_BIT, 2, v2LumaSlots,
                        v2LumaIface, v2LumaCode);
                    const DxvkResourceSlot v2DownsampleSlots[] = {
                        { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                          VK_IMAGE_VIEW_TYPE_2D, VK_ACCESS_SHADER_READ_BIT },
                        { 4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                          VK_IMAGE_VIEW_TYPE_2D, VK_ACCESS_SHADER_WRITE_BIT },
                    };
                    DxvkInterfaceSlots v2DownsampleIface{};
                    v2DownsampleIface.pushConstOffset = 0;
                    v2DownsampleIface.pushConstSize = 16;
                    SpirvCodeBuffer v2DownsampleCode(
                        snapframe::shaders::k_snap_motion_v2_downsample_word_count,
                        snapframe::shaders::k_snap_motion_v2_downsample);
                    g_v2DownsampleShader = device->createShader(
                        VK_SHADER_STAGE_COMPUTE_BIT, 2, v2DownsampleSlots,
                        v2DownsampleIface, v2DownsampleCode);
                    const DxvkResourceSlot v2SearchSlots[] = {
                        { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_IMAGE_VIEW_TYPE_2D, VK_ACCESS_SHADER_READ_BIT },
                        { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_IMAGE_VIEW_TYPE_2D, VK_ACCESS_SHADER_READ_BIT },
                        { 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_IMAGE_VIEW_TYPE_2D, VK_ACCESS_SHADER_READ_BIT },
                        { 4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_IMAGE_VIEW_TYPE_2D, VK_ACCESS_SHADER_WRITE_BIT },
                    };
                    DxvkInterfaceSlots v2SearchIface{};
                    v2SearchIface.pushConstOffset = 0;
                    v2SearchIface.pushConstSize = 48;
                    SpirvCodeBuffer v2SearchCode(
                        snapframe::shaders::k_snap_motion_v2_search_word_count,
                        snapframe::shaders::k_snap_motion_v2_search);
                    g_v2SearchShader = device->createShader(
                        VK_SHADER_STAGE_COMPUTE_BIT, 4, v2SearchSlots,
                        v2SearchIface, v2SearchCode);
                    const DxvkResourceSlot v2RefineSlots[] = {
                        { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_IMAGE_VIEW_TYPE_2D, VK_ACCESS_SHADER_READ_BIT },
                        { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_IMAGE_VIEW_TYPE_2D, VK_ACCESS_SHADER_READ_BIT },
                        { 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_IMAGE_VIEW_TYPE_2D, VK_ACCESS_SHADER_READ_BIT },
                        { 4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_IMAGE_VIEW_TYPE_2D, VK_ACCESS_SHADER_WRITE_BIT },
                    };
                    DxvkInterfaceSlots v2RefineIface{};
                    v2RefineIface.pushConstOffset = 0;
                    v2RefineIface.pushConstSize = 32;
                    SpirvCodeBuffer v2RefineCode(
                        snapframe::shaders::k_snap_motion_v2_refine_word_count,
                        snapframe::shaders::k_snap_motion_v2_refine);
                    g_v2RefineShader = device->createShader(
                        VK_SHADER_STAGE_COMPUTE_BIT, 4, v2RefineSlots,
                        v2RefineIface, v2RefineCode);
                    const DxvkResourceSlot v2ValidationSlots[] = {
                        { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_IMAGE_VIEW_TYPE_2D, VK_ACCESS_SHADER_READ_BIT },
                        { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_IMAGE_VIEW_TYPE_2D, VK_ACCESS_SHADER_READ_BIT },
                        { 4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_IMAGE_VIEW_TYPE_2D, VK_ACCESS_SHADER_WRITE_BIT },
                    };
                    DxvkInterfaceSlots v2ValidationIface{};
                    v2ValidationIface.pushConstOffset = 0;
                    v2ValidationIface.pushConstSize = 32;
                    SpirvCodeBuffer v2ValidationCode(
                        snapframe::shaders::k_snap_motion_v2_validation_word_count,
                        snapframe::shaders::k_snap_motion_v2_validation);
                    g_v2ValidationShader = device->createShader(
                        VK_SHADER_STAGE_COMPUTE_BIT, 3, v2ValidationSlots,
                        v2ValidationIface, v2ValidationCode);
                }
                g_v2PyramidAvailable = v2ImagesCreated
                    && g_lumaSampler != nullptr
                    && g_v2LumaShader != nullptr
                    && g_v2DownsampleShader != nullptr;
                if (needV2ShaderRegistration) {
                    if (g_v2LumaShader != nullptr) device->registerShader(g_v2LumaShader);
                    if (g_v2DownsampleShader != nullptr) device->registerShader(g_v2DownsampleShader);
                    if (g_v2SearchShader != nullptr) device->registerShader(g_v2SearchShader);
                    if (g_v2RefineShader != nullptr) device->registerShader(g_v2RefineShader);
                    if (g_v2ValidationShader != nullptr) device->registerShader(g_v2ValidationShader);
                    /* Remember this device even if an optional shader failed. */
                    g_v2ShaderDevice = device;
                }
                if (!g_v2PyramidAvailable) {
                    snapframeLog("SnapFrame: V2 pyramid unavailable (images=%u luma=%u downsample=%u sampler=%u); V1 path remains active\n",
                        unsigned(v2ImagesCreated), unsigned(g_v2LumaShader != nullptr),
                        unsigned(g_v2DownsampleShader != nullptr), unsigned(g_lumaSampler != nullptr));
                } else {
                    snapframeLog("SnapFrame: V2 pyramid resources ready L0=%ux%u L1=%ux%u L2=%ux%u slots=2 format=R16F\n",
                        v2Widths[0], v2Heights[0], v2Widths[1], v2Heights[1],
                        v2Widths[2], v2Heights[2]);
                }
            }

            const VkFormat flowFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
            const VkFormatProperties flowProperties = device->adapter()->formatProperties(flowFormat);
            const VkFormatFeatureFlags flowRequired = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
            VkImageFormatProperties flowImageProperties{};
            const VkResult flowImageResult = device->adapter()->imageFormatProperties(
                flowFormat, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, flowImageProperties);
            const uint32_t flowWidth = flowExtent(lumaWidth);
            const uint32_t flowHeight = flowExtent(lumaHeight);
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

                bool v2FlowImagesCreated = true;
                const uint32_t v2LumaWidths[3] = {
                    lumaWidth, lumaWidth / 2u + lumaWidth % 2u,
                    (lumaWidth / 2u + lumaWidth % 2u) / 2u
                        + ((lumaWidth / 2u + lumaWidth % 2u) % 2u)
                };
                const uint32_t v2LumaHeights[3] = {
                    lumaHeight, lumaHeight / 2u + lumaHeight % 2u,
                    (lumaHeight / 2u + lumaHeight % 2u) / 2u
                        + ((lumaHeight / 2u + lumaHeight % 2u) % 2u)
                };
                for (unsigned direction = 0; direction < 2; ++direction) {
                    for (unsigned level = 0; level < 3; ++level) {
                        DxvkImageCreateInfo v2FlowInfo = flowInfo;
                        v2FlowInfo.extent = {
                            v2FlowGridExtent(v2LumaWidths[level]),
                            v2FlowGridExtent(v2LumaHeights[level]), 1
                        };
                        g_v2FlowImage[direction][level] = device->createImage(
                            v2FlowInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                        if (g_v2FlowImage[direction][level] != nullptr)
                            g_v2FlowView[direction][level] = device->createImageView(
                                g_v2FlowImage[direction][level], flowViewInfo);
                        v2FlowImagesCreated = v2FlowImagesCreated
                            && g_v2FlowImage[direction][level] != nullptr
                            && g_v2FlowView[direction][level] != nullptr;
                    }
                }
                g_v2FlowAvailable = v2FlowImagesCreated
                    && g_v2SearchShader != nullptr && g_lumaSampler != nullptr;
                if (!g_v2FlowAvailable) {
                    snapframeLog("SnapFrame: V2 flow search unavailable (images=%u search=%u sampler=%u); pyramid remains independently available\n",
                        unsigned(v2FlowImagesCreated), unsigned(g_v2SearchShader != nullptr),
                        unsigned(g_lumaSampler != nullptr));
                }
                if (g_v2FlowAvailable) {
                    snapframeLog("SnapFrame: V2 flow grids ready L0=%ux%u L1=%ux%u L2=%ux%u directions=2 format=RGBA16F\n",
                        v2FlowGridExtent(v2LumaWidths[0]), v2FlowGridExtent(v2LumaHeights[0]),
                        v2FlowGridExtent(v2LumaWidths[1]), v2FlowGridExtent(v2LumaHeights[1]),
                        v2FlowGridExtent(v2LumaWidths[2]), v2FlowGridExtent(v2LumaHeights[2]));
                    for (unsigned direction = 0; direction < 2; ++direction) {
                        DxvkImageCreateInfo refinedInfo = flowInfo;
                        refinedInfo.extent = {
                            v2FlowGridExtent(v2LumaWidths[0]),
                            v2FlowGridExtent(v2LumaHeights[0]), 1
                        };
                        g_v2RefinedFlowImage[direction] = device->createImage(
                            refinedInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                        if (g_v2RefinedFlowImage[direction] != nullptr)
                            g_v2RefinedFlowView[direction] = device->createImageView(
                                g_v2RefinedFlowImage[direction], flowViewInfo);
                    }
                    DxvkImageCreateInfo validationInfo{};
                    validationInfo.type = VK_IMAGE_TYPE_2D;
                    validationInfo.format = VK_FORMAT_R16_SFLOAT;
                    validationInfo.flags = 0;
                    validationInfo.sampleCount = VK_SAMPLE_COUNT_1_BIT;
                    validationInfo.extent = {
                        v2FlowGridExtent(v2LumaWidths[0]),
                        v2FlowGridExtent(v2LumaHeights[0]), 1
                    };
                    validationInfo.numLayers = 1;
                    validationInfo.mipLevels = 1;
                    validationInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
                    validationInfo.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
                    validationInfo.access = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
                    validationInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
                    validationInfo.layout = VK_IMAGE_LAYOUT_GENERAL;
                    validationInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                    validationInfo.shared = VK_FALSE;
                    DxvkImageViewCreateInfo validationViewInfo{};
                    validationViewInfo.type = VK_IMAGE_VIEW_TYPE_2D;
                    validationViewInfo.format = VK_FORMAT_R16_SFLOAT;
                    validationViewInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
                    validationViewInfo.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
                    validationViewInfo.minLevel = 0;
                    validationViewInfo.numLevels = 1;
                    validationViewInfo.minLayer = 0;
                    validationViewInfo.numLayers = 1;
                    g_v2ValidationImage = device->createImage(
                        validationInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                    if (g_v2ValidationImage != nullptr)
                        g_v2ValidationView = device->createImageView(
                            g_v2ValidationImage, validationViewInfo);
                    g_v2RefinementAvailable =
                        g_v2RefinedFlowView[0] != nullptr
                        && g_v2RefinedFlowView[1] != nullptr
                        && g_v2ValidationView != nullptr
                        && g_v2RefineShader != nullptr
                        && g_v2ValidationShader != nullptr;
                    if (!g_v2RefinementAvailable)
                        snapframeLog("SnapFrame: V2 refinement/validation resources unavailable; coarse flow retained for diagnostics only\n");
                }

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
                            { 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_IMAGE_VIEW_TYPE_2D, VK_ACCESS_SHADER_READ_BIT },
                        };
                        DxvkInterfaceSlots consistencyIface{};
                        SpirvCodeBuffer consistencyCode(
                            snapframe::shaders::k_flow_consistency_word_count,
                            snapframe::shaders::k_flow_consistency);
                        g_consistencyShader = device->createShader(
                            VK_SHADER_STAGE_COMPUTE_BIT, 4, consistencySlots,
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
                        snapframeLog("SnapFrame: forward/backward consistency resources unavailable; midpoint reconstruction disabled\n");

                    const VkFormat midpointFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
                    const VkFormatProperties midpointProperties =
                        device->adapter()->formatProperties(midpointFormat);
                    const VkFormatFeatureFlags midpointRequired =
                        VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
                    VkImageFormatProperties midpointImageProperties{};
                    const VkResult midpointImageResult = device->adapter()->imageFormatProperties(
                        midpointFormat, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        0, midpointImageProperties);
                    const bool midpointSupported =
                        (midpointProperties.optimalTilingFeatures & midpointRequired) == midpointRequired
                        && midpointImageResult == VK_SUCCESS
                        && device->features().core.features.shaderStorageImageExtendedFormats
                        && width <= midpointImageProperties.maxExtent.width
                        && height <= midpointImageProperties.maxExtent.height;
                    if (g_consistencyAvailable && midpointSupported) {
                        DxvkImageCreateInfo midpointInfo{};
                        midpointInfo.type = VK_IMAGE_TYPE_2D;
                        midpointInfo.format = midpointFormat;
                        midpointInfo.flags = 0;
                        midpointInfo.sampleCount = VK_SAMPLE_COUNT_1_BIT;
                        midpointInfo.extent = { width, height, 1 };
                        midpointInfo.numLayers = 1;
                        midpointInfo.mipLevels = 1;
                        midpointInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
                        midpointInfo.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
                        midpointInfo.access = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
                        midpointInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
                        midpointInfo.layout = VK_IMAGE_LAYOUT_GENERAL;
                        midpointInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                        midpointInfo.shared = VK_FALSE;
                        g_midpointImage = device->createImage(
                            midpointInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                        DxvkImageViewCreateInfo midpointViewInfo{};
                        midpointViewInfo.type = VK_IMAGE_VIEW_TYPE_2D;
                        midpointViewInfo.format = midpointFormat;
                        midpointViewInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
                        midpointViewInfo.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
                        midpointViewInfo.minLevel = 0;
                        midpointViewInfo.numLevels = 1;
                        midpointViewInfo.minLayer = 0;
                        midpointViewInfo.numLayers = 1;
                        if (g_midpointImage != nullptr)
                            g_midpointView = device->createImageView(g_midpointImage, midpointViewInfo);

                        const bool needMidpointRegistration =
                            g_midpointShaderDevice != device || g_midpointShader == nullptr;
                        if (needMidpointRegistration) {
                            g_midpointShader = Rc<DxvkShader>();
                            g_midpointShaderDevice = nullptr;
                            const DxvkResourceSlot midpointSlots[] = {
                                { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_IMAGE_VIEW_TYPE_2D, VK_ACCESS_SHADER_READ_BIT },
                                { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_IMAGE_VIEW_TYPE_2D, VK_ACCESS_SHADER_READ_BIT },
                                { 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_IMAGE_VIEW_TYPE_2D, VK_ACCESS_SHADER_READ_BIT },
                                { 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_IMAGE_VIEW_TYPE_2D, VK_ACCESS_SHADER_READ_BIT },
                                { 4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_IMAGE_VIEW_TYPE_2D, VK_ACCESS_SHADER_READ_BIT },
                                { 5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_IMAGE_VIEW_TYPE_2D, VK_ACCESS_SHADER_WRITE_BIT },
                            };
                            DxvkInterfaceSlots midpointIface{};
                            SpirvCodeBuffer midpointCode(
                                snapframe::shaders::k_midpoint_warp_word_count,
                                snapframe::shaders::k_midpoint_warp);
                            g_midpointShader = device->createShader(
                                VK_SHADER_STAGE_COMPUTE_BIT, 6, midpointSlots,
                                midpointIface, midpointCode);
                        }
                        g_midpointAvailable = g_midpointImage != nullptr
                            && g_midpointView != nullptr && g_midpointSampler != nullptr
                            && g_midpointShader != nullptr;
                        if (g_midpointAvailable && needMidpointRegistration) {
                            device->registerShader(g_midpointShader);
                            g_midpointShaderDevice = device;
                        }
                    } else {
                        g_midpointAvailable = false;
                        snapframeLog("SnapFrame: midpoint output format unsupported or consistency unavailable result=%d features=0x%x; warp disabled\n",
                            int(midpointImageResult), unsigned(midpointProperties.optimalTilingFeatures));
                    }
                    // Metrics use the live V1 flow and consistency images only.
                    // They are independent of midpoint reconstruction availability.
                    try {
                        const bool needMetricsShaderRegistration =
                            g_metricsShaderDevice != device
                            || g_metricsPartialShader == nullptr
                            || g_metricsFinalShader == nullptr;
                        if (needMetricsShaderRegistration) {
                            g_metricsPartialShader = Rc<DxvkShader>();
                            g_metricsFinalShader = Rc<DxvkShader>();
                            g_metricsShaderDevice = nullptr;
                            const DxvkResourceSlot partialSlots[] = {
                                { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_IMAGE_VIEW_TYPE_2D, VK_ACCESS_SHADER_READ_BIT },
                                { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_IMAGE_VIEW_TYPE_2D, VK_ACCESS_SHADER_READ_BIT },
                                { 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_IMAGE_VIEW_TYPE_2D, VK_ACCESS_SHADER_READ_BIT },
                                { 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_IMAGE_VIEW_TYPE_2D, VK_ACCESS_SHADER_WRITE_BIT },
                            };
                            DxvkInterfaceSlots partialIface{};
                            SpirvCodeBuffer partialCode(
                                snapframe::shaders::k_v1_metrics_partial_word_count,
                                snapframe::shaders::k_v1_metrics_partial);
                            g_metricsPartialShader = device->createShader(
                                VK_SHADER_STAGE_COMPUTE_BIT, 4, partialSlots,
                                partialIface, partialCode);

                            const DxvkResourceSlot finalSlots[] = {
                                { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_IMAGE_VIEW_TYPE_2D, VK_ACCESS_SHADER_READ_BIT },
                                { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_IMAGE_VIEW_TYPE_2D, VK_ACCESS_SHADER_WRITE_BIT },
                            };
                            DxvkInterfaceSlots finalIface{};
                            finalIface.pushConstOffset = 0;
                            finalIface.pushConstSize = sizeof(SnapFrameMetricsPush);
                            SpirvCodeBuffer finalCode(
                                snapframe::shaders::k_v1_metrics_final_word_count,
                                snapframe::shaders::k_v1_metrics_final);
                            g_metricsFinalShader = device->createShader(
                                VK_SHADER_STAGE_COMPUTE_BIT, 2, finalSlots,
                                finalIface, finalCode);
                        }

                        if (g_metricsPartialShader != nullptr
                            && g_metricsFinalShader != nullptr
                            && needMetricsShaderRegistration) {
                            device->registerShader(g_metricsPartialShader);
                            device->registerShader(g_metricsFinalShader);
                            g_metricsShaderDevice = device;
                        }

                        const uint32_t metricsGroupsX = (flowWidth + 7u) / 8u;
                        const uint32_t metricsGroupsY = (flowHeight + 7u) / 8u;
                        g_metricsPartialCount = metricsGroupsX * metricsGroupsY;
                        const VkDeviceSize partialBytes =
                            VkDeviceSize(g_metricsPartialCount) * 32u;
                        DxvkBufferCreateInfo partialInfo{};
                        partialInfo.size = partialBytes;
                        partialInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                                           | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                        partialInfo.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
                        partialInfo.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                        g_metricsPartialBuffer = device->createBuffer(
                            partialInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

                        DxvkBufferCreateInfo resultInfo{};
                        resultInfo.size = sizeof(SnapFrameMetricsGpuResult);
                        resultInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                                         | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
                        resultInfo.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                                          | VK_PIPELINE_STAGE_TRANSFER_BIT;
                        resultInfo.access = VK_ACCESS_SHADER_WRITE_BIT
                                          | VK_ACCESS_TRANSFER_READ_BIT;
                        g_metricsResultBuffer = device->createBuffer(
                            resultInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

                        bool readbackReady = true;
                        for (uint32_t slot = 0; slot < kMetricsReadbackSlots; ++slot) {
                            DxvkBufferCreateInfo readbackInfo{};
                            readbackInfo.size = sizeof(SnapFrameMetricsGpuResult);
                            readbackInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                            readbackInfo.stages = VK_PIPELINE_STAGE_HOST_BIT;
                            readbackInfo.access = VK_ACCESS_HOST_READ_BIT;
                            g_metricsReadbackBuffers[slot] = device->createBuffer(
                                readbackInfo,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                            if (g_metricsReadbackBuffers[slot] == nullptr
                                || g_metricsReadbackBuffers[slot]->mapPtr(0) == nullptr) {
                                readbackReady = false;
                                break;
                            }
                            g_metricsReadbackBusy[slot].store(0, std::memory_order_release);
                        }
                        g_metricsAvailable = g_metricsPartialShader != nullptr
                            && g_metricsFinalShader != nullptr
                            && g_metricsPartialBuffer != nullptr
                            && g_metricsResultBuffer != nullptr
                            && readbackReady;
                        if (!g_metricsAvailable) {
                            g_metricsPartialBuffer = Rc<DxvkBuffer>();
                            g_metricsResultBuffer = Rc<DxvkBuffer>();
                            for (uint32_t slot = 0; slot < kMetricsReadbackSlots; ++slot)
                                g_metricsReadbackBuffers[slot] = Rc<DxvkBuffer>();
                        }
                        snapframeLog("SnapFrame: V1 GPU scalar metrics %s grid=%ux%u partials=%u readback_slots=%u\n",
                            g_metricsAvailable ? "ready" : "unavailable",
                            flowWidth, flowHeight, g_metricsPartialCount,
                            kMetricsReadbackSlots);
                    } catch (...) {
                        g_metricsAvailable = false;
                        g_metricsPartialBuffer = Rc<DxvkBuffer>();
                        g_metricsResultBuffer = Rc<DxvkBuffer>();
                        for (uint32_t slot = 0; slot < kMetricsReadbackSlots; ++slot)
                            g_metricsReadbackBuffers[slot] = Rc<DxvkBuffer>();
                        snapframeLog("SnapFrame: V1 GPU scalar metrics allocation failed; frame generation remains active\n");
                    }

                    snapframeLog("SnapFrame: midpoint reconstruction resources %s output=RGBA16F extent=%ux%u\n",
                        g_midpointAvailable ? "ready" : "unavailable", width, height);
                    snapframeEvent(g_capturedFrames.load(std::memory_order_relaxed) + 1u,
                        g_midpointAvailable ? "midpoint_resources_ready" : "midpoint_resources_unavailable",
                        width, height, g_midpointAvailable
                            ? "rgba16f_output_and_compute_pipeline_ready"
                            : "format_or_consistency_requirement_not_met");
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
            g_midpointView = Rc<DxvkImageView>();
            g_midpointImage = Rc<DxvkImage>();
            g_midpointSampler = Rc<DxvkSampler>();
            g_midpointShader = Rc<DxvkShader>();
            g_midpointShaderDevice = nullptr;
            g_midpointAvailable = false;
            g_metricsAvailable = false;
            g_metricsPartialBuffer = Rc<DxvkBuffer>();
            g_metricsResultBuffer = Rc<DxvkBuffer>();
            for (uint32_t slot = 0; slot < kMetricsReadbackSlots; ++slot)
                g_metricsReadbackBuffers[slot] = Rc<DxvkBuffer>();
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
            snapframeLog("SnapFrame: bidirectional block-matching flow ready outputs=2 extent=%ux%u format=RGBA16F search=coarse5x5-step4+refine3x3 range=+/-9 patch=3x3 candidates_max=33 flow_scale=%u%% block_stride=%u luma_texels\n",
                flowExtent(width / 2u + width % 2u),
                flowExtent(height / 2u + height % 2u),
                flowScalePercent(), flowBlockStride());
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

void snapframeCopyFrameImpl(void* contextPtr, void* sourceViewPtr, void* devicePtr,
                        uint32_t width, uint32_t height, uint32_t formatValue) {
    if (!g_initialized.load(std::memory_order_acquire))
        return;
    SnapAIPendingMetrics aiMetricsSnapshot{};
    const uint64_t aiTimingFrame = g_lastGpuTimingFrame.load(std::memory_order_acquire);
    {
        std::lock_guard<std::mutex> aiMetricsLock(g_snapAiMetricsMutex);
        aiMetricsSnapshot = g_snapAiPendingMetrics;
        /* Keep the validated metrics pending until their timestamp callback
         * arrives. Drop only if a newer timing sample proves them stale. */
        if (aiMetricsSnapshot.frame != 0
            && aiTimingFrame >= aiMetricsSnapshot.frame)
            g_snapAiPendingMetrics.frame = 0;
    }
    if (aiMetricsSnapshot.frame != 0
        && aiTimingFrame == aiMetricsSnapshot.frame
        && g_lastGpuTimingValid.load(std::memory_order_acquire)) {
        SnapAIFrameMetrics aiMetrics{};
        aiMetrics.struct_size = sizeof(aiMetrics);
        aiMetrics.frame_id = aiMetricsSnapshot.frame;
        aiMetrics.frame_time_ms = g_lastGpuFrameIntervalMs.load(std::memory_order_acquire);
        aiMetrics.gpu_time_ms = g_lastGpuTimingMs.load(std::memory_order_acquire);
        aiMetrics.motion_magnitude = aiMetricsSnapshot.motionMagnitude;
        aiMetrics.motion_complexity = aiMetricsSnapshot.motionComplexity;
        aiMetrics.flow_confidence = aiMetricsSnapshot.flowConfidence;
        aiMetrics.gpu_pressure = aiMetrics.gpu_time_ms / 6.0f;
        aiMetrics.artifact_risk = aiMetricsSnapshot.artifactRisk;
        aiMetrics.scene_cut = 0;
        aiMetrics.telemetry_valid = 1;
        SnapAIDecision decision{};
        decision.struct_size = sizeof(decision);
        SnapAIStatus aiStatus = SNAP_AI_ERROR_INTERNAL;
        {
            std::lock_guard<std::mutex> aiLock(g_snapAiMutex);
            if (g_snapAi)
                aiStatus = snap_ai_update(g_snapAi, &aiMetrics, &decision);
        }
        if (aiStatus == SNAP_AI_OK && decision.mode != SNAP_AI_MODE_BYPASS
            && std::isfinite(decision.flow_scale)) {
            const int requestedScale = std::max(50, std::min(75,
                static_cast<int>(std::lround(decision.flow_scale * 100.0f))));
            const bool scaleDiffers =
                static_cast<uint32_t>(requestedScale) != flowScalePercent();
            const bool scaleChangeCooldownElapsed =
                aiMetrics.frame_id >= g_aiLastScaleChangeFrame
                && (aiMetrics.frame_id - g_aiLastScaleChangeFrame)
                    >= kAiScaleChangeCooldownFrames;
            /* Flow Scale changes rebuild V1 flow/consistency resources and
             * reset temporal history. Treat AI output as a request: never
             * increase compute load without clear GPU and source-frame headroom. */
            const bool reducingLoad = requestedScale < static_cast<int>(flowScalePercent());
            const bool increaseHasHeadroom =
                aiMetrics.gpu_pressure <= 0.55f
                && aiMetrics.frame_time_ms > 0.0f
                && aiMetrics.frame_time_ms <= 20.0f
                && aiMetrics.flow_confidence >= 0.45f
                && aiMetrics.artifact_risk <= 0.25f;
            const bool decreaseIsNeeded =
                aiMetrics.gpu_pressure >= 0.75f
                || aiMetrics.frame_time_ms >= 25.0f;
            const bool scaleChangeAllowed = reducingLoad
                ? decreaseIsNeeded : increaseHasHeadroom;
            if (scaleDiffers && scaleChangeCooldownElapsed && scaleChangeAllowed) {
                if (snapframe_host_apply_backend_config(
                        g_enabled.load(std::memory_order_acquire), requestedScale)) {
                    g_aiLastScaleChangeFrame = aiMetrics.frame_id;
                    snapframeLog("SnapFrame AI: accepted Flow Scale=%d%% at frame=%llu direction=%s pressure=%.3f frame_ms=%.3f confidence=%.3f artifact=%.3f; resource rebuild scheduled\\n",
                        requestedScale,
                        static_cast<unsigned long long>(aiMetrics.frame_id),
                        reducingLoad ? "load_reduction" : "quality_increase",
                        aiMetrics.gpu_pressure, aiMetrics.frame_time_ms,
                        aiMetrics.flow_confidence, aiMetrics.artifact_risk);
                }
            } else if (scaleDiffers && scaleChangeCooldownElapsed) {
                snapframeEvent(aiMetrics.frame_id, "ai_scale_change_deferred",
                    width, height, reducingLoad
                        ? "load_reduction_without_sustained_pressure"
                        : "quality_increase_without_gpu_and_source_headroom");
            }
            const uint64_t decisions = g_aiDecisionCount.fetch_add(
                1, std::memory_order_relaxed) + 1;
            if (decisions == 1 || (decisions % 30u) == 0u)
                snapframeLog("SnapFrame AI: frame=%llu mode=%u flow_scale=%d%% gpu=%.3fms pressure=%.3f confidence=%.3f complexity=%.3f artifact=%.3f\n",
                    static_cast<unsigned long long>(aiMetrics.frame_id),
                    static_cast<unsigned>(decision.mode), requestedScale,
                    aiMetrics.gpu_time_ms, aiMetrics.gpu_pressure,
                    aiMetrics.flow_confidence, aiMetrics.motion_complexity,
                    aiMetrics.artifact_risk);
        }
    } else if (aiMetricsSnapshot.frame != 0
        && aiTimingFrame > aiMetricsSnapshot.frame) {
        snapframeEvent(aiMetricsSnapshot.frame, "ai_bypass", width, height,
            "same_frame_gpu_timestamp_unavailable_or_stale");
    }
    // Consume pending resets before checking enabled. This lets the next
    // present callback release GPU history immediately after the feature is
    // switched off, rather than retaining it until a later re-enable/device
    // destruction.
    if (g_resetHistoryRequested.exchange(0, std::memory_order_acq_rel)) {
        clearHistory();
        snapframeLog("SnapFrame: history reset requested; cached GPU resources released\n");
    }
    g_midpointFrameReady.store(0, std::memory_order_release);
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
        snapframeEvent(g_capturedFrames.load(std::memory_order_relaxed), "skip_invalid_args",
            width, height, "null_callback_argument_or_zero_extent");
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
        snapframeEvent(g_capturedFrames.load(std::memory_order_relaxed), "skip_source_validation",
            width, height, skipReason);
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
            snapframeEvent(g_capturedFrames.load(std::memory_order_relaxed), "skip_history_allocation",
                width, height, "history_or_compute_resource_allocation_failed");
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
        && (nextCapturedFrame % kAiTelemetryIntervalFrames) == 0u;
    /* V2 pyramid/search/refinement/validation are diagnostic-only and do
     * not feed the V1 midpoint warp. Keep them out of normal gameplay; enable
     * them only when explicit profiling or the diagnostic suite is requested.
     * This also avoids inserting sparse timestamp queries into the production
     * path when no V2 diagnostic work is being measured. */
    const bool v2DiagnosticsEnabled = g_profilingEnabled || g_diagnosticSuiteEnabled;
    // The compact V1 metrics reduction and total-chain timestamp share the
    // same frame cadence. Keep this one timestamp pair active in normal
    // gameplay even when verbose profiling and V2 diagnostics are disabled;
    // it is asynchronous and does not enable per-stage profiling/readback.
    const bool telemetrySampleThisFrame =
        (nextCapturedFrame % kAiTelemetryIntervalFrames) == 0u;
    const SnapFrameGpuTimer totalTimer = beginGpuTimer(
        context, device, "total_chain", nextCapturedFrame, width, height,
        profileThisFrame || telemetrySampleThisFrame);
    const uint32_t currentV2SearchInterval =
        g_v2SearchIntervalFrames.load(std::memory_order_acquire);
    if (currentV2SearchInterval != g_v2SearchIntervalLogged) {
        g_v2SearchIntervalLogged = currentV2SearchInterval;
        snapframeLog("SnapFrame AI budget: V2 motion search interval=%u source frames (GPU budget target=6ms)\n",
            currentV2SearchInterval);
    }
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
    snapframeEvent(nextCapturedFrame, "capture_copy_recorded", width, height, "history_image_copy_recorded_on_present_command_list");

    // Shared level extents are needed by both pyramid construction and the
    // subsequent flow-search dispatch. Keep them in the capture-frame scope.
    const uint32_t l0Width = width / 2u + width % 2u;
    const uint32_t l0Height = height / 2u + height % 2u;
    const uint32_t l1Width = l0Width / 2u + l0Width % 2u;
    const uint32_t l1Height = l0Height / 2u + l0Height % 2u;
    const uint32_t l2Width = l1Width / 2u + l1Width % 2u;
    const uint32_t l2Height = l1Height / 2u + l1Height % 2u;
    if (v2DiagnosticsEnabled && g_v2PyramidAvailable) {
        const SnapFrameGpuTimer v2PyramidTimer = beginGpuTimer(
            context, device, "v2_pyramid", nextCapturedFrame, width, height, profileThisFrame);
        g_v2PyramidValid[slot] = false;
        struct alignas(16) V2DimsPush { float dims[4]; };
        static_assert(sizeof(V2DimsPush) == 16, "V2 luma/downsample push ABI");
        V2DimsPush lumaPush{{float(width), float(height), float(l0Width), float(l0Height)}};
        Rc<DxvkImageView> lumaViews[5];
        Rc<DxvkSampler> lumaSamplers[5];
        lumaViews[0] = g_historyViews[slot];
        lumaViews[4] = g_v2PyramidView[slot][0];
        lumaSamplers[0] = g_lumaSampler;
        const bool lumaOk = context->dispatchSnapFrameV2(
            g_v2LumaShader, lumaViews, lumaSamplers, 5u,
            (l0Width + 7u) / 8u, (l0Height + 7u) / 8u,
            sizeof(lumaPush), &lumaPush);
        if (lumaOk) {
            ++g_v2LumaDispatchCount;
            snapframeEvent(nextCapturedFrame, "v2_luma_dispatch_ok",
                width, height, "source_to_L0_R16F");
            context->flushComputeBarriers();
            V2DimsPush l1Push{{float(l0Width), float(l0Height), float(l1Width), float(l1Height)}};
            Rc<DxvkImageView> downsampleViews[5];
            Rc<DxvkSampler> downsampleSamplers[5];
            downsampleViews[0] = g_v2PyramidView[slot][0];
            downsampleViews[4] = g_v2PyramidView[slot][1];
            downsampleSamplers[0] = g_lumaSampler;
            const bool l1Ok = context->dispatchSnapFrameV2(
                g_v2DownsampleShader, downsampleViews, downsampleSamplers, 5u,
                (l1Width + 7u) / 8u, (l1Height + 7u) / 8u,
                sizeof(l1Push), &l1Push);
            if (l1Ok) {
                context->flushComputeBarriers();
                V2DimsPush l2Push{{float(l1Width), float(l1Height), float(l2Width), float(l2Height)}};
                downsampleViews[0] = g_v2PyramidView[slot][1];
                downsampleViews[4] = g_v2PyramidView[slot][2];
                const bool l2Ok = context->dispatchSnapFrameV2(
                    g_v2DownsampleShader, downsampleViews, downsampleSamplers, 5u,
                    (l2Width + 7u) / 8u, (l2Height + 7u) / 8u,
                    sizeof(l2Push), &l2Push);
                if (l2Ok) {
                    g_v2PyramidValid[slot] = true;
                    ++g_v2DownsampleDispatchCount;
                    snapframeEvent(nextCapturedFrame, "v2_downsample_dispatch_ok",
                        width, height, "L0_to_L1_to_L2");
                    context->flushComputeBarriers();
                }
                if (!l2Ok || !l1Ok) {
                    snapframeEvent(nextCapturedFrame, "v2_downsample_failed",
                        width, height, l2Ok ? "l1_or_l2_dispatch_rejected" : "l2_dispatch_rejected");
                } else if (g_v2DownsampleDispatchCount == 1
                    || (g_v2DownsampleDispatchCount % 300u) == 0u) {
                    snapframeLog("SnapFrame: V2 pyramid dispatch accepted count=%llu frame=%llu L0=%ux%u L1=%ux%u L2=%ux%u\n",
                        static_cast<unsigned long long>(g_v2DownsampleDispatchCount),
                        static_cast<unsigned long long>(nextCapturedFrame),
                        l0Width, l0Height, l1Width, l1Height, l2Width, l2Height);
                }
            } else {
                snapframeEvent(nextCapturedFrame, "v2_downsample_failed",
                    width, height, "l1_dispatch_rejected");
            }
        } else {
            snapframeEvent(nextCapturedFrame, "v2_luma_failed",
                width, height, "v2_luma_dispatch_rejected");
        }
        endGpuTimer(context, v2PyramidTimer);
    }

    const bool v2SearchScheduled = currentV2SearchInterval <= 1u
        || (nextCapturedFrame % currentV2SearchInterval) == 0u;
    if (v2DiagnosticsEnabled && g_v2FlowAvailable && g_v2PyramidValid[slot]
        && g_v2PyramidValid[slot ^ 1u] && v2SearchScheduled) {
        const SnapFrameGpuTimer v2SearchTimer = beginGpuTimer(
            context, device, "v2_search", nextCapturedFrame, width, height, profileThisFrame);
        const unsigned previousSlot = slot ^ 1u;
        struct alignas(16) V2SearchPush {
            float dims[4]; float geom[4]; float flags[4];
        };
        static_assert(sizeof(V2SearchPush) == 48, "V2 search push ABI");
        bool directionsOk = true;
        for (unsigned direction = 0; direction < 2 && directionsOk; ++direction) {
            const unsigned currentSlot = direction == 0 ? slot : previousSlot;
            const unsigned referenceSlot = direction == 0 ? previousSlot : slot;
            for (int level = 2; level >= 0; --level) {
                const uint32_t levelWidth = level == 0 ? l0Width : (level == 1 ? l1Width : l2Width);
                const uint32_t levelHeight = level == 0 ? l0Height : (level == 1 ? l1Height : l2Height);
                const uint32_t blocksX = v2FlowGridExtent(levelWidth);
                const uint32_t blocksY = v2FlowGridExtent(levelHeight);
                V2SearchPush push{};
                push.dims[0] = float(levelWidth); push.dims[1] = float(levelHeight);
                push.dims[2] = float(blocksX); push.dims[3] = float(blocksY);
                push.geom[0] = level == 2 ? 4.0f : (level == 1 ? 2.0f : 1.0f);
                push.geom[1] = level == 2 ? 16.0f : (level == 1 ? 8.0f : 4.0f);
                push.geom[2] = 1.0f; push.geom[3] = level == 2 ? 0.0f : 1.0f;
                push.flags[0] = level == 2 ? 0.0f : 1.0f;
                Rc<DxvkImageView> views[5];
                Rc<DxvkSampler> samplers[5];
                views[0] = g_v2PyramidView[currentSlot][level];
                views[1] = g_v2PyramidView[referenceSlot][level];
                views[2] = level == 2 ? g_v2FlowView[direction ^ 1u][2]
                                      : g_v2FlowView[direction][level + 1];
                views[4] = g_v2FlowView[direction][level];
                samplers[0] = g_lumaSampler; samplers[1] = g_lumaSampler;
                samplers[2] = g_lumaSampler;
                const bool dispatched = context->dispatchSnapFrameV2(
                    g_v2SearchShader, views, samplers, 5u,
                    (blocksX + 7u) / 8u, (blocksY + 7u) / 8u,
                    sizeof(push), &push);
                if (!dispatched) {
                    directionsOk = false;
                    snapframeEvent(nextCapturedFrame, "v2_flow_search_failed", width, height,
                        level == 2 ? "L2_search_dispatch_rejected"
                            : (level == 1 ? "L1_search_dispatch_rejected" : "L0_search_dispatch_rejected"));
                    break;
                }
                context->flushComputeBarriers();
            }
        }
        if (directionsOk) {
            ++g_v2FlowDispatchCount;
            snapframeEvent(nextCapturedFrame, "v2_flow_pair_ok", width, height,
                "bidirectional_L2_to_L1_to_L0_search");
            if (g_v2FlowDispatchCount == 1u || (g_v2FlowDispatchCount % 120u) == 0u)
                snapframeLog("SnapFrame: V2 coarse-to-fine flow pair accepted count=%llu frame=%llu levels=L2,L1,L0 directions=2\n",
                    static_cast<unsigned long long>(g_v2FlowDispatchCount),
                    static_cast<unsigned long long>(nextCapturedFrame));
        }

        endGpuTimer(context, v2SearchTimer);

        if (directionsOk && g_v2RefinementAvailable) {
            struct alignas(16) V2RefinePush {
                float dims[4]; float tune[4];
            };
            static_assert(sizeof(V2RefinePush) == 32, "V2 refine push ABI");
            const uint32_t blocksX = v2FlowGridExtent(l0Width);
            const uint32_t blocksY = v2FlowGridExtent(l0Height);
            const SnapFrameGpuTimer v2RefineTimer = beginGpuTimer(
                context, device, "v2_refine", nextCapturedFrame,
                width, height, profileThisFrame);
            bool refinementOk = true;
            for (unsigned direction = 0; direction < 2; ++direction) {
                const unsigned currentSlot = direction == 0 ? slot : previousSlot;
                const unsigned referenceSlot = direction == 0 ? previousSlot : slot;
                V2RefinePush push{};
                push.dims[0] = float(l0Width);
                push.dims[1] = float(l0Height);
                push.dims[2] = float(blocksX);
                push.dims[3] = float(blocksY);
                push.tune[0] = 4.0f;
                push.tune[1] = 2.0f;
                Rc<DxvkImageView> views[5];
                Rc<DxvkSampler> samplers[5];
                views[0] = g_v2FlowView[direction][0];
                views[1] = g_v2PyramidView[currentSlot][0];
                views[2] = g_v2PyramidView[referenceSlot][0];
                views[4] = g_v2RefinedFlowView[direction];
                samplers[0] = g_lumaSampler;
                samplers[1] = g_lumaSampler;
                samplers[2] = g_lumaSampler;
                const bool dispatched = context->dispatchSnapFrameV2(
                    g_v2RefineShader, views, samplers, 5u,
                    (blocksX + 7u) / 8u, (blocksY + 7u) / 8u,
                    sizeof(push), &push);
                if (!dispatched) {
                    refinementOk = false;
                    snapframeEvent(nextCapturedFrame, "v2_refinement_failed",
                        width, height, direction == 0 ? "backward_refine_rejected" : "forward_refine_rejected");
                    break;
                }
                context->flushComputeBarriers();
            }
            endGpuTimer(context, v2RefineTimer);
            if (refinementOk) {
                struct alignas(16) V2ValidationPush {
                    float dims[4]; float tune[4];
                };
                static_assert(sizeof(V2ValidationPush) == 32, "V2 validation push ABI");
                V2ValidationPush validationPush{};
                validationPush.dims[0] = float(l0Width);
                validationPush.dims[1] = float(l0Height);
                validationPush.dims[2] = float(blocksX);
                validationPush.dims[3] = float(blocksY);
                validationPush.tune[0] = 1.5f;
                validationPush.tune[1] = 4.0f;
                Rc<DxvkImageView> validationViews[5];
                Rc<DxvkSampler> validationSamplers[5];
                validationViews[0] = g_v2RefinedFlowView[0]; // current -> previous
                validationViews[1] = g_v2RefinedFlowView[1]; // previous -> current
                validationViews[4] = g_v2ValidationView;
                validationSamplers[0] = g_lumaSampler;
                validationSamplers[1] = g_lumaSampler;
                const SnapFrameGpuTimer v2ValidationTimer = beginGpuTimer(
                    context, device, "v2_validation", nextCapturedFrame,
                    width, height, profileThisFrame);
                const bool validationOk = context->dispatchSnapFrameV2(
                    g_v2ValidationShader, validationViews, validationSamplers, 5u,
                    (blocksX + 7u) / 8u, (blocksY + 7u) / 8u,
                    sizeof(validationPush), &validationPush);
                endGpuTimer(context, v2ValidationTimer);
                if (validationOk) {
                    context->flushComputeBarriers();
                    ++g_v2RefinementDispatchCount;
                    snapframeEvent(nextCapturedFrame, "v2_occlusion_mask_ok",
                        width, height, "refined_bidirectional_round_trip_confidence");
                    if (g_v2RefinementDispatchCount == 1u
                        || (g_v2RefinementDispatchCount % 120u) == 0u)
                        snapframeLog("SnapFrame: V2 refinement+validation accepted count=%llu frame=%llu grid=%ux%u thresholds=1.5/4.0px\n",
                            static_cast<unsigned long long>(g_v2RefinementDispatchCount),
                            static_cast<unsigned long long>(nextCapturedFrame), blocksX, blocksY);
                } else {
                    snapframeEvent(nextCapturedFrame, "v2_occlusion_mask_failed",
                        width, height, "forward_backward_validation_dispatch_rejected");
                }
            }
        }
    }

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
        snapframeEvent(nextCapturedFrame, dispatched ? "luma_dispatch_ok" : "luma_dispatch_failed",
            width, height, dispatched ? "dispatch_accepted" : "dispatch_rejected_or_resources_unavailable");
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
            const uint32_t flowWidth = flowExtent(lumaWidth);
            const uint32_t flowHeight = flowExtent(lumaHeight);
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
            snapframeEvent(nextCapturedFrame,
                currentToPrevious && previousToCurrent ? "flow_pair_ok" : "flow_pair_failed",
                width, height, currentToPrevious && previousToCurrent
                    ? "both_directions_recorded" : "one_or_both_directions_rejected");
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
                            g_lumaView[slot], g_lumaSampler, g_consistencyView,
                            (flowWidth + 7u) / 8u, (flowHeight + 7u) / 8u);
                    if (consistencyDispatched && profileThisFrame) context->flushComputeBarriers();
                    endGpuTimer(context, consistencyTimer);
                    snapframeEvent(nextCapturedFrame,
                        consistencyDispatched ? "consistency_dispatch_ok" : "consistency_dispatch_failed",
                        width, height, consistencyDispatched ? "dispatch_accepted" : "dispatch_rejected");
                    if (consistencyDispatched) {
                        ++g_consistencyDispatchCount;
                        if (g_metricsAvailable && (nextCapturedFrame % kAiTelemetryIntervalFrames) == 0u) {
                            recordV1GpuMetrics(context, nextCapturedFrame,
                                width, height, flowWidth, flowHeight);
                        }
                        if (g_midpointAvailable) {
                            const SnapFrameGpuTimer midpointTimer = beginGpuTimer(
                                context, device, "midpoint_warp", nextCapturedFrame,
                                width, height, profileThisFrame);
                            const bool midpointDispatched = context->dispatchSnapFrameWarp(
                                g_midpointShader, g_historyViews[previousSlot],
                                g_historyViews[slot], g_flowView[0], g_flowView[1],
                                g_consistencyView, g_midpointSampler, g_midpointView,
                                (width + 7u) / 8u, (height + 7u) / 8u);
                            endGpuTimer(context, midpointTimer);
                            snapframeEvent(nextCapturedFrame,
                                midpointDispatched ? "midpoint_warp_ok" : "midpoint_warp_failed",
                                width, height, midpointDispatched
                                    ? "rgba16f_midpoint_image_written" : "dispatch_rejected");
                            if (midpointDispatched) {
                                ++g_midpointDispatchCount;
                                g_midpointFrameReady.store(1, std::memory_order_release);
                                // The following present/composite pass samples this storage output.
                                context->flushComputeBarriers();
                                if (g_midpointDispatchCount == 1
                                 || (g_midpointDispatchCount % 300u) == 0u)
                                    snapframeLog("SnapFrame: midpoint warp accepted count=%llu extent=%ux%u output=RGBA16F\n",
                                        static_cast<unsigned long long>(g_midpointDispatchCount),
                                        width, height);
                            } else {
                                ++g_midpointFailureCount;
                            }
                        }
                        if (g_profilingEnabled && (g_flowDispatchCount % 120u) == 0u) {
                            endGpuTimer(context, totalTimer);
                            totalTimerEnded = true;
                            recordFlowReadback(context, device, nextCapturedFrame, flowWidth, flowHeight,
                                g_diagnosticSuiteEnabled && (g_flowDispatchCount % 600u) == 0u);
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
    snapframeEvent(captured, "frame_complete", width, height,
        g_lumaAvailable ? "capture_complete_luma_path_available" : "capture_complete_capture_only");
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

void snapframeCopyFrame(void* contextPtr, void* sourceViewPtr, void* devicePtr,
                        uint32_t width, uint32_t height, uint32_t formatValue) {
    if (!g_initialized.load(std::memory_order_acquire))
        return;

    const uint64_t nowNs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    const uint64_t previousNs = g_lastFrameTimestampNs.exchange(nowNs, std::memory_order_acq_rel);
    if (previousNs && nowNs > previousNs)
        g_lastFrameIntervalMs.store(static_cast<float>(nowNs - previousNs) / 1000000.0f,
            std::memory_order_release);

    /* UI changes only set this flag. Resource release happens on this serialized
     * DXVK callback, before either the portable core or legacy fallback runs. */
    if (g_resetHistoryRequested.exchange(0, std::memory_order_acq_rel)) {
        clearHistory();
        snapframeLog("SnapFrame: queued history reset consumed on render callback\n");
    }
    g_midpointFrameReady.store(0, std::memory_order_release);

    const uint64_t skippedBefore = g_skippedFrames.load(std::memory_order_relaxed);
    const int adapterResult = snapframe_engine_adapter_process_frame(
        contextPtr, sourceViewPtr, devicePtr, width, height, formatValue);
    if (adapterResult == 0) {
        /* Portable engine creation failure: preserve the previous real path. */
        snapframeCopyFrameImpl(contextPtr, sourceViewPtr, devicePtr,
            width, height, formatValue);
    } else if (adapterResult < 0) {
        if (g_skippedFrames.load(std::memory_order_relaxed) == skippedBefore)
            g_skippedFrames.fetch_add(1, std::memory_order_relaxed);
        snapframeLog("SnapFrame: portable backend failed; presenting real frame only\n");
    }
}

void snapframeDeviceDestroyed(void* devicePtr) {
    auto* device = reinterpret_cast<DxvkDevice*>(devicePtr);
    if (g_device == device || g_lumaShaderDevice == device || g_v2ShaderDevice == device) {
        snapframeLog("SnapFrame: DXVK device destroyed; releasing history and shader state\n");
        clearHistory();
        g_lumaShader = Rc<DxvkShader>();
        g_lumaShaderDevice = nullptr;
        g_flowShader = Rc<DxvkShader>();
        g_flowShaderDevice = nullptr;
        g_consistencyShader = Rc<DxvkShader>();
        g_consistencyShaderDevice = nullptr;
        g_midpointShader = Rc<DxvkShader>();
        g_midpointShaderDevice = nullptr;
        g_v2LumaShader = Rc<DxvkShader>();
        g_v2DownsampleShader = Rc<DxvkShader>();
        g_v2SearchShader = Rc<DxvkShader>();
        g_v2ShaderDevice = nullptr;
    }
}

void snapframeForwardDxvkLog(const char* message) {
    if (message && *message)
        snapframeLog("%s\n", message);
}
}

extern "C" int snapframe_host_apply_backend_config(int enabled, int flow_scale_percent) {
    if (flow_scale_percent < 25 || flow_scale_percent > 100)
        return 0;
    const uint32_t scale = static_cast<uint32_t>(flow_scale_percent);
    const uint32_t previousScale = g_flowScalePercent.exchange(scale, std::memory_order_acq_rel);
    const int nextEnabled = enabled ? 1 : 0;
    const int previousEnabled = g_enabled.exchange(nextEnabled, std::memory_order_acq_rel);
    if (previousScale != scale || previousEnabled != nextEnabled)
        g_resetHistoryRequested.store(1, std::memory_order_release);
    return 1;
}

extern "C" void snapframe_host_backend_reset_history(void) {
    g_resetHistoryRequested.store(1, std::memory_order_release);
}

extern "C" int snapframe_host_process_legacy_frame(
    void* context, void* source_view, void* device,
    uint32_t width, uint32_t height, uint32_t format) {
    const uint64_t skippedBefore = g_skippedFrames.load(std::memory_order_relaxed);
    snapframeCopyFrameImpl(context, source_view, device, width, height, format);
    if (g_skippedFrames.load(std::memory_order_relaxed) == skippedBefore)
        return 1;
    g_midpointFrameReady.store(0, std::memory_order_release);
    g_resetHistoryRequested.store(1, std::memory_order_release);
    return 0;
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

extern "C" int snapframe_host_set_event_log_path(const char *path) {
    if (!path || !*path) return 0;
    std::lock_guard<std::mutex> lock(g_eventLogMutex);
    if (g_eventLogFile) fclose(g_eventLogFile);
    g_eventLogFile = fopen(path, "w");
    if (!g_eventLogFile) {
        fprintf(stderr, "SnapFrame: cannot open event CSV: %s\n", path);
        return 0;
    }
    setvbuf(g_eventLogFile, nullptr, _IOFBF, 64u * 1024u);
    g_eventLogWriteCount = 0;
    fprintf(g_eventLogFile, "frame,event,width,height,detail\n");
    fflush(g_eventLogFile);
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
extern "C" void snapframe_dxvk_set_present_mode_hook(int (*hook)(int*));
extern "C" void snapframe_dxvk_set_generation_hooks(
    bool (*canGenerateHook)(void),
    void* (*generateHook)(void*, void*, uint32_t, uint32_t, uint32_t),
    void (*presentResultHook)(int, int));
extern "C" bool snapframe_host_can_generate(void);
extern "C" void* snapframe_host_generate_view(
    void* contextPtr, void* devicePtr,
    uint32_t width, uint32_t height, uint32_t formatValue);
extern "C" void snapframe_host_present_result(int success, int vkResult);

extern "C" void snapframe_config_defaults(SnapFrameConfig *config) {
    if (!config) return;
    memset(config, 0, sizeof(*config));
    config->enabled = 0;
    config->multiplier = 2;
    config->quality = SNAPFRAME_QUALITY_BALANCED;
    config->flow_scale_percent = 50;
    config->present_mode = SNAPFRAME_PRESENT_MODE_AUTO;
    config->motion_analysis = 1;
    config->profiling_enabled = 0;
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
    if (validated.present_mode < SNAPFRAME_PRESENT_MODE_AUTO
        || validated.present_mode > SNAPFRAME_PRESENT_MODE_IMMEDIATE)
        validated.present_mode = SNAPFRAME_PRESENT_MODE_AUTO;
    g_presentMode.store(validated.present_mode, std::memory_order_release);
    if (validated.diagnostic_phase_frames < 300) validated.diagnostic_phase_frames = 300;
    if (validated.diagnostic_phase_frames > 36000) validated.diagnostic_phase_frames = 36000;
    g_flowScalePercent.store(static_cast<uint32_t>(validated.flow_scale_percent), std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(g_motionV2Mutex);
        if (!g_motionV2) {
            SnapMotionV2Config motionConfig{};
            snap_motion_v2_default_config(&motionConfig);
            motionConfig.enabled = 0;
            motionConfig.static_mask_enabled = 0;
            motionConfig.validation_enabled = 0;
            motionConfig.adaptive_quality_enabled = 0;
            g_motionV2 = snap_motion_v2_create(&motionConfig);
        }
    }
    {
        std::lock_guard<std::mutex> aiLock(g_snapAiMutex);
        if (g_snapAi) {
            snap_ai_destroy(g_snapAi);
            g_snapAi = nullptr;
        }
        SnapAIConfig aiConfig{};
        snap_ai_default_config(&aiConfig);
        aiConfig.enabled = 1;
        aiConfig.profile = SNAP_AI_PROFILE_BALANCED;
        aiConfig.target_gpu_budget_ms = 6.0f;
        aiConfig.min_flow_scale = 0.50f;
        aiConfig.max_flow_scale = 0.75f;
        aiConfig.min_refinement_passes = 1;
        aiConfig.max_refinement_passes = 2;
        aiConfig.hysteresis = 0.08f;
        aiConfig.warmup_frames = 8;
        g_snapAi = snap_ai_create(&aiConfig);
    }
    {
        std::lock_guard<std::mutex> aiMetricsLock(g_snapAiMetricsMutex);
        g_snapAiPendingMetrics = SnapAIPendingMetrics{};
    }
    g_aiDecisionCount.store(0, std::memory_order_relaxed);
    g_aiLastScaleChangeFrame = 0;
    g_motionAnalysisEnabled = validated.motion_analysis != 0;
    g_diagnosticSuiteEnabled = validated.diagnostic_suite != 0 && g_motionAnalysisEnabled;
    if (validated.diagnostic_suite != 0 && !g_motionAnalysisEnabled)
        snapframeLog("SnapFrame: diagnostic_suite requested but skipped because motion_analysis=0\n");
    g_diagnosticSuiteComplete = false;
    g_diagnosticPhaseFrames = static_cast<uint32_t>(validated.diagnostic_phase_frames);
    g_diagnosticPhase = UINT32_MAX;
    g_profilingEnabled = validated.profiling_enabled != 0;
    g_flowReadbackFailureLogged = false;
    snapframeLog("SnapFrame: MotionV2 telemetry store %s\n",
        g_motionV2 ? "ready" : "unavailable");
    snapframeLog("SnapFrame: flow readback %s (motion_analysis=%d profiling=%d diagnostic_suite=%d interval=120 frames)\n",
        (g_motionAnalysisEnabled && g_profilingEnabled) ? "enabled" : "disabled",
        g_motionAnalysisEnabled ? 1 : 0, g_profilingEnabled ? 1 : 0,
        g_diagnosticSuiteEnabled ? 1 : 0);

    g_enabled.store(validated.enabled ? 1 : 0, std::memory_order_release);
    const int portableReady = snapframe_engine_adapter_init(&validated);
    g_capturedFrames.store(0, std::memory_order_relaxed);
    g_generatedFrames.store(0, std::memory_order_relaxed);
    g_midpointFrameReady.store(0, std::memory_order_relaxed);
    g_skippedFrames.store(0, std::memory_order_relaxed);
    g_copyCallbackCalls.store(0, std::memory_order_relaxed);
    g_lastGpuTimingFrame.store(0, std::memory_order_relaxed);
    g_lastGpuTimingMs.store(0.0f, std::memory_order_relaxed);
    g_lastGpuFrameIntervalMs.store(0.0f, std::memory_order_relaxed);
    g_lastGpuTimingValid.store(0, std::memory_order_relaxed);
    g_lastFrameIntervalMs.store(0.0f, std::memory_order_relaxed);
    g_lastFrameTimestampNs.store(0, std::memory_order_relaxed);
    g_resetHistoryRequested.store(1, std::memory_order_release);
    g_initialized.store(1, std::memory_order_release);

    snapframe_dxvk_set_copy_hook(snapframeCopyFrame);
    snapframe_dxvk_set_generation_hooks(
        snapframe_host_can_generate, snapframe_host_generate_view,
        snapframe_host_present_result);
    snapframe_dxvk_set_device_destroy_hook(snapframeDeviceDestroyed);
    snapframe_dxvk_set_log_hook(snapframeForwardDxvkLog);
    snapframe_dxvk_set_present_mode_hook(snapframe_host_get_present_mode);
    snapframeEvent(0, "host_init", 0, 0,
        g_motionAnalysisEnabled ? "motion_analysis_enabled" : "motion_analysis_disabled");
    snapframeLog("SnapFrame: diagnostic_suite=%d phase_frames=%u phases=7 profiling=%d motion_analysis=%d timestamp_interval=30 flow_readback_interval=120\n",
        g_diagnosticSuiteEnabled ? 1 : 0, g_diagnosticPhaseFrames,
        g_profilingEnabled ? 1 : 0, g_motionAnalysisEnabled ? 1 : 0);
    snapframeLog( "SnapFrame: host initialized enabled=%d quality=%d flow_scale=%d%% motion_analysis=%d mode=%s\n",
            validated.enabled, validated.quality, validated.flow_scale_percent,
            g_motionAnalysisEnabled ? 1 : 0,
            g_motionAnalysisEnabled ? "capture+luma+bidirectional-flow+fb-consistency+midpoint-warp" : "capture-only");
    snapframeLog("SnapFrame: portable SnapFrameBackendV1 %s\n",
        portableReady ? "active (DXVK GPU pipeline adapter)" : "unavailable; legacy fallback active");
    return 1;
}

extern "C" void snapframe_host_shutdown(void) {
    g_enabled.store(0, std::memory_order_release);
    g_initialized.store(0, std::memory_order_release);
    g_resetHistoryRequested.store(1, std::memory_order_release);
    snapframe_dxvk_set_copy_hook(nullptr);
    snapframe_dxvk_set_generation_hooks(nullptr, nullptr, nullptr);
    snapframe_dxvk_set_present_mode_hook(nullptr);
    snapframe_engine_adapter_shutdown();
    {
        std::lock_guard<std::mutex> aiLock(g_snapAiMutex);
        if (g_snapAi) {
            snap_ai_destroy(g_snapAi);
            g_snapAi = nullptr;
        }
    }
    {
        std::lock_guard<std::mutex> aiMetricsLock(g_snapAiMetricsMutex);
        g_snapAiPendingMetrics = SnapAIPendingMetrics{};
    }
    {
        std::lock_guard<std::mutex> lock(g_motionV2Mutex);
        if (g_motionV2) {
            snap_motion_v2_destroy(g_motionV2);
            g_motionV2 = nullptr;
        }
    }
    g_midpointFrameReady.store(0, std::memory_order_release);
    /* Keep the device-destroy hook registered so owned images are released
     * before DXVK tears down the device allocator. */
}

extern "C" int snapframe_host_active(void) {
    return g_initialized.load(std::memory_order_acquire)
        && g_enabled.load(std::memory_order_acquire);
}

extern "C" int snapframe_host_set_enabled(int enabled) {
    return snapframe_host_update_settings(enabled ? 1 : 0,
        static_cast<int>(flowScalePercent()));
}


extern "C" int snapframe_host_get_settings(int *enabled, int *flow_scale_percent) {
    if (!g_initialized.load(std::memory_order_acquire)) return 0;
    if (enabled) *enabled = g_enabled.load(std::memory_order_acquire);
    if (flow_scale_percent) *flow_scale_percent = static_cast<int>(flowScalePercent());
    return 1;
}

extern "C" int snapframe_host_get_present_mode(int *present_mode) {
    if (present_mode)
        *present_mode = g_presentMode.load(std::memory_order_acquire);
    return g_initialized.load(std::memory_order_acquire) ? 1 : 0;
}

extern "C" int snapframe_host_update_present_mode(int present_mode) {
    if (present_mode < SNAPFRAME_PRESENT_MODE_AUTO
        || present_mode > SNAPFRAME_PRESENT_MODE_IMMEDIATE
        || !g_initialized.load(std::memory_order_acquire))
        return 0;
    const int previous = g_presentMode.exchange(present_mode, std::memory_order_acq_rel);
    if (previous != present_mode)
        snapframeLog("SnapFrame: live present mode change queued; DXVK will recreate the shared swapchain at the next present boundary\n");
    return 1;
}

extern "C" int snapframe_host_get_last_telemetry(
    uint64_t *frame, float *gpu_time_ms, float *frame_time_ms) {
    const uint64_t completedFrame = g_lastGpuTimingFrame.load(std::memory_order_acquire);
    if (frame) *frame = completedFrame;
    if (gpu_time_ms) *gpu_time_ms = g_lastGpuTimingMs.load(std::memory_order_acquire);
    if (frame_time_ms) *frame_time_ms = g_lastGpuFrameIntervalMs.load(std::memory_order_acquire);
    return completedFrame != 0 && g_lastGpuTimingValid.load(std::memory_order_acquire) ? 1 : 0;
}

extern "C" int snapframe_host_update_settings(int enabled, int flow_scale_percent) {
    if (!g_initialized.load(std::memory_order_acquire)
        || flow_scale_percent < 25 || flow_scale_percent > 100)
        return 0;

    if (snapframe_engine_adapter_available()) {
        if (!snapframe_engine_adapter_update_settings(enabled ? 1 : 0, flow_scale_percent))
            return 0;
        snapframeLog("SnapFrame: portable config update queued enabled=%d flow_scale=%d%%\n",
            enabled ? 1 : 0, flow_scale_percent);
        return 1;
    }

    /* Adapter creation fallback: retain the legacy host's live controls. */
    const uint32_t requestedScale = static_cast<uint32_t>(flow_scale_percent);
    const uint32_t previousScale = g_flowScalePercent.exchange(
        requestedScale, std::memory_order_acq_rel);
    const int requestedEnabled = enabled ? 1 : 0;
    const int previousEnabled = g_enabled.exchange(
        requestedEnabled, std::memory_order_acq_rel);
    if (previousScale != requestedScale || previousEnabled != requestedEnabled) {
        g_resetHistoryRequested.store(1, std::memory_order_release);
        snapframeLog("SnapFrame: legacy live settings queued enabled=%d flow_scale=%u%%\n",
            requestedEnabled, requestedScale);
    }
    return 1;
}

extern "C" bool snapframe_host_can_generate(void) {
    return snapframe_host_active()
        && g_midpointAvailable
        && g_midpointView != nullptr
        && g_midpointFrameReady.load(std::memory_order_acquire) != 0;
}

extern "C" void* snapframe_host_generate_view(
    void* contextPtr, void* devicePtr,
    uint32_t width, uint32_t height, uint32_t formatValue) {
    if (!contextPtr || !devicePtr
        || !snapframe_host_can_generate()
        || reinterpret_cast<DxvkDevice*>(devicePtr) != g_device
        || width != g_width || height != g_height
        || static_cast<VkFormat>(formatValue) != g_format) {
        g_midpointFrameReady.store(0, std::memory_order_release);
        snapframeEvent(g_capturedFrames.load(std::memory_order_relaxed),
            "midpoint_present_rejected", width, height,
            "missing_ready_image_or_device_extent_format_mismatch");
        return nullptr;
    }

    auto* generatedView = new (std::nothrow) Rc<DxvkImageView>(g_midpointView);
    if (!generatedView) {
        g_midpointFrameReady.store(0, std::memory_order_release);
        snapframeEvent(g_capturedFrames.load(std::memory_order_relaxed),
            "midpoint_present_rejected", width, height, "view_reference_allocation_failed");
        return nullptr;
    }

    g_midpointFrameReady.store(0, std::memory_order_release);
    snapframeEvent(g_capturedFrames.load(std::memory_order_relaxed),
        "midpoint_present_requested", width, height, "additional_wsi_present_uses_rgba16f_midpoint");
    return generatedView;
}

extern "C" void snapframe_host_present_result(int success, int vkResult) {
    if (success) {
        g_generatedFrames.fetch_add(1, std::memory_order_relaxed);
        snapframeEvent(g_capturedFrames.load(std::memory_order_relaxed),
            "midpoint_present_accepted", g_width, g_height,
            (std::string("wsi_present_completion_vk_success result=") + std::to_string(vkResult)).c_str());
    } else if (vkResult == VK_NOT_READY || vkResult == VK_TIMEOUT) {
        /* Opportunistic generated acquire is intentionally non-blocking.
         * No free WSI image is normal queue backpressure, not a render failure. */
        snapframeEvent(g_capturedFrames.load(std::memory_order_relaxed),
            "midpoint_present_skipped_backpressure", g_width, g_height,
            (std::string("generated_wsi_image_unavailable result=") + std::to_string(vkResult)).c_str());
    } else {
        snapframeEvent(g_capturedFrames.load(std::memory_order_relaxed),
            "midpoint_present_failed", g_width, g_height,
            (std::string("generation_or_wsi_present_not_accepted result=") + std::to_string(vkResult)).c_str());
    }
}

extern "C" void snapframe_host_get_stats(SnapFrameStats *stats) {
    if (!stats) return;
    memset(stats, 0, sizeof(*stats));
    stats->captured_frames = g_capturedFrames.load(std::memory_order_relaxed);
    stats->generated_frames = g_generatedFrames.load(std::memory_order_relaxed);
    stats->skipped_frames = g_skippedFrames.load(std::memory_order_relaxed);
}
