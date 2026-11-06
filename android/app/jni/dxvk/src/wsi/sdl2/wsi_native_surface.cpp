#include "wsi_native_surface.h"

#include <mutex>
#include <utility>

#ifdef __ANDROID__
#include <android/native_window.h>
#endif

namespace {
  std::mutex g_lock;
  void*      g_hwnd;
  dxvk::wsi::NativeSurface g_surface = { nullptr, 0, 0 };

  void acquireWindow(void* window) {
#ifdef __ANDROID__
    if (window)
      ANativeWindow_acquire(static_cast<ANativeWindow*>(window));
#else
    (void)window;
#endif
  }

  void releaseWindow(void* window) {
#ifdef __ANDROID__
    if (window)
      ANativeWindow_release(static_cast<ANativeWindow*>(window));
#else
    (void)window;
#endif
  }
}

namespace dxvk::wsi {
  bool nativeSurface(HWND hWindow, NativeSurface* pOut) {
    std::lock_guard<std::mutex> lock(g_lock);
    if (!g_hwnd || (void*)hWindow != g_hwnd)
      return false;
    if (pOut) {
      *pOut = g_surface;
      // Pin the native window across the caller's surface-create operation.
      acquireWindow(pOut->window);
    }
    return true;
  }

  void releaseNativeSurface(NativeSurface* pSurface) {
    if (!pSurface)
      return;
    releaseWindow(pSurface->window);
    pSurface->window = nullptr;
    pSurface->width = 0;
    pSurface->height = 0;
  }

  bool nativeSurfaceLease(HWND hWindow, NativeSurfaceLease* pOut) {
    if (!pOut)
      return false;

    void* window = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    {
      std::lock_guard<std::mutex> lock(g_lock);
      if (!g_hwnd || (void*)hWindow != g_hwnd || !g_surface.window)
        return false;

      window = g_surface.window;
      width = g_surface.width;
      height = g_surface.height;
      // Acquire while the registry lock still protects this generation.
      acquireWindow(window);
    }

    // Build/replace the shared lease outside the registry lock. Its deleter
    // may invoke native-window callbacks and must never run under g_lock.
    std::shared_ptr<void> owned;
    try {
      owned = std::shared_ptr<void>(window, [](void* value) {
        releaseWindow(value);
      });
    } catch (...) {
      releaseWindow(window);
      throw;
    }

    pOut->window = std::move(owned);
    pOut->width = width;
    pOut->height = height;
    return true;
  }
}

extern "C" __attribute__((visibility("default")))
void dxvk_native_surface(void* hwnd, void* window, uint32_t width, uint32_t height) {
  // Acquire before publishing so replacement never exposes an unowned window.
  acquireWindow(window);

  void* previousWindow = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_lock);
    previousWindow = g_surface.window;
    g_hwnd = window ? hwnd : nullptr;
    g_surface = { window, width, height };
  }

  // Release outside the registry lock; ANativeWindow callbacks must not run
  // while the global WSI registry mutex is held.
  releaseWindow(previousWindow);
}
