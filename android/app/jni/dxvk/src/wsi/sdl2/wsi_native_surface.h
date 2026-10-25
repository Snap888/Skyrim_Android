#pragma once

// Port: a second Android display (the HUD screen) is not an SDL window. The
// game registers its ANativeWindow under a made-up HWND with
// dxvk_native_surface(); the SDL2 WSI functions treat that HWND as a plain
// Vulkan surface of the given size.
#include <windows.h>
#include <cstdint>

namespace dxvk::wsi {
  struct NativeSurface { void* window; uint32_t width, height; };
  bool nativeSurface(HWND hWindow, NativeSurface* pOut);
}

extern "C" void dxvk_native_surface(void* hwnd, void* window, uint32_t width, uint32_t height);
