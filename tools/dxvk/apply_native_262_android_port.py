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
    dxvk_dir = root / "src/dxvk"
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
            static_cast<void*>(&cSrcView),
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
    if "m_presentPending = false;" not in presenter_text:
        raise SystemExit("Presenter pending-state retirement changed; re-review generated-present scheduling")

    # Verify the actual submission queue implementation, not a guessed
    # dxvk_submission_queue.* path. In this pinned revision it is dxvk_queue.*.
    # A generated-present scheduler must preserve the submit-thread ordering,
    # finish-thread retirement, and queue mutex ownership shown below.
    queue_header = (dxvk_dir / "dxvk_queue.h").read_text(encoding="utf-8")
    queue_cpp = (dxvk_dir / "dxvk_queue.cpp").read_text(encoding="utf-8")
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
        "cDevice->presentImage(cPresenter, cLatency, cFrameId, nullptr);",
    )
    positions = [swapchain_text.find(item) for item in present_sequence]
    if any(position < 0 for position in positions) or positions != sorted(positions):
        raise SystemExit(
            "D3D9 source submit/present ordering changed; re-review SnapFrame transaction insertion"
        )

    print("DXVK_NATIVE_262_ANDROID_PRESENTER_WSI_PATCH_OK")
    print("Verified: one outstanding acquire/present transaction; pre-acquire advances semaphore ring.")
    print("Verified: DxvkSubmissionQueue FIFO submit worker + finish-thread retirement.")
    print("Verified: D3D9 WSI semaphore handoff -> command-list enqueue -> present enqueue order.")
    print("SnapFrame generated present: DISABLED pending a DXVK-owned asynchronous transaction scheduler.")
    print("Changes: Presenter-owned ANativeWindow lease; Android surface create; instance extension.")
    print("Build status: NOT_BUILT")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
