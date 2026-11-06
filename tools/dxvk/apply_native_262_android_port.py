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
        '#include "d3d9_window.h"\n#include "../wsi/sdl2/wsi_native_surface.h"\n',
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

    print("DXVK_NATIVE_262_ANDROID_PRESENTER_WSI_PATCH_OK")
    print("Changes: Presenter-owned ANativeWindow lease; Android surface create; instance extension.")
    print("Build status: NOT_BUILT")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
