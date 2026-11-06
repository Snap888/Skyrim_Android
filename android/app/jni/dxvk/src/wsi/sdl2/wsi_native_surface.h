#pragma once

// Android's game-provided ANativeWindow is registered behind a synthetic HWND.
// The registry owns one native-window reference while the entry is active.
// A presenter-generation lease can outlive registry replacement and must be
// retained until that presenter's VkSurfaceKHR and swapchain are destroyed.
#include <windows.h>
#include <cstdint>
#include <memory>

namespace dxvk::wsi {
  struct NativeSurface { void* window; uint32_t width, height; };

  // Copyable ownership token. Capturing this by value in PresenterSurfaceProc
  // pins the exact ANativeWindow generation for the presenter's full lifetime.
  struct NativeSurfaceLease {
    std::shared_ptr<void> window;
    uint32_t width = 0;
    uint32_t height = 0;

    explicit operator bool() const { return bool(window); }
    void* get() const { return window.get(); }
  };

  bool nativeSurface(HWND hWindow, NativeSurface* pOut);
  void releaseNativeSurface(NativeSurface* pSurface);

  // Acquire an independent reference to the currently registered generation.
  // The returned lease is safe to copy into a presenter callback.
  bool nativeSurfaceLease(HWND hWindow, NativeSurfaceLease* pOut);
}

extern "C" void dxvk_native_surface(void* hwnd, void* window, uint32_t width, uint32_t height);
