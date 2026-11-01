#pragma once

#include <cstdlib>
#include <cstdint>
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

inline LsfgGpuTimer lsfgGpuTimerBegin(
    DxvkContext* context, DxvkDevice* device, const char* stage,
    uint64_t frame, uint32_t width, uint32_t height) {
  LsfgGpuTimer timer;
  if (!context || !device || !stage)
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

    static std::mutex csvMutex;
    std::lock_guard<std::mutex> lock(csvMutex);
    const std::string path = lsfgGpuCsvPath();
    std::ifstream existing(path.c_str());
    const bool writeHeader = !existing.good();
    existing.close();

    std::ofstream file(path.c_str(), std::ios::out | std::ios::app);
    if (!file.good())
      return;
    if (writeHeader)
      file << "frame,stage,gpu_ms,width,height,status\n";

    if (valid) {
      const double gpuMs = double(endData.timestamp.time - beginData.timestamp.time)
                         * timer.periodNs / 1000000.0;
      file << timer.frame << ',' << timer.stage << ',' << gpuMs << ','
           << timer.width << ',' << timer.height << ",ok\n";
    } else {
      file << timer.frame << ',' << timer.stage << ",0,"
           << timer.width << ',' << timer.height << ",unavailable:"
           << int(result) << ':' << int(beginStatus) << ':' << int(endStatus) << "\n";
    }
  });
}

} // namespace dxvk
