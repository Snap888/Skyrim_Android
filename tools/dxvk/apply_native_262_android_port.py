#!/usr/bin/env python3
"""Apply the reviewed Android WSI/Presenter overlay to pinned DXVK Native 2.6.2.

The patch is intentionally narrow: it does not claim to produce a complete
Android build. It adds generation-owned ANativeWindow lifetime, Android
surface creation, and the Vulkan instance extension required by that surface.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import shutil


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"expected exactly one patch anchor in {path}: found {count}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source_root", type=Path)
    parser.add_argument("--project-root", type=Path, required=True)
    args = parser.parse_args()
    root = args.source_root.resolve()
    project = args.project_root.resolve()

    expected = "9d6f54a1ade20d1d27dd421024717a636f3d8c68"
    revision_file = root / ".git" / "HEAD"
    if not revision_file.exists():
        raise SystemExit("source root is not a git checkout")
    import subprocess
    actual = subprocess.check_output(
        ["git", "-C", str(root), "rev-parse", "HEAD"], text=True
    ).strip()
    if actual != expected:
        raise SystemExit(f"wrong DXVK source revision: {actual}; expected {expected}")

    # Resolve the DXVK source directory before any source-file paths are built.
    dxvk_dir = root / "src/dxvk"

    # DXVK's loader header force-enables Win32 Vulkan declarations even for
    # Android. Keep those declarations on Windows only. Android still parses
    # the optional full-screen-exclusive extension fields, so provide the
    # extension's ABI declarations/constants without importing Win32 headers.
    vulkan_loader = root / "src/vulkan/vulkan_loader.h"
    replace_once(
        vulkan_loader,
        """#define VK_USE_PLATFORM_WIN32_KHR 1
""",
        """#if defined(_WIN32) && !defined(__ANDROID__)
#define VK_USE_PLATFORM_WIN32_KHR 1
#endif
""",
    )
    replace_once(
        vulkan_loader,
        """#include <vulkan/vulkan.h>
""",
        """#include <vulkan/vulkan.h>
#if defined(__ANDROID__)
// DXVK references optional Win32-only extension names in its common feature
// inventory. These string constants are harmless on Android: the driver will
// never advertise the Win32 extensions. Full-screen-exclusive is likewise
// disabled by the driver, but the common Presenter code still needs its ABI.
#ifndef VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME
#define VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME "VK_EXT_full_screen_exclusive"
typedef enum VkFullScreenExclusiveEXT {
  VK_FULL_SCREEN_EXCLUSIVE_DEFAULT_EXT = 0,
  VK_FULL_SCREEN_EXCLUSIVE_ALLOWED_EXT = 1,
  VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT = 2,
  VK_FULL_SCREEN_EXCLUSIVE_APPLICATION_CONTROLLED_EXT = 3
} VkFullScreenExclusiveEXT;
#define VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT ((VkStructureType)1000255000)
typedef struct VkSurfaceFullScreenExclusiveInfoEXT {
  VkStructureType sType;
  const void* pNext;
  VkFullScreenExclusiveEXT fullScreenExclusive;
} VkSurfaceFullScreenExclusiveInfoEXT;
#endif
#ifndef VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME
#define VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME "VK_KHR_external_memory_win32"
#endif
#ifndef VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME
#define VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME "VK_KHR_external_semaphore_win32"
#endif
#ifndef VK_KHR_WIN32_KEYED_MUTEX_EXTENSION_NAME
#define VK_KHR_WIN32_KEYED_MUTEX_EXTENSION_NAME "VK_KHR_win32_keyed_mutex"
#endif
#endif
""",
    )

    # The loader header normally gets HMODULE from Vulkan's Win32 platform
    # declarations. Android must not enable VK_USE_PLATFORM_WIN32_KHR, but the
    # loader still uses the compatibility HMODULE/dlopen abstraction. Include
    # that abstraction explicitly so LibraryFn remains well-formed.
    replace_once(
        vulkan_loader,
        """#include "../util/rc/util_rc_ptr.h"

""",
        """#include "../util/rc/util_rc_ptr.h"
#include "../util/util_win32_compat.h"

""",
    )

    # DXVK's shared semaphore HANDLE import/export path is Win32-only.
    # Android uses native Vulkan synchronization and must not compile Win32
    # handle structures or DeviceFn entry points from dxvk_fence.cpp.
    fence_cpp = dxvk_dir / "dxvk_fence.cpp"
    replace_once(
        fence_cpp,
        """    if (info.sharedHandle != INVALID_HANDLE_VALUE) {
      if (externalFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT) {
        VkImportSemaphoreWin32HandleInfoKHR importInfo = { VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR };
        importInfo.semaphore = m_semaphore;
        importInfo.handleType = info.sharedType;
        importInfo.handle = info.sharedHandle;

        vr = m_vkd->vkImportSemaphoreWin32HandleKHR(m_vkd->device(), &importInfo);
        if (vr != VK_SUCCESS)
          throw DxvkError("Failed to import timeline semaphore");
      } else {
        Logger::warn(str::format("Importing semaphores of type ", info.sharedType, " not supported by device"));
      }
    }
""",
        """#ifdef _WIN32
    if (info.sharedHandle != INVALID_HANDLE_VALUE) {
      if (externalFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT) {
        VkImportSemaphoreWin32HandleInfoKHR importInfo = { VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR };
        importInfo.semaphore = m_semaphore;
        importInfo.handleType = info.sharedType;
        importInfo.handle = info.sharedHandle;

        vr = m_vkd->vkImportSemaphoreWin32HandleKHR(m_vkd->device(), &importInfo);
        if (vr != VK_SUCCESS)
          throw DxvkError("Failed to import timeline semaphore");
      } else {
        Logger::warn(str::format("Importing semaphores of type ", info.sharedType, " not supported by device"));
      }
    }
#else
    if (info.sharedHandle != INVALID_HANDLE_VALUE)
      Logger::warn("External Win32 semaphore handles are unavailable on this platform");
#endif
""",
    )
    replace_once(
        fence_cpp,
        """  HANDLE DxvkFence::sharedHandle() const {
    if (m_info.sharedType == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_FLAG_BITS_MAX_ENUM)
      return INVALID_HANDLE_VALUE;

    VkSemaphoreGetWin32HandleInfoKHR win32HandleInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR };
    win32HandleInfo.semaphore = m_semaphore;
    win32HandleInfo.handleType = m_info.sharedType;

    HANDLE sharedHandle = INVALID_HANDLE_VALUE;
    VkResult vr = m_vkd->vkGetSemaphoreWin32HandleKHR(m_vkd->device(), &win32HandleInfo, &sharedHandle);

    if (vr != VK_SUCCESS)
      Logger::err(str::format("Failed to get semaphore handle: ", vr));

    return sharedHandle;
  }
""",
        """  HANDLE DxvkFence::sharedHandle() const {
#ifdef _WIN32
    if (m_info.sharedType == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_FLAG_BITS_MAX_ENUM)
      return INVALID_HANDLE_VALUE;

    VkSemaphoreGetWin32HandleInfoKHR win32HandleInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR };
    win32HandleInfo.semaphore = m_semaphore;
    win32HandleInfo.handleType = m_info.sharedType;

    HANDLE sharedHandle = INVALID_HANDLE_VALUE;
    VkResult vr = m_vkd->vkGetSemaphoreWin32HandleKHR(m_vkd->device(), &win32HandleInfo, &sharedHandle);

    if (vr != VK_SUCCESS)
      Logger::err(str::format("Failed to get semaphore handle: ", vr));

    return sharedHandle;
#else
    return INVALID_HANDLE_VALUE;
#endif
  }
""",
    )

    # DXVK shared-image HANDLE import is a Win32-only interop path. Android
    # Vulkan exposes FD imports, but DXVK's DxvkSharedImageInfo carries a Win32
    # HANDLE and Win32 handle type; silently treating it as an FD is invalid.
    # Keep ordinary image allocation intact and reject this optional interop
    # path explicitly on Android.
    image_cpp = dxvk_dir / "dxvk_image.cpp"
    replace_once(
        image_cpp,
        """    // Set up external memory parameters for shared images
    VkExternalMemoryImageCreateInfo externalInfo = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };

    if (m_shared) {
      externalInfo.pNext = std::exchange(imageInfo.pNext, &externalInfo);
      externalInfo.handleTypes = m_info.sharing.type;
    }

    // Set up shared memory properties
    void* sharedMemoryInfo = nullptr;

    VkExportMemoryAllocateInfo sharedExport = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO };
    VkImportMemoryWin32HandleInfoKHR sharedImportWin32 = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR };

    if (m_shared && m_info.sharing.mode == DxvkSharedHandleMode::Export) {
      sharedExport.pNext = std::exchange(sharedMemoryInfo, &sharedExport);
      sharedExport.handleTypes = m_info.sharing.type;
    }

    if (m_shared && m_info.sharing.mode == DxvkSharedHandleMode::Import) {
      sharedImportWin32.pNext = std::exchange(sharedMemoryInfo, &sharedImportWin32);
      sharedImportWin32.handleType = m_info.sharing.type;
      sharedImportWin32.handle = m_info.sharing.handle;
    }
""",
        """    // Set up external memory parameters for shared images. DXVK's shared
    // image handle is a Win32 HANDLE, not an Android native FD.
    void* sharedMemoryInfo = nullptr;
#ifdef _WIN32
    VkExternalMemoryImageCreateInfo externalInfo = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };

    if (m_shared) {
      externalInfo.pNext = std::exchange(imageInfo.pNext, &externalInfo);
      externalInfo.handleTypes = m_info.sharing.type;
    }

    VkExportMemoryAllocateInfo sharedExport = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO };
    VkImportMemoryWin32HandleInfoKHR sharedImportWin32 = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR };

    if (m_shared && m_info.sharing.mode == DxvkSharedHandleMode::Export) {
      sharedExport.pNext = std::exchange(sharedMemoryInfo, &sharedExport);
      sharedExport.handleTypes = m_info.sharing.type;
    }

    if (m_shared && m_info.sharing.mode == DxvkSharedHandleMode::Import) {
      sharedImportWin32.pNext = std::exchange(sharedMemoryInfo, &sharedImportWin32);
      sharedImportWin32.handleType = m_info.sharing.type;
      sharedImportWin32.handle = m_info.sharing.handle;
    }
#else
    if (m_shared)
      Logger::warn("DXVK Win32 shared-image handles are unavailable on Android");
#endif
""",
    )

    # Resolve Presenter source before applying the Android-only present-mode
    # guard below. The full WSI transaction patch reuses this path later.
    presenter_cpp = dxvk_dir / "dxvk_presenter.cpp"

    # VK_EXT_full_screen_exclusive is a Win32-only path. Android must
    # query present modes through the portable KHR surface API and must not
    # compile or load vkGetPhysicalDeviceSurfacePresentModes2EXT.
    presenter_modes = """    VkSurfaceFullScreenExclusiveInfoEXT fullScreenInfo = { VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT };
    fullScreenInfo.fullScreenExclusive = m_fullscreenMode;

    VkPhysicalDeviceSurfaceInfo2KHR surfaceInfo = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR, &fullScreenInfo };
    surfaceInfo.surface = m_surface;

    VkResult status;

    if (m_device->features().extFullScreenExclusive) {
      status = m_vki->vkGetPhysicalDeviceSurfacePresentModes2EXT(
        m_device->adapter()->handle(), &surfaceInfo, &numModes, nullptr);
    } else {
      status = m_vki->vkGetPhysicalDeviceSurfacePresentModesKHR(
        m_device->adapter()->handle(), m_surface, &numModes, nullptr);
    }
"""
    replace_once(
        presenter_cpp,
        presenter_modes,
        """    VkResult status;
#ifdef _WIN32
    VkSurfaceFullScreenExclusiveInfoEXT fullScreenInfo = { VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT };
    fullScreenInfo.fullScreenExclusive = m_fullscreenMode;

    VkPhysicalDeviceSurfaceInfo2KHR surfaceInfo = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR, &fullScreenInfo };
    surfaceInfo.surface = m_surface;

    if (m_device->features().extFullScreenExclusive) {
      status = m_vki->vkGetPhysicalDeviceSurfacePresentModes2EXT(
        m_device->adapter()->handle(), &surfaceInfo, &numModes, nullptr);
    } else
#endif
    {
      status = m_vki->vkGetPhysicalDeviceSurfacePresentModesKHR(
        m_device->adapter()->handle(), m_surface, &numModes, nullptr);
    }
"""
    )
    presenter_modes_second = """    if (m_device->features().extFullScreenExclusive) {
      status = m_vki->vkGetPhysicalDeviceSurfacePresentModes2EXT(
        m_device->adapter()->handle(), &surfaceInfo, &numModes, modes.data());
    } else {
      status = m_vki->vkGetPhysicalDeviceSurfacePresentModesKHR(
        m_device->adapter()->handle(), m_surface, &numModes, modes.data());
    }
"""
    replace_once(
        presenter_cpp,
        presenter_modes_second,
        """#ifdef _WIN32
    if (m_device->features().extFullScreenExclusive) {
      status = m_vki->vkGetPhysicalDeviceSurfacePresentModes2EXT(
        m_device->adapter()->handle(), &surfaceInfo, &numModes, modes.data());
    } else
#endif
    {
      status = m_vki->vkGetPhysicalDeviceSurfacePresentModesKHR(
        m_device->adapter()->handle(), m_surface, &numModes, modes.data());
    }
"""
    )

    # Carry immutable identity for the source WSI acquire all the way from
    # Presenter -> command list -> queued present. This is deliberately the
    # first safe transaction step: one source transaction remains in flight,
    # but present no longer reconstructs its image/semaphore from mutable ring
    # indices. Generated multi-flight scheduling remains disabled.
    presenter_h = dxvk_dir / "dxvk_presenter.h"
    presenter_cpp = dxvk_dir / "dxvk_presenter.cpp"
    queue_h = dxvk_dir / "dxvk_queue.h"
    queue_cpp_path = dxvk_dir / "dxvk_queue.cpp"
    context_h = dxvk_dir / "dxvk_context.h"
    cmdlist_h = dxvk_dir / "dxvk_cmdlist.h"
    device_h = dxvk_dir / "dxvk_device.h"
    device_cpp_path = dxvk_dir / "dxvk_device.cpp"

    swapchain = root / "src/d3d9/d3d9_swapchain.cpp"

    replace_once(
        presenter_h,
        """  struct PresenterSync {
    VkSemaphore acquire = VK_NULL_HANDLE;
    VkSemaphore present = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkBool32 fenceSignaled = VK_FALSE;
  };
""",
        """  struct PresenterSync {
    VkSemaphore acquire = VK_NULL_HANDLE;
    VkSemaphore present = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkBool32 fenceSignaled = VK_FALSE;

    // Immutable identity of the acquired WSI transaction. Handles remain
    // owned by the Presenter semaphore ring; this record identifies which
    // exact image/ring entry the command list and present operation consume.
    uint64_t transactionId = 0u;
    uint64_t swapchainGeneration = 0u;
    uint32_t imageIndex = 0u;
    uint32_t semaphoreIndex = 0u;
  };
""",
    )
    replace_once(
        presenter_h,
        """    VkResult presentImage(
            uint64_t                frameId,
      const Rc<DxvkLatencyTracker>& tracker);
""",
        """    VkResult presentImage(
      const PresenterSync&          transaction,
            uint64_t                frameId,
      const Rc<DxvkLatencyTracker>& tracker);
""",
    )
    replace_once(
        presenter_h,
        """    VkResult                    m_acquireStatus = VK_NOT_READY;
    bool                        m_presentPending = false;
""",
        """    VkResult                    m_acquireStatus = VK_NOT_READY;
    bool                        m_presentPending = false;
    uint64_t                    m_swapchainGeneration = 0u;
    uint64_t                    m_nextTransactionId = 1u;
    uint64_t                    m_activeTransactionId = 0u;
""",
    )

    replace_once(
        presenter_cpp,
        """    sync = m_semaphores.at(m_frameIndex);
    image = m_images.at(m_imageIndex);

    m_presentPending = true;
""",
        """    sync = m_semaphores.at(m_frameIndex);
    sync.transactionId = m_nextTransactionId++;
    sync.swapchainGeneration = m_swapchainGeneration;
    sync.imageIndex = m_imageIndex;
    sync.semaphoreIndex = m_frameIndex;
    image = m_images.at(m_imageIndex);

    m_activeTransactionId = sync.transactionId;
    m_presentPending = true;
""",
    )
    replace_once(
        presenter_cpp,
        """  VkResult Presenter::presentImage(uint64_t frameId, const Rc<DxvkLatencyTracker>& tracker) {
    PresenterSync& currSync = m_semaphores.at(m_frameIndex);
""",
        """  VkResult Presenter::presentImage(
          const PresenterSync& transaction,
                uint64_t frameId,
          const Rc<DxvkLatencyTracker>& tracker) {
    {
      std::lock_guard transactionLock(m_surfaceMutex);

      // Reject stale or malformed ownership rather than presenting an image
      // selected from whichever mutable ring index happens to be current.
      if (!transaction.transactionId
       || transaction.transactionId != m_activeTransactionId
       || !m_presentPending)
        return VK_ERROR_OUT_OF_DATE_KHR;

      // If this is still the active transaction but its captured WSI
      // ownership is malformed, retire it so acquire waiters cannot deadlock.
      if (transaction.swapchainGeneration != m_swapchainGeneration
       || transaction.semaphoreIndex >= m_semaphores.size()
       || transaction.imageIndex >= m_images.size()
       || !m_swapchain) {
        m_activeTransactionId = 0u;
        m_presentPending = false;
        m_surfaceCond.notify_one();
        return VK_ERROR_OUT_OF_DATE_KHR;
      }

      const PresenterSync& ownedSync = m_semaphores.at(transaction.semaphoreIndex);
      if (ownedSync.acquire != transaction.acquire
       || ownedSync.present != transaction.present
       || ownedSync.fence != transaction.fence)
        return VK_ERROR_VALIDATION_FAILED_EXT;
    }

    // acquireNextImage is serialized by m_presentPending until the end of
    // this method, so the validated semaphore ring entry remains stable.
    PresenterSync& currSync = m_semaphores.at(transaction.semaphoreIndex);
    uint32_t presentImageIndex = transaction.imageIndex;
""",
    )
    replace_once(
        presenter_cpp,
        """    fenceInfo.pFences       = &currSync.fence;
""",
        """    fenceInfo.pFences       = &currSync.fence;
""",
    )
    replace_once(
        presenter_cpp,
        """    info.pImageIndices      = &m_imageIndex;
""",
        """    info.pImageIndices      = &presentImageIndex;
""",
    )
    # Generation advances only after a new swapchain has been created and its
    # images imported. No old transaction may be silently retargeted.
    replace_once(
        presenter_cpp,
        """    m_presentPending = false;
    m_surfaceCond.notify_one();
    return status;
""",
        """    m_activeTransactionId = 0u;
    m_presentPending = false;
    m_surfaceCond.notify_one();
    return status;
""",
    )

    replace_once(
        presenter_cpp,
        """    // Create one set of semaphores per swap image, as well as a fence
""",
        """    // Publish a new identity only after the new swapchain exists.
    m_swapchainGeneration++;

    // Create one set of semaphores per swap image, as well as a fence
""",
    )

    replace_once(
        queue_h,
        """  struct DxvkPresentInfo {
    Rc<Presenter>       presenter;
    uint64_t            frameId;
  };
""",
        """  struct DxvkPresentInfo {
    Rc<Presenter>       presenter;
    PresenterSync       transaction;
    uint64_t            frameId;
  };
""",
    )
    replace_once(
        device_h,
        """    void presentImage(
      const Rc<Presenter>&            presenter,
      const Rc<DxvkLatencyTracker>&   tracker,
            uint64_t                  frameId,
            DxvkSubmitStatus*         status);
""",
        """    void presentImage(
      const Rc<Presenter>&            presenter,
      const PresenterSync&            transaction,
      const Rc<DxvkLatencyTracker>&   tracker,
            uint64_t                  frameId,
            DxvkSubmitStatus*         status);
""",
    )
    replace_once(
        device_cpp_path,
        """  void DxvkDevice::presentImage(
    const Rc<Presenter>&            presenter,
    const Rc<DxvkLatencyTracker>&   tracker,
          uint64_t                  frameId,
          DxvkSubmitStatus*         status) {
    DxvkPresentInfo presentInfo = { };
    presentInfo.presenter = presenter;
    presentInfo.frameId = frameId;
""",
        """  void DxvkDevice::presentImage(
    const Rc<Presenter>&            presenter,
    const PresenterSync&            transaction,
    const Rc<DxvkLatencyTracker>&   tracker,
          uint64_t                  frameId,
          DxvkSubmitStatus*         status) {
    DxvkPresentInfo presentInfo = { };
    presentInfo.presenter = presenter;
    presentInfo.transaction = transaction;
    presentInfo.frameId = frameId;
""",
    )
    replace_once(
        queue_cpp_path,
        """          entry.result = entry.present.presenter->presentImage(
            entry.present.frameId, entry.latency.tracker);
""",
        """          entry.result = entry.present.presenter->presentImage(
            entry.present.transaction, entry.present.frameId, entry.latency.tracker);
""",
    )
    replace_once(
        swapchain,
        """        cDevice->presentImage(cPresenter, cLatency, cFrameId, nullptr);
""",
        """        cDevice->presentImage(cPresenter, cSync, cLatency, cFrameId, nullptr);
""",
    )

    # Keep the 2.6.2 SnapFrame seam inside the pinned upstream tree. This
    # C ABI contains opaque 2.6.2 object pointers only; it is not shared with
    # the legacy 1.9.2b host.
    d3d9_dir = root / "src/d3d9"
    d3d9_dir.mkdir(parents=True, exist_ok=True)
    snapframe_api = project / "android/app/jni/snapframe/snapframe_dxvk262_api.h"
    if not snapframe_api.is_file():
        raise SystemExit(f"missing versioned SnapFrame 2.6.2 API: {snapframe_api}")
    shutil.copyfile(snapframe_api, d3d9_dir / "snapframe_dxvk262_api.h")

    # Device teardown runs after DXVK has drained submissions and stopped
    # pipeline workers. Notify the matching host so device-scoped GPU state
    # can be retired without stale device keys.
    dxvk_dir.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(snapframe_api, dxvk_dir / "snapframe_dxvk262_api.h")
    device_cpp = dxvk_dir / "dxvk_device.cpp"
    replace_once(
        device_cpp,
        """#include "dxvk_device.h"
#include "dxvk_instance.h"
""",
        """#include "dxvk_device.h"
#include "snapframe_dxvk262_api.h"
#include "dxvk_instance.h"
""",
    )
    device_text = device_cpp.read_text(encoding="utf-8")
    stop_workers = "m_objects.pipelineManager().stopWorkerThreads();"
    stop_offset = device_text.find(stop_workers)
    if stop_offset < 0 or device_text.find(stop_workers, stop_offset + 1) >= 0:
        raise SystemExit("unexpected DXVK device worker-stop lifecycle; re-review device retirement")
    # Device-owned GPU resources may be retired only after queued submissions
    # have drained and pipeline workers have stopped.
    wait_offset = device_text.rfind("this->waitForIdle();", 0, stop_offset)
    if wait_offset < 0:
        raise SystemExit("DXVK device teardown no longer drains submissions before worker shutdown")
    replace_once(
        device_cpp,
        """    m_objects.pipelineManager().stopWorkerThreads();
  }
""",
        """    m_objects.pipelineManager().stopWorkerThreads();

    if (snapframe_dxvk262_release_device)
      snapframe_dxvk262_release_device(static_cast<void*>(this));
  }
""",
    )

    native_dir = root / "src/wsi/sdl2"
    native_dir.mkdir(parents=True, exist_ok=True)
    for name in ("wsi_native_surface.h", "wsi_native_surface.cpp"):
        src = project / "android/app/jni/dxvk/src/wsi/sdl2" / name
        if not src.is_file():
            raise SystemExit(f"missing project Android WSI source: {src}")
        shutil.copyfile(src, native_dir / name)

    header = native_dir / "wsi_native_surface.h"
    htext = header.read_text(encoding="utf-8")
    htext = htext.replace(
        '#include <memory>\n',
        '#include <memory>\n#include <vulkan/vulkan.h>\n',
        1,
    )
    htext += """
namespace dxvk::wsi {
  // Creates a surface from the exact ANativeWindow generation held by lease.
  VkResult createAndroidSurface(
      PFN_vkGetInstanceProcAddr loader,
      VkInstance instance,
      void* nativeWindow,
      VkSurfaceKHR* surface);
}
"""
    header.write_text(htext, encoding="utf-8")

    platform_cpp = root / "src/wsi/sdl2/wsi_platform_sdl2.cpp"
    include_anchor = '#include <SDL_vulkan.h>\n'
    replace_once(
        platform_cpp,
        include_anchor,
        include_anchor + '#include "wsi_native_surface.h"\n\n#include <algorithm>\n#include <cstring>\n',
    )
    # SDL2's Android CMake target is named libSDL2.so; the desktop soname
    # libSDL2-2.0.so.0 is not the packaged Android library name.
    replace_once(
        platform_cpp,
        '''#elif defined(__APPLE__)
        "libSDL2-2.0.0.dylib"
#else
        "libSDL2-2.0.so.0"
#endif
''',
        '''#elif defined(__ANDROID__)
        "libSDL2.so"
#elif defined(__APPLE__)
        "libSDL2-2.0.0.dylib"
#else
        "libSDL2-2.0.so.0"
#endif
''',
    )
    namespace_anchor = "namespace dxvk::wsi {\n\n"
    helper = r'''namespace dxvk::wsi {

#ifdef __ANDROID__
  namespace {
    // Vulkan-Headers shipped with some Android DXVK toolchains predate
    // VK_KHR_android_surface. Keep the ABI declaration local and exact.
    struct SnapVkAndroidSurfaceCreateInfoKHR {
      VkStructureType sType;
      const void* pNext;
      VkFlags flags;
      void* window;
    };
    using SnapPFN_vkCreateAndroidSurfaceKHR = VkResult (VKAPI_PTR *)(
      VkInstance,
      const SnapVkAndroidSurfaceCreateInfoKHR*,
      const VkAllocationCallbacks*,
      VkSurfaceKHR*);
    constexpr VkStructureType SnapVkStructureTypeAndroidSurfaceCreateInfoKHR =
      static_cast<VkStructureType>(1000008000);
  }

  VkResult createAndroidSurface(
      PFN_vkGetInstanceProcAddr loader,
      VkInstance instance,
      void* nativeWindow,
      VkSurfaceKHR* surface) {
    if (!loader || !instance || !nativeWindow || !surface)
      return VK_ERROR_INITIALIZATION_FAILED;

    auto create = reinterpret_cast<SnapPFN_vkCreateAndroidSurfaceKHR>(
      loader(instance, "vkCreateAndroidSurfaceKHR"));
    if (!create)
      return VK_ERROR_EXTENSION_NOT_PRESENT;

    SnapVkAndroidSurfaceCreateInfoKHR info = {
      SnapVkStructureTypeAndroidSurfaceCreateInfoKHR,
      nullptr,
      0,
      nativeWindow,
    };
    return create(instance, &info, nullptr, surface);
  }
#endif

'''
    if platform_cpp.read_text(encoding="utf-8").count(namespace_anchor) != 1:
        raise SystemExit("unexpected SDL2 WSI namespace anchor")
    text = platform_cpp.read_text(encoding="utf-8").replace(namespace_anchor, helper, 1)
    ext_anchor = """    return extensionNames;
"""
    ext_patch = """#ifdef __ANDROID__
    // SDL2's extension list may omit the Android surface extension when
    // Vulkan was loaded through a custom loader. DXVK validates availability
    // before creating the instance, so unsupported devices still fail cleanly.
    bool hasAndroidSurfaceExtension = false;
    for (const char* extensionName : extensionNames) {
      if (extensionName && std::strcmp(extensionName, "VK_KHR_android_surface") == 0) {
        hasAndroidSurfaceExtension = true;
        break;
      }
    }
    if (!hasAndroidSurfaceExtension)
      extensionNames.push_back("VK_KHR_android_surface");
#endif

    return extensionNames;
"""
    if text.count(ext_anchor) != 1:
        raise SystemExit("unexpected SDL2 instance-extension return anchor")
    platform_cpp.write_text(text.replace(ext_anchor, ext_patch, 1), encoding="utf-8")

    swapchain = root / "src/d3d9/d3d9_swapchain.cpp"
    replace_once(
        swapchain,
        '#include "d3d9_window.h"\n',
        '#include "d3d9_window.h"\n#include "../wsi/sdl2/wsi_native_surface.h"\n#include "snapframe_dxvk262_api.h"\n',
    )
    replace_once(
        swapchain,
        """        // Blit back buffer onto Vulkan swap chain
        auto contextObjects = ctx->beginExternalRendering();
""",
        """        // Begin external rendering exactly once. SnapFrame records into
        // these same DXVK 2.6.2 command-list/descriptor-pool objects before
        // the WSI blit consumes the source. It must not submit or flush.
        auto contextObjects = ctx->beginExternalRendering();

        // Optional host: absent symbol leaves the normal DXVK path unchanged.
        if (snapframe_dxvk262_record_source) {
          const auto extent = cSrcView->image()->info().extent;
          const SnapFrameDxvk262FrameInfo frameInfo = {
            sizeof(SnapFrameDxvk262FrameInfo),
            SNAPFRAME_DXVK262_ABI_VERSION,
            cFrameId,
            extent.width,
            extent.height,
            cSrcView->image()->info().format,
            static_cast<void*>(cPresenter.ptr()),
            0u
          };
          (void)snapframe_dxvk262_record_source(
            static_cast<void*>(ctx),
            static_cast<void*>(&contextObjects),
            static_cast<void*>(cDevice.ptr()),
            static_cast<const void*>(&cSrcView),
            &frameInfo);
        }
""",
    )
    old_callback = """  Rc<Presenter> presenter = new Presenter(m_device, Signal, presenterDesc, [
      cDevice = m_device,
      cWindow = Window
    ] (VkSurfaceKHR* surface) {
      auto vki = cDevice->adapter()->vki();

      return wsi::createSurface(cWindow,
        vki->getLoaderProc(),
        vki->instance(),
        surface);
    });
"""
    new_callback = """#ifdef __ANDROID__
    // Capture the exact registered ANativeWindow generation before creating
    // the Presenter. The lease is copied into PresenterSurfaceProc and remains
    // alive until the callback is destroyed, after Presenter resources retire.
    wsi::NativeSurfaceLease nativeWindow;
    const bool hasNativeWindow = wsi::nativeSurfaceLease(Window, &nativeWindow);
#endif

    Rc<Presenter> presenter = new Presenter(m_device, Signal, presenterDesc, [
      cDevice = m_device,
      cWindow = Window
#ifdef __ANDROID__
      , cNativeWindow = nativeWindow,
      cHasNativeWindow = hasNativeWindow
#endif
    ] (VkSurfaceKHR* surface) {
      auto vki = cDevice->adapter()->vki();

#ifdef __ANDROID__
      if (cHasNativeWindow)
        return wsi::createAndroidSurface(
          vki->getLoaderProc(), vki->instance(), cNativeWindow.get(), surface);
#endif

      return wsi::createSurface(cWindow,
        vki->getLoaderProc(),
        vki->instance(),
        surface);
    });
"""
    replace_once(swapchain, old_callback, new_callback)

    # Retire per-Presenter SnapFrame history before DXVK invalidates and erases
    # the owning Presenter. This prevents stale history surviving pointer reuse.
    replace_once(
        swapchain,
        """    if (entry != m_presenters.end()) {
      if (entry->second.presenter) {
        entry->second.presenter->destroyResources();
        entry->second.presenter = nullptr;
""",
        """    if (entry != m_presenters.end()) {
      if (entry->second.presenter) {
        if (snapframe_dxvk262_release_window)
          snapframe_dxvk262_release_window(static_cast<void*>(entry->second.presenter.ptr()));
        entry->second.presenter->destroyResources();
        entry->second.presenter = nullptr;
""",
    )

    # Guard the lifetime assumption against upstream Presenter refactors:
    # the callback member must remain alive through surface destruction.
    presenter_cpp = root / "src/dxvk/dxvk_presenter.cpp"
    presenter_text = presenter_cpp.read_text(encoding="utf-8")
    destructor_anchor = """  Presenter::~Presenter() {
    destroySwapchain();
    destroySurface();
"""
    if presenter_text.count(destructor_anchor) != 1:
        raise SystemExit("upstream Presenter destruction order changed; re-review native-window lease lifetime")
    presenter_header = (root / "src/dxvk/dxvk_presenter.h").read_text(encoding="utf-8")
    if "PresenterSurfaceProc        m_surfaceProc;" not in presenter_header:
        raise SystemExit("Presenter no longer owns PresenterSurfaceProc; re-review lease lifetime")

    # SnapFrame's generated-present scheduler is not enabled by this Android
    # port patch. Pin the exact single-transaction assumptions so an upstream
    # Presenter refactor cannot silently invalidate the reviewed design.
    acquire_cpp = """    // Don't acquire more than one image at a time
    VkResult status = VK_SUCCESS;

    m_surfaceCond.wait(lock, [this, &status] {
      status = m_device->getDeviceStatus();
      return !m_presentPending || status < 0;
    });"""
    if presenter_text.count(acquire_cpp) != 1:
        raise SystemExit("Presenter acquire serialization changed; re-review generated-present scheduling")

    present_cpp = """    VkResult status = m_vkd->vkQueuePresentKHR(
      m_device->queues().graphics.queueHandle, &info);"""
    if presenter_text.count(present_cpp) != 1:
        raise SystemExit("Presenter present queue path changed; re-review generated-present scheduling")

    if "m_acquireStatus = m_vkd->vkAcquireNextImageKHR" not in presenter_text:
        raise SystemExit("Presenter no longer pre-acquires after present; re-review slot ownership")
    if "m_frameIndex %= m_semaphores.size();" not in presenter_text:
        raise SystemExit("Presenter semaphore ring changed; re-review generated-present scheduling")
    if "waitForSwapchainFence(nextSync);" not in presenter_text:
        raise SystemExit("Presenter next-slot fence reuse changed; re-review generated-present scheduling")

    # The pinned presentImage implementation performs a potentially blocking
    # next-slot fence wait and an infinite-timeout acquire on the submit worker.
    # A generated present must not be inserted into this worker until that
    # blocking WSI path is replaced by independently owned non-blocking slots.
    present_begin = presenter_text.find("VkResult Presenter::presentImage(")
    present_end = presenter_text.find("void Presenter::signalFrame(", present_begin)
    if present_begin < 0 or present_end <= present_begin:
        raise SystemExit("Presenter::presentImage boundaries changed; re-review WSI blocking behavior")
    present_body = presenter_text[present_begin:present_end]
    for blocking_anchor in (
        "waitForSwapchainFence(nextSync);",
        "std::numeric_limits<uint64_t>::max(),",
        "vkAcquireNextImageKHR(",
    ):
        if blocking_anchor not in present_body:
            raise SystemExit(
                "Presenter blocking pre-acquire behavior changed; re-review generated-present scheduling"
            )

    if "m_presentPending = false;" not in presenter_text:
        raise SystemExit("Presenter pending-state retirement changed; re-review generated-present scheduling")

    # WSI acquire/present semaphores are attached to the command list:
    # acquire is waited before graphics work and present is signalled after it.
    # Any transaction refactor must preserve this GPU-side dependency chain.
    cmdlist_header = (dxvk_dir / "dxvk_cmdlist.h").read_text(encoding="utf-8")
    cmdlist_cpp = (dxvk_dir / "dxvk_cmdlist.cpp").read_text(encoding="utf-8")
    context_header = (dxvk_dir / "dxvk_context.h").read_text(encoding="utf-8")
    presenter_header = (dxvk_dir / "dxvk_presenter.h").read_text(encoding="utf-8")
    presenter_sync = (dxvk_dir / "dxvk_presenter.h").read_text(encoding="utf-8")
    wsi_gpu_contract = (
        ("context hands WSI sync to command list",
         "void synchronizeWsi(PresenterSync sync)" in context_header
         and "m_cmd->setWsiSemaphores(sync);" in context_header),
        ("command list owns WSI semaphore pair",
         "PresenterSync             m_wsiSemaphores = { };" in cmdlist_header
         and "void setWsiSemaphores(const PresenterSync& wsiSemaphores)" in cmdlist_header),
        ("graphics waits acquire semaphore",
         "m_commandSubmission.waitSemaphore(m_wsiSemaphores.acquire," in cmdlist_cpp),
        ("graphics signals present semaphore",
         "m_commandSubmission.signalSemaphore(m_wsiSemaphores.present," in cmdlist_cpp),
        ("presenter sync pair contains binary semaphores",
         "VkSemaphore acquire = VK_NULL_HANDLE;" in presenter_sync
         and "VkSemaphore present = VK_NULL_HANDLE;" in presenter_sync),
        ("presenter keeps mutable ring state",
         "std::vector<PresenterSync>  m_semaphores;" in presenter_header
         and "uint32_t                    m_imageIndex = 0;" in presenter_header
         and "uint32_t                    m_frameIndex = 0;" in presenter_header),
    )
    for description, valid in wsi_gpu_contract:
        if not valid:
            raise SystemExit(
                f"DXVK WSI command-list contract changed ({description}); "
                "re-review transaction ownership and semaphore retirement"
            )

    # Fail closed unless the exact source-acquire token reaches the
    # queued present entry and Presenter consumes its captured image index.
    queue_cpp = (dxvk_dir / "dxvk_queue.cpp").read_text(encoding="utf-8")
    presenter_header_text = presenter_h.read_text(encoding="utf-8")
    presenter_source_text = presenter_cpp.read_text(encoding="utf-8")
    queue_header_text = queue_h.read_text(encoding="utf-8")
    device_header_text = device_h.read_text(encoding="utf-8")
    device_source_text = device_cpp_path.read_text(encoding="utf-8")
    frontend_text = swapchain.read_text(encoding="utf-8")
    transaction_contract = (
        ("PresenterSync carries transaction identity",
         "uint64_t transactionId = 0u;" in presenter_header_text
         and "uint64_t swapchainGeneration = 0u;" in presenter_header_text
         and "uint32_t imageIndex = 0u;" in presenter_header_text
         and "uint32_t semaphoreIndex = 0u;" in presenter_header_text),
        ("acquire stamps active token",
         "sync.transactionId = m_nextTransactionId++;" in presenter_source_text
         and "m_activeTransactionId = sync.transactionId;" in presenter_source_text
         and "m_swapchainGeneration++;" in presenter_source_text),
        ("present retires active token",
         "m_activeTransactionId = 0u;" in presenter_source_text
         and "m_presentPending = false;" in presenter_source_text
         and presenter_source_text.find("m_activeTransactionId = 0u;", presenter_source_text.find("VkResult Presenter::presentImage(")) < presenter_source_text.find("m_presentPending = false;", presenter_source_text.find("VkResult Presenter::presentImage("))),
        ("present validates active token",
         "transaction.transactionId != m_activeTransactionId" in presenter_source_text
         and "transaction.swapchainGeneration != m_swapchainGeneration" in presenter_source_text),
        ("present uses captured image index",
         "info.pImageIndices      = &presentImageIndex;" in presenter_source_text
         and "uint32_t presentImageIndex = transaction.imageIndex;" in presenter_source_text),
        ("present queue carries transaction",
         "PresenterSync       transaction;" in queue_header_text
         and "presentInfo.transaction = transaction;" in device_source_text
         and "entry.present.transaction, entry.present.frameId" in queue_cpp),
        ("frontend forwards exact acquire token",
         "cDevice->presentImage(cPresenter, cSync, cLatency, cFrameId, nullptr);" in frontend_text
         and "const PresenterSync&            transaction" in device_header_text),
    )
    for description, valid in transaction_contract:
        if not valid:
            raise SystemExit(
                f"DXVK immutable WSI transaction contract changed ({description}); "
                "do not enable generated present"
            )

    # Verify the actual submission queue implementation, not a guessed
    # dxvk_submission_queue.* path. In this pinned revision it is dxvk_queue.*.
    # A generated-present scheduler must preserve the submit-thread ordering,
    # finish-thread retirement, and queue mutex ownership shown below.
    queue_header = (dxvk_dir / "dxvk_queue.h").read_text(encoding="utf-8")
    queue_contract = (
        ("queue owns submit and finish workers",
         "dxvk::thread                m_submitThread;" in queue_header
         and "dxvk::thread                m_finishThread;" in queue_header),
        ("submit queue is FIFO",
         "std::queue<DxvkSubmitEntry> m_submitQueue;" in queue_header),
        ("present enqueued under queue mutex",
         "std::unique_lock<dxvk::mutex> lock(m_mutex);" in queue_cpp
         and "m_submitQueue.push(std::move(entry));" in queue_cpp),
        ("submit worker serializes Vulkan queue access",
         "std::lock_guard<dxvk::mutex> lock(m_mutexQueue);" in queue_cpp),
        ("submit worker submits command lists",
         "entry.submit.cmdList->submit(" in queue_cpp),
        ("present operation runs on submit worker",
         "entry.present.presenter->presentImage(" in queue_cpp),
        ("present completion is retired by finish worker",
         "entry.present.presenter->signalFrame(" in queue_cpp),
        ("finish queue owns command completion",
         "m_finishQueue.push(std::move(entry));" in queue_cpp
         and "m_finishQueue.pop();" in queue_cpp),
    )
    for description, valid in queue_contract:
        if not valid:
            raise SystemExit(
                f"DXVK submission queue contract changed ({description}); "
                "re-review asynchronous generated-present integration"
            )

    # The frontend currently enqueues exactly one command list followed by
    # one present entry from its CS closure. Keep that ordering explicit.
    swapchain_text = swapchain.read_text(encoding="utf-8")
    present_sequence = (
        "ctx->synchronizeWsi(cSync);",
        "ctx->flushCommandList(nullptr, nullptr);",
        "cDevice->presentImage(cPresenter, cSync, cLatency, cFrameId, nullptr);",
    )
    positions = [swapchain_text.find(item) for item in present_sequence]
    if any(position < 0 for position in positions) or positions != sorted(positions):
        raise SystemExit(
            "D3D9 source submit/present ordering changed; re-review SnapFrame transaction insertion"
        )

    # DXVK's D3D9 version script hides symbols not listed in global:.
    # Export the Android surface registry and the matching SnapFrame callbacks
    # so main_262/JNI can resolve them from this exact versioned DSO.
    symbol_map = root / "src/d3d9/d3d9.sym"
    symbol_text = symbol_map.read_text(encoding="utf-8")
    if symbol_text.count("  global:") != 1 or symbol_text.count("  local:") != 1:
        raise SystemExit("unexpected DXVK D3D9 symbol map structure")
    required_exports = (
        "dxvk_native_surface",
        "snapframe_dxvk262_record_source",
        "snapframe_dxvk262_release_window",
        "snapframe_dxvk262_release_device",
    )
    missing_exports = [
        name for name in required_exports
        if f"    {name};" not in symbol_text
    ]
    if missing_exports:
        export_lines = "".join(f"    {name};\\n" for name in missing_exports)
        symbol_text = symbol_text.replace(
            "  local:", export_lines + "\\n  local:", 1
        )
        symbol_map.write_text(symbol_text, encoding="utf-8")
    print("DXVK_NATIVE_262_SNAPFRAME_DYNAMIC_EXPORTS_OK")

    print("DXVK_NATIVE_262_ANDROID_PRESENTER_WSI_PATCH_OK")
    print("Verified: source acquire token is carried through command list and queued present.")
    print("Verified: Presenter consumes the captured image/semaphore indices, not a reconstructed token.")
    print("Still gated: multiple outstanding transactions and non-blocking generated acquire.")
    print("Verified: DxvkSubmissionQueue FIFO submit worker + finish-thread retirement.")
    print("Verified: D3D9 WSI semaphore handoff -> command-list enqueue -> present enqueue order.")
    print("SnapFrame generated present: DISABLED pending a DXVK-owned asynchronous transaction scheduler.")
    print("Changes: Presenter-owned ANativeWindow lease; Android surface create; instance extension.")
    print("Build status: NOT_BUILT")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
