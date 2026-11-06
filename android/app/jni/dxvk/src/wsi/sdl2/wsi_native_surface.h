#pragma once

// Android's game-provided ANativeWindow is registered behind a synthetic HWND.
// The registry owns one native-window reference while the entry is active.
// nativeSurface() returns a second, temporary reference which the caller must
// release with releaseNativeSurface() after the Vulkan surface-create call.
#include <windows.h>
#include <cstdint>

namespace dxvk::wsi {
  struct NativeSurface { void* window; uint32_t width, height; };
  bool nativeSurface(HWND hWindow, NativeSurface* pOut);
  void releaseNativeSurface(NativeSurface* pSurface);
}

extern "C" void dxvk_native_surface(void* hwnd, void* window, uint32_t width, uint32_t height);
