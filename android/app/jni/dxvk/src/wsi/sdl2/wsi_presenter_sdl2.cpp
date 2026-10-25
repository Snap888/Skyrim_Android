#include "../wsi_presenter.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>

#include <wsi/native_wsi.h>

#include "wsi_native_surface.h"

#ifdef __ANDROID__
// VK_KHR_android_surface by hand: the NDK's vulkan_android.h is newer than
// dxvk's Vulkan headers.
namespace {
  struct AndroidSurfaceCreateInfo {
    VkStructureType sType;
    const void*     pNext;
    VkFlags         flags;
    void*           window;
  };
  typedef VkResult (VKAPI_PTR *PFN_CreateAndroidSurface)(
    VkInstance, const AndroidSurfaceCreateInfo*, const VkAllocationCallbacks*, VkSurfaceKHR*);
}
#endif

namespace dxvk::wsi {

  VkResult createSurface(
          HWND                hWindow,
    const Rc<vk::InstanceFn>& vki,
          VkSurfaceKHR*       pSurface) {
#ifdef __ANDROID__
    NativeSurface ns;
    if (nativeSurface(hWindow, &ns)) {
      auto create = reinterpret_cast<PFN_CreateAndroidSurface>(
        vki->sym("vkCreateAndroidSurfaceKHR"));
      if (!create)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
      AndroidSurfaceCreateInfo info = { VkStructureType(1000008000), nullptr, 0, ns.window };
      return create(vki->instance(), &info, nullptr, pSurface);
    }
#endif
    SDL_Window* window = fromHwnd(hWindow);

    return SDL_Vulkan_CreateSurface(window, vki->instance(), pSurface)
         ? VK_SUCCESS
         : VK_ERROR_OUT_OF_HOST_MEMORY;
  }

}