#include "dxvk_device.h"
#include "dxvk_queue.h"

#ifdef __ANDROID__
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace dxvk {
  
  DxvkSubmissionQueue::DxvkSubmissionQueue(DxvkDevice* device)
  : m_device(device),
    m_submitThread([this] () { submitCmdLists(); }),
    m_finishThread([this] () { finishCmdLists(); }) {

  }
  
  
  DxvkSubmissionQueue::~DxvkSubmissionQueue() {
    { std::unique_lock<dxvk::mutex> lock(m_mutex);
      m_stopped.store(true);
    }
    
    m_appendCond.notify_all();
    m_submitCond.notify_all();

    m_submitThread.join();
    m_finishThread.join();
  }
  
  
  void DxvkSubmissionQueue::submit(DxvkSubmitInfo submitInfo) {
    std::unique_lock<dxvk::mutex> lock(m_mutex);

    m_finishCond.wait(lock, [this] {
      return m_submitQueue.size() + m_finishQueue.size() <= MaxNumQueuedCommandBuffers;
    });

    DxvkSubmitEntry entry = { };
    entry.enqueuedAt = std::chrono::steady_clock::now();
    entry.submit = std::move(submitInfo);

    m_pending += 1;
    m_submitQueue.push(std::move(entry));
    m_appendCond.notify_all();
  }


  void DxvkSubmissionQueue::present(DxvkPresentInfo presentInfo, DxvkSubmitStatus* status) {
    std::unique_lock<dxvk::mutex> lock(m_mutex);

    DxvkSubmitEntry entry = { };
    entry.enqueuedAt = std::chrono::steady_clock::now();
    entry.status  = status;
    entry.present = std::move(presentInfo);

    m_submitQueue.push(std::move(entry));
    m_appendCond.notify_all();
  }


  void DxvkSubmissionQueue::synchronizeSubmission(
          DxvkSubmitStatus*   status) {
    std::unique_lock<dxvk::mutex> lock(m_mutex);

    m_submitCond.wait(lock, [status] {
      return status->result.load() != VK_NOT_READY;
    });
  }


  void DxvkSubmissionQueue::synchronize() {
    std::unique_lock<dxvk::mutex> lock(m_mutex);

    m_submitCond.wait(lock, [this] {
      return m_submitQueue.empty();
    });
  }


  void DxvkSubmissionQueue::lockDeviceQueue() {
    m_mutexQueue.lock();
  }


  void DxvkSubmissionQueue::unlockDeviceQueue() {
    m_mutexQueue.unlock();
  }


  void DxvkSubmissionQueue::submitCmdLists() {
    env::setThreadName("dxvk-submit");

#ifdef __ANDROID__
    if (env::getEnvVar("DXVK_THREAD_DIAGNOSTICS") == "1") {
      Logger::info(str::format("DXVK-THREAD: role=submit tid=",
        ::syscall(SYS_gettid), " cpu=", ::sched_getcpu()));
    }
#endif

    std::unique_lock<dxvk::mutex> lock(m_mutex);

    while (!m_stopped.load()) {
      m_appendCond.wait(lock, [this] {
        return m_stopped.load() || !m_submitQueue.empty();
      });
      
      if (m_stopped.load())
        return;
      
      DxvkSubmitEntry entry = std::move(m_submitQueue.front());
      const uint32_t queuedEntriesAtDequeue =
        static_cast<uint32_t>(m_submitQueue.size() - 1);
      const auto dequeuedAt = std::chrono::steady_clock::now();
      const uint64_t queueResidenceUs =
        std::chrono::duration_cast<std::chrono::microseconds>(
          dequeuedAt - entry.enqueuedAt).count();
      lock.unlock();

      // Submit command buffer to device
      VkResult status = VK_NOT_READY;

      uint64_t queueLockWaitUs = 0;
      uint64_t presentCallUs = 0;

      if (m_lastError != VK_ERROR_DEVICE_LOST) {
        auto queueLockStart = dxvk::high_resolution_clock::now();
        {
          std::unique_lock<dxvk::mutex> queueLock(m_mutexQueue);
          auto queueLockAcquired = dxvk::high_resolution_clock::now();
          queueLockWaitUs = std::chrono::duration_cast<std::chrono::microseconds>(
            queueLockAcquired - queueLockStart).count();

          if (entry.submit.cmdList != nullptr) {
            status = entry.submit.cmdList->submit(
              entry.submit.waitSync,
              entry.submit.wakeSync);
          } else if (entry.present.presenter != nullptr) {
            auto presentStart = dxvk::high_resolution_clock::now();
            status = entry.present.presenter->presentImage(
              entry.present.applyFrameRateLimit);
            auto presentEnd = dxvk::high_resolution_clock::now();
            presentCallUs = std::chrono::duration_cast<std::chrono::microseconds>(
              presentEnd - presentStart).count();
          }
        }

#ifdef __ANDROID__
        /* Always collect rare long stalls on Android. This is bounded and
         * avoids relying on launcher environment-variable support. Queue
         * residence distinguishes submit-thread backlog from Vulkan queue
         * mutex contention and vkQueuePresentKHR/WSI latency. */
        static std::atomic<uint32_t> presentDiagnosticCount = { 0u };
        if (entry.present.presenter != nullptr
            && (queueResidenceUs >= 8000
                || queueLockWaitUs >= 8000
                || presentCallUs >= 8000)
            && presentDiagnosticCount.fetch_add(1) < 64) {
          const uint64_t totalEntryUs =
            std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now() - entry.enqueuedAt).count();
          Logger::info(str::format(
            "DXVK-PRESENT: queue_residence_us=", queueResidenceUs,
            " queue_depth=", queuedEntriesAtDequeue,
            " queue_lock_wait_us=", queueLockWaitUs,
            " present_call_us=", presentCallUs,
            " total_entry_us=", totalEntryUs,
            " result=", status,
            " diagnostic_count=", presentDiagnosticCount.load()));
        }
#endif
      } else {
        // Don't submit anything after device loss
        // so that drivers get a chance to recover
        status = VK_ERROR_DEVICE_LOST;
      }

      if (entry.status)
        entry.status->result = status;
      
      // On success, pass it on to the queue thread
      lock = std::unique_lock<dxvk::mutex>(m_mutex);

      if (status == VK_SUCCESS) {
        if (entry.submit.cmdList != nullptr)
          m_finishQueue.push(std::move(entry));
      } else if (status == VK_ERROR_DEVICE_LOST || entry.submit.cmdList != nullptr) {
        Logger::err(str::format("DxvkSubmissionQueue: Command submission failed: ", status));
        m_lastError = status;
        m_device->waitForIdle();
      }

      m_submitQueue.pop();
      m_submitCond.notify_all();
    }
  }
  
  
  void DxvkSubmissionQueue::finishCmdLists() {
    env::setThreadName("dxvk-queue");

#ifdef __ANDROID__
    if (env::getEnvVar("DXVK_THREAD_DIAGNOSTICS") == "1") {
      Logger::info(str::format("DXVK-THREAD: role=finish tid=",
        ::syscall(SYS_gettid), " cpu=", ::sched_getcpu()));
    }
#endif

    std::unique_lock<dxvk::mutex> lock(m_mutex);

    while (!m_stopped.load()) {
      if (m_finishQueue.empty()) {
        auto t0 = dxvk::high_resolution_clock::now();

        m_submitCond.wait(lock, [this] {
          return m_stopped.load() || !m_finishQueue.empty();
        });

        auto t1 = dxvk::high_resolution_clock::now();
        m_gpuIdle += std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
      }

      if (m_stopped.load())
        return;
      
      DxvkSubmitEntry entry = std::move(m_finishQueue.front());
      lock.unlock();
      
      VkResult status = m_lastError.load();
      
      if (status != VK_ERROR_DEVICE_LOST)
        status = entry.submit.cmdList->synchronize();
      
      if (status != VK_SUCCESS) {
        Logger::err(str::format("DxvkSubmissionQueue: Failed to sync fence: ", status));
        m_lastError = status;
        m_device->waitForIdle();
      }

      entry.submit.cmdList->notifyCompletion(status);
      entry.submit.cmdList->notifySignals();
      entry.submit.cmdList->reset();

      m_device->recycleCommandList(entry.submit.cmdList);

      lock = std::unique_lock<dxvk::mutex>(m_mutex);
      m_pending -= 1;

      m_finishQueue.pop();
      m_finishCond.notify_all();
    }
  }
  
}