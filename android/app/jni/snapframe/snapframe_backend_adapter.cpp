#include "snapframe_host.h"
#include "snap_frame/snap_frame.h"

#include <atomic>
#include <cstdint>
#include <mutex>

extern "C" int snapframe_host_apply_backend_config(int enabled, int flow_scale_percent);
extern "C" void snapframe_host_backend_reset_history(void);
extern "C" int snapframe_host_process_legacy_frame(
    void* context, void* source_view, void* device,
    uint32_t width, uint32_t height, uint32_t format);

namespace {

struct NativeFrame {
    void* context;
    void* source_view;
    void* device;
    uint32_t width;
    uint32_t height;
    uint32_t format;
};

std::mutex g_adapter_mutex;
SnapFrameEngine* g_engine = nullptr;
SnapFrameEngineConfig g_config{};
std::atomic<uint64_t> g_frame_id{0};

SnapFrameStatus backendInitialize(void*, const SnapFrameEngineConfig* config) {
    if (!config) return SNAP_FRAME_ERROR_INVALID_ARGUMENT;
    return snapframe_host_apply_backend_config(
        static_cast<int>(config->enabled),
        static_cast<int>(config->flow_scale_percent))
        ? SNAP_FRAME_OK : SNAP_FRAME_ERROR_BACKEND;
}

void backendShutdown(void*) {
}

SnapFrameStatus backendUpdateConfig(void*, const SnapFrameEngineConfig* config) {
    if (!config || config->flow_scale_percent < 25
        || config->flow_scale_percent > 100
        || config->multiplier < 1 || config->multiplier > 2)
        return SNAP_FRAME_ERROR_INVALID_ARGUMENT;

    /* These settings are not yet hot-wired to the legacy GPU host. Reject
     * changes instead of pretending that they affected the shader pipeline. */
    if (config->multiplier != g_config.multiplier
        || config->quality != g_config.quality
        || config->motion_analysis != g_config.motion_analysis
        || config->profiling_enabled != g_config.profiling_enabled
        || config->diagnostic_suite != g_config.diagnostic_suite
        || config->diagnostic_phase_frames != g_config.diagnostic_phase_frames)
        return SNAP_FRAME_ERROR_INVALID_ARGUMENT;

    return snapframe_host_apply_backend_config(
        static_cast<int>(config->enabled),
        static_cast<int>(config->flow_scale_percent))
        ? SNAP_FRAME_OK : SNAP_FRAME_ERROR_BACKEND;
}

void backendResetHistory(void*, uint32_t) {
    snapframe_host_backend_reset_history();
}

SnapFrameStatus backendProcessFrame(
    void*, const SnapFrameFrameInput* input, SnapFrameFrameOutput* output) {
    if (!input || !output || !input->native_frame)
        return SNAP_FRAME_ERROR_INVALID_ARGUMENT;

    const auto* frame = reinterpret_cast<const NativeFrame*>(input->native_frame);
    const int ok = snapframe_host_process_legacy_frame(
        frame->context, frame->source_view, frame->device,
        frame->width, frame->height, frame->format);
    if (!ok) return SNAP_FRAME_ERROR_BACKEND;

    /* The WSI present-result callback is asynchronous relative to this call.
     * Count accepted generated frames in the host hook, not at dispatch time. */
    output->generated_frames = 0;
    output->capture_gpu_ms = 0.0f;
    output->preprocess_gpu_ms = 0.0f;
    output->flow_gpu_ms = 0.0f;
    output->consistency_gpu_ms = 0.0f;
    output->warp_gpu_ms = 0.0f;
    output->composite_gpu_ms = 0.0f;
    return SNAP_FRAME_OK;
}

} // namespace

extern "C" int snapframe_engine_adapter_init(const SnapFrameConfig* legacy_config) {
    if (!legacy_config || snap_frame_get_api_version() != SNAP_FRAME_API_VERSION)
        return 0;

    SnapFrameEngineConfig config{};
    snap_frame_default_config(&config);
    config.enabled = legacy_config->enabled ? 1u : 0u;
    config.multiplier = legacy_config->multiplier;
    config.quality = static_cast<SnapFrameEngineQuality>(legacy_config->quality);
    config.flow_scale_percent = static_cast<uint32_t>(legacy_config->flow_scale_percent);
    config.motion_analysis = legacy_config->motion_analysis ? 1u : 0u;
    config.profiling_enabled = legacy_config->profiling_enabled ? 1u : 0u;
    config.diagnostic_suite = legacy_config->diagnostic_suite ? 1u : 0u;
    config.diagnostic_phase_frames = static_cast<uint32_t>(legacy_config->diagnostic_phase_frames);

    SnapFrameBackendV1 backend{};
    backend.struct_size = sizeof(backend);
    backend.api_version = SNAP_FRAME_API_VERSION;
    backend.initialize = backendInitialize;
    backend.shutdown = backendShutdown;
    backend.update_config = backendUpdateConfig;
    backend.reset_history = backendResetHistory;
    backend.process_frame = backendProcessFrame;

    std::lock_guard<std::mutex> lock(g_adapter_mutex);
    if (g_engine) {
        snap_frame_destroy(g_engine);
        g_engine = nullptr;
    }
    g_config = config;
    g_frame_id.store(0, std::memory_order_relaxed);
    g_engine = snap_frame_create(&g_config, &backend);
    return g_engine != nullptr ? 1 : 0;
}

extern "C" void snapframe_engine_adapter_shutdown(void) {
    std::lock_guard<std::mutex> lock(g_adapter_mutex);
    if (g_engine) {
        snap_frame_destroy(g_engine);
        g_engine = nullptr;
    }
    g_frame_id.store(0, std::memory_order_relaxed);
}

extern "C" int snapframe_engine_adapter_update_settings(int enabled, int flow_scale_percent) {
    if (flow_scale_percent < 25 || flow_scale_percent > 100)
        return 0;

    std::lock_guard<std::mutex> lock(g_adapter_mutex);
    if (!g_engine) return 0;

    SnapFrameEngineConfig next = g_config;
    next.enabled = enabled ? 1u : 0u;
    next.flow_scale_percent = static_cast<uint32_t>(flow_scale_percent);
    if (snap_frame_set_config(g_engine, &next) != SNAP_FRAME_OK)
        return 0;
    g_config = next;
    return 1;
}

/* Returns 1 when the portable core handled the frame (including bypass),
 * 0 when no core exists and the caller should use the legacy path, or -1
 * when the core/backend failed and the real frame must be presented unchanged. */
extern "C" int snapframe_engine_adapter_process_frame(
    void* context, void* source_view, void* device,
    uint32_t width, uint32_t height, uint32_t format) {
    std::lock_guard<std::mutex> lock(g_adapter_mutex);
    if (!g_engine) return 0;

    NativeFrame native_frame{context, source_view, device, width, height, format};
    SnapFrameFrameInput input{};
    input.struct_size = sizeof(input);
    input.frame_id = g_frame_id.fetch_add(1, std::memory_order_relaxed) + 1;
    input.width = width;
    input.height = height;
    input.format = format;
    input.native_frame = reinterpret_cast<uintptr_t>(&native_frame);

    SnapFrameFrameOutput output{};
    output.struct_size = sizeof(output);
    const SnapFrameStatus status = snap_frame_process(g_engine, &input, &output);
    if (status == SNAP_FRAME_OK || status == SNAP_FRAME_BYPASSED)
        return 1;
    return -1;
}

extern "C" int snapframe_engine_adapter_available(void) {
    std::lock_guard<std::mutex> lock(g_adapter_mutex);
    return g_engine != nullptr ? 1 : 0;
}
