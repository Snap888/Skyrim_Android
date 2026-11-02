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
    static std::mutex csvMutex;
    static std::ofstream csvFile;
    static std::string openedPath;
    static uint32_t rowsSinceFlush = 0;
    std::lock_guard<std::mutex> lock(csvMutex);

    const std::string path = lsfgGpuCsvPath();
    if (!csvFile.is_open() || path != openedPath) {
      if (csvFile.is_open()) {
        csvFile.flush();
        csvFile.close();
      }

      openedPath = path;
      std::ifstream existing(path.c_str());
      const bool writeHeader = !existing.good();
      existing.close();

      csvFile.open(path.c_str(), std::ios::out | std::ios::app);
      if (!csvFile.good()) {
        csvFile.close();
        openedPath.clear();
        rowsSinceFlush = 0;
        return;
      }
      if (writeHeader)
        csvFile << "frame,stage,gpu_ms,width,height,status\n";
      rowsSinceFlush = 0;
    }

    if (valid) {
      const double gpuMs = double(endData.timestamp.time - beginData.timestamp.time)
                         * timer.periodNs / 1000000.0;
      csvFile << timer.frame << ',' << timer.stage << ',' << gpuMs << ','
              << timer.width << ',' << timer.height << ",ok\n";
    } else {
      csvFile << timer.frame << ',' << timer.stage << ",0,"
              << timer.width << ',' << timer.height << ",unavailable:"
              << int(result) << ':' << int(beginStatus) << ':' << int(endStatus) << "\n";
    }

    // Bound data loss on an abnormal exit without flushing every single row.
    if (++rowsSinceFlush >= 60) {
      csvFile.flush();
      rowsSinceFlush = 0;
      if (!csvFile.good()) {
        csvFile.close();
        openedPath.clear();
      }
    }
  });
}

} // namespace dxvk
