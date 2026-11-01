#include "vulkan_loader.h"

#ifdef __ANDROID__
#include <adrenotools/driver.h>
#include <dlfcn.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include <android/log.h>
#endif

namespace dxvk::vk {

#ifdef __ANDROID__
  static PFN_vkGetInstanceProcAddr loadInstanceProcAddr() {
    const char* driverDir = std::getenv("SKYRIM_TURNIP_DRIVER_PATH");
    const char* driverName = std::getenv("SKYRIM_TURNIP_DRIVER_NAME");
    if (driverDir && driverName && *driverDir && *driverName) {
      Dl_info info{};
      if (dladdr(reinterpret_cast<void*>(&loadInstanceProcAddr), &info) && info.dli_fname) {
        std::string hookDir(info.dli_fname);
        const auto slash = hookDir.find_last_of('/');
        if (slash != std::string::npos) hookDir.resize(slash + 1);
        void* library = adrenotools_open_libvulkan(
          RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, nullptr, hookDir.c_str(),
          driverDir, driverName, nullptr, nullptr);
        if (library) {
          auto proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library, "vkGetInstanceProcAddr"));
          if (proc) {
            __android_log_print(ANDROID_LOG_INFO, "SkyrimVulkan",
              "AdrenoTools activated custom Vulkan driver %s from %s", driverName, driverDir);
            return proc;
          }
          __android_log_print(ANDROID_LOG_ERROR, "SkyrimVulkan",
            "AdrenoTools driver loaded but vkGetInstanceProcAddr is missing");
        } else {
          __android_log_print(ANDROID_LOG_ERROR, "SkyrimVulkan",
            "AdrenoTools failed to open custom driver %s; falling back to system Vulkan", driverName);
        }
      } else {
        __android_log_print(ANDROID_LOG_ERROR, "SkyrimVulkan",
          "Cannot resolve app nativeLibraryDir for AdrenoTools hooks");
      }
    }
    return vkGetInstanceProcAddr;
  }
  static PFN_vkGetInstanceProcAddr getInstanceProcAddr() {
    static const PFN_vkGetInstanceProcAddr proc = loadInstanceProcAddr();
    return proc;
  }
#else
  static PFN_vkGetInstanceProcAddr getInstanceProcAddr() { return vkGetInstanceProcAddr; }
#endif

  PFN_vkVoidFunction LibraryLoader::sym(const char* name) const {
    return dxvk::vk::getInstanceProcAddr()(nullptr, name);
  }
  
  
  InstanceLoader::InstanceLoader(bool owned, VkInstance instance)
  : m_instance(instance), m_owned(owned) { }
  
  
  PFN_vkVoidFunction InstanceLoader::sym(const char* name) const {
    return dxvk::vk::getInstanceProcAddr()(m_instance, name);
  }
  
  
  DeviceLoader::DeviceLoader(bool owned, VkInstance instance, VkDevice device)
  : m_getDeviceProcAddr(reinterpret_cast<PFN_vkGetDeviceProcAddr>(
      dxvk::vk::getInstanceProcAddr()(instance, "vkGetDeviceProcAddr"))),
    m_device(device), m_owned(owned) { }
  
  
  PFN_vkVoidFunction DeviceLoader::sym(const char* name) const {
    return m_getDeviceProcAddr(m_device, name);
  }
  
  
  LibraryFn::LibraryFn() { }
  LibraryFn::~LibraryFn() { }
  
  
  InstanceFn::InstanceFn(bool owned, VkInstance instance)
  : InstanceLoader(owned, instance) { }
  InstanceFn::~InstanceFn() {
    if (m_owned)
      this->vkDestroyInstance(m_instance, nullptr);
  }
  
  
  DeviceFn::DeviceFn(bool owned, VkInstance instance, VkDevice device)
  : DeviceLoader(owned, instance, device) { }
  DeviceFn::~DeviceFn() {
    if (m_owned)
      this->vkDestroyDevice(m_device, nullptr);
  }
  
}