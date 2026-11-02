#pragma once

#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>

#include "dxvk_context.h"
#include "dxvk_device.h"
#include "dxvk_gpu_query.h"

namespace dxvk {

/*
 * Non-blocking LSFG GPU timing. Both timestamp writes are recorded in the
 * caller's active DXVK command list. Results are read only from its completion
 * callback, after the GPU fence has signalled and before query handles recycle.
 */
struct LsfgGpuTimer {
  Rc<DxvkGpuQuery> begin;
  Rc<DxvkGpuQuery> end;
  double periodNs = 0.0;
  uint64_t frame = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  std::string stage;
};

inline std::string lsfgGpuCsvPath() {
  const char* explicitPath = std::getenv("DXVK_LSFG_GPU_PROFILE_LOG");
  if (explicitPath && *explicitPath)
    return std::string(explicitPath);

  const char* dir = std::getenv("DXVK_LOG_PATH");
  if (dir && *dir && std::string(dir) != "none")
    return std::string(dir) + "/dxvk_lsfg_gpu.csv";

  const char* perf = std::getenv("DXVK_PERF_LOG");
  if (perf && *perf) {
    std::string path(perf);
    const size_t slash = path.find_last_of('/');
    if (slash != std::string::npos)
      return path.substr(0, slash + 1) + "dxvk_lsfg_gpu.csv";
  }

  return "port/dxvk_lsfg_gpu.csv";
}

// GPU timestamp queries add query-pool objects, command-buffer writes and
// completion-callback/file-I/O work. Keep them opt-in so normal gameplay does
// not pay for diagnostics. Set DXVK_LSFG_GPU_PROFILE=1 for a profiling run.
inline bool lsfgGpuProfilerEnabled() {
  static const bool enabled = []() {
    const char* value = std::getenv("DXVK_LSFG_GPU_PROFILE");
    return value && (std::strcmp(value, "1") == 0
                  || std::strcmp(value, "true") == 0
                  || std::strcmp(value, "yes") == 0);
  }();
  return enabled;
}

// One CSV belongs to one application launch. Reset it before LSFG's runtime
// enable gate so an OFF run cannot leave rows from a previous ON session.
struct LsfgGpuCsvState {
  std::mutex mutex;
  std::ofstream file;
  std::string path;
  uint32_t rowsSinceFlush = 0;
};

inline LsfgGpuCsvState& lsfgGpuCsvState() {
  static LsfgGpuCsvState state;
  return state;
}

inline bool lsfgGpuProfilerBeginSession(bool lsfgRuntimeEnabled) {
  LsfgGpuCsvState& state = lsfgGpuCsvState();
  std::lock_guard<std::mutex> lock(state.mutex);

  if (state.file.is_open()) {
    state.file.flush();
    state.file.close();
  }

  state.path = lsfgGpuCsvPath();
  state.rowsSinceFlush = 0;
  state.file.open(state.path.c_str(), std::ios::out | std::ios::trunc);
  if (!state.file.good()) {
    state.file.close();
    state.path.clear();
    return false;
  }

  state.file << "frame,stage,gpu_ms,width,height,status\n";
  const char* status = !lsfgGpuProfilerEnabled()
    ? "profiling_disabled"
    : (lsfgRuntimeEnabled ? "session_started" : "lsfg_disabled");
  state.file << "0,SESSION,0,0,0," << status << "\n";
  state.file.flush();
  if (!state.file.good()) {
    state.file.close();
    state.path.clear();
    return false;
  }
  return true;
}

inline LsfgGpuTimer lsfgGpuTimerBegin(
    DxvkContext* context, DxvkDevice* device, const char* stage,
    uint64_t frame, uint32_t width, uint32_t height) {
  LsfgGpuTimer timer;
  if (!lsfgGpuProfilerEnabled() || !context || !device || !stage)
    return timer;

  timer.begin = new DxvkGpuQuery(device->vkd(), VK_QUERY_TYPE_TIMESTAMP, 0, 0);
  timer.end = new DxvkGpuQuery(device->vkd(), VK_QUERY_TYPE_TIMESTAMP, 0, 0);
  timer.periodNs = device->adapter()->deviceProperties().limits.timestampPeriod;
  timer.frame = frame;
  timer.width = width;
  timer.height = height;
  timer.stage = stage;
  context->writeTimestamp(timer.begin);
  return timer;
}

inline void lsfgGpuTimerEnd(DxvkContext* context, const LsfgGpuTimer& timer) {
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

    // Keep the file open and serialize callback writes. Opening, probing and
    // closing the CSV once per stage caused thousands of filesystem operations
    // during a profiling run and distorted the CPU-side PresentImage timings.
    LsfgGpuCsvState& csv = lsfgGpuCsvState();
    std::lock_guard<std::mutex> lock(csv.mutex);

    const std::string path = lsfgGpuCsvPath();
    if (!csv.file.is_open() || path != csv.path) {
      if (csv.file.is_open()) {
        csv.file.flush();
        csv.file.close();
      }

      csv.path = path;
      csv.rowsSinceFlush = 0;
      // A path change also starts a new session; never append stale rows.
      csv.file.open(path.c_str(), std::ios::out | std::ios::trunc);
      if (!csv.file.good()) {
        csv.file.close();
        csv.path.clear();
        return;
      }
      csv.file << "frame,stage,gpu_ms,width,height,status\n";
    }

    if (valid) {
      const double gpuMs = double(endData.timestamp.time - beginData.timestamp.time)
                         * timer.periodNs / 1000000.0;
      csv.file << timer.frame << ',' << timer.stage << ',' << gpuMs << ','
               << timer.width << ',' << timer.height << ",ok\n";
    } else {
      csv.file << timer.frame << ',' << timer.stage << ",0,"
               << timer.width << ',' << timer.height << ",unavailable:"
               << int(result) << ':' << int(beginStatus) << ':' << int(endStatus) << "\n";
    }

    // Bound data loss on an abnormal exit without flushing every single row.
    if (++csv.rowsSinceFlush >= 60) {
      csv.file.flush();
      csv.rowsSinceFlush = 0;
      if (!csv.file.good()) {
        csv.file.close();
        csv.path.clear();
      }
    }
  });
}

} // namespace dxvk
