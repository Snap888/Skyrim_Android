#include "wsi_native_surface.h"

#include <mutex>

namespace {
  std::mutex g_lock;
  void*      g_hwnd;
  dxvk::wsi::NativeSurface g_surface;
}

namespace dxvk::wsi {
  bool nativeSurface(HWND hWindow, NativeSurface* pOut) {
    std::lock_guard<std::mutex> lock(g_lock);
    if (!g_hwnd || (void*)hWindow != g_hwnd)
      return false;
    if (pOut)
      *pOut = g_surface;
    return true;
  }
}

extern "C" __attribute__((visibility("default")))
void dxvk_native_surface(void* hwnd, void* window, uint32_t width, uint32_t height) {
  std::lock_guard<std::mutex> lock(g_lock);
  g_hwnd = window ? hwnd : nullptr;
  g_surface = { window, width, height };
}
