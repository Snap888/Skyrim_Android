/*
 * Public SnapFrame runtime ABI for the isolated DXVK Native 2.6.2 DSO.
 * Only capabilities actually implemented by the 2.6.2 command-stream host
 * are accepted. Midpoint output is not advertised as presented; WSI is gated.
 */
#include "snapframe_host.h"
#include "snapframe_dxvk262_api.h"
#include <android/log.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <mutex>

namespace {
constexpr const char* kTag = "SnapFrame262Runtime";
std::mutex g_mutex;
FILE* g_log = nullptr;
FILE* g_gpu_log = nullptr;
FILE* g_flow_log = nullptr;
FILE* g_flow_vectors_log = nullptr;
FILE* g_event_log = nullptr;
bool g_initialized = false;
bool g_enabled = false;
constexpr int kSupportedFlowScale = 50;

int replaceLog(FILE** slot, const char* path) {
  if (!path || !*path) return -EINVAL;
  FILE* next = std::fopen(path, "a");
  if (!next) return -errno;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (*slot) std::fclose(*slot);
  *slot = next;
  return 0;
}
void writeLog(const char* message) {
  const char* value = message ? message : "";
  __android_log_print(ANDROID_LOG_INFO, kTag, "%s", value);
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_log) { std::fprintf(g_log, "%s\n", value); std::fflush(g_log); }
}
bool validConfig(const SnapFrameConfig* c) {
  return c && (c->enabled == 0 || c->enabled == 1)
      && c->flow_scale_percent == kSupportedFlowScale
      && c->present_mode == SNAPFRAME_PRESENT_MODE_AUTO
      && c->multiplier >= 1 && c->multiplier <= 2
      && (!c->enabled || c->multiplier == 1)
      && c->quality == SNAPFRAME_QUALITY_PERFORMANCE
      && c->motion_analysis == 1
      && c->ai_mode == SNAPFRAME_AI_MANUAL
      && c->profiling_enabled == 0 && c->diagnostic_suite == 0;
}
}
extern "C" void snapframe_config_defaults(SnapFrameConfig* c) {
  if (!c) return;
  std::memset(c, 0, sizeof(*c));
  c->enabled = 0;
  c->present_mode = SNAPFRAME_PRESENT_MODE_AUTO;
  c->multiplier = 1;
  c->quality = SNAPFRAME_QUALITY_PERFORMANCE;
  c->flow_scale_percent = kSupportedFlowScale;
  c->motion_analysis = 1;
  c->ai_mode = SNAPFRAME_AI_MANUAL;
}
extern "C" int snapframe_host_set_log_path(const char* p) { return replaceLog(&g_log, p); }
extern "C" int snapframe_host_set_gpu_log_path(const char* p) { return replaceLog(&g_gpu_log, p); }
extern "C" int snapframe_host_set_flow_log_path(const char* p) { return replaceLog(&g_flow_log, p); }
extern "C" int snapframe_host_set_flow_vectors_log_path(const char* p) { return replaceLog(&g_flow_vectors_log, p); }
extern "C" int snapframe_host_set_event_log_path(const char* p) { return replaceLog(&g_event_log, p); }
extern "C" void snapframe_host_log(const char* p) { writeLog(p); }

extern "C" int snapframe_host_init(const SnapFrameConfig* c) {
  if (!validConfig(c)) {
    writeLog("Rejected unsupported DXVK 2.6.2 SnapFrame configuration");
    return -ENOTSUP;
  }
  if (!snapframe_dxvk262_set_enabled(c->enabled)) return -ENODEV;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_enabled = c->enabled != 0;
    g_initialized = true;
  }
  writeLog(c->enabled
    ? "Capture/compute enabled; generated WSI present remains disabled"
    : "Runtime initialized disabled");
  return 0;
}
extern "C" void snapframe_host_shutdown(void) {
  snapframe_dxvk262_set_enabled(0);
  std::lock_guard<std::mutex> lock(g_mutex);
  g_enabled = false;
  g_initialized = false;
  for (FILE** f : {&g_event_log, &g_flow_vectors_log, &g_flow_log, &g_gpu_log, &g_log}) {
    if (*f) { std::fclose(*f); *f = nullptr; }
  }
}
extern "C" int snapframe_host_active(void) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_initialized && g_enabled ? 1 : 0;
}
extern "C" int snapframe_host_set_enabled(int enabled) {
  if (enabled != 0 && enabled != 1) return -EINVAL;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized) return -EPIPE;
  }
  if (!snapframe_dxvk262_set_enabled(enabled)) return -ENODEV;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_enabled = enabled != 0;
  return 0;
}
extern "C" void snapframe_host_get_stats(SnapFrameStats* s) {
  if (s) std::memset(s, 0, sizeof(*s));
}
extern "C" int snapframe_host_get_settings(int* enabled, int* scale) {
  if (!enabled || !scale) return -EINVAL;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_initialized) return -EPIPE;
  *enabled = g_enabled ? 1 : 0;
  *scale = kSupportedFlowScale;
  return 0;
}
extern "C" int snapframe_host_get_present_mode(int* mode) {
  if (!mode) return -EINVAL;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_initialized) return -EPIPE;
  *mode = SNAPFRAME_PRESENT_MODE_AUTO;
  return 0;
}
extern "C" int snapframe_host_update_present_mode(int mode) {
  return mode == SNAPFRAME_PRESENT_MODE_AUTO ? 0 : -ENOTSUP;
}
extern "C" int snapframe_host_update_settings(int enabled, int scale) {
  if (enabled != 0 && enabled != 1) return -EINVAL;
  if (scale != kSupportedFlowScale) return -ENOTSUP;
  return snapframe_host_set_enabled(enabled);
}
extern "C" int snapframe_host_get_last_telemetry(uint64_t* frame, float* gpu, float* frame_time) {
  if (!frame || !gpu || !frame_time) return -EINVAL;
  *frame = 0; *gpu = 0.0f; *frame_time = 0.0f;
  return -ENODATA;
}
