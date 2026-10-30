#include "dxvk_device.h"
#include "dxvk_instance.h"

/* LSFG: context hook (device, physicalDevice, graphicsQueue, queueFamily) */
static void (*g_lsfg_vk_ctx_hook)(void*, void*, void*, uint32_t) = nullptr;
static void (*g_lsfg_vk_device_hook)(void*) = nullptr;
static void (*g_lsfg_present_image_hook)(void*, void*, uint32_t, uint32_t, uint32_t) = nullptr;
static void (*g_lsfg_step3g_copy_hook)(void*, void*, void*, uint32_t, uint32_t, uint32_t) = nullptr;
static void (*g_lsfg_inject_hook)(void*, void*, void*, uint32_t, uint32_t, uint32_t) = nullptr;
static int  (*g_lsfg_should_inject_hook)(void) = nullptr;

extern "C" void lsfg_set_vk_device_hook(void (*fn)(void*)) {
  g_lsfg_vk_device_hook = fn;
}
extern "C" void lsfg_set_vk_context_hook(void (*fn)(void*, void*, void*, uint32_t)) {
  g_lsfg_vk_ctx_hook = fn;
}
extern "C" void lsfg_set_present_image_hook(void (*fn)(void*, void*, uint32_t, uint32_t, uint32_t)) {
  g_lsfg_present_image_hook = fn;
}
extern "C" void* lsfg_get_present_image_hook(void) {
  return (void*)g_lsfg_present_image_hook;
}
extern "C" void lsfg_set_step3g_copy_hook(void (*fn)(void*, void*, void*, uint32_t, uint32_t, uint32_t)) {
  g_lsfg_step3g_copy_hook = fn;
}
extern "C" void lsfg_set_step3hj_inject_hook(void (*fn)(void*, void*, void*, uint32_t, uint32_t, uint32_t)) {
  g_lsfg_inject_hook = fn;
}
extern "C" void lsfg_set_should_inject_hook(int (*fn)(void)) {
  g_lsfg_should_inject_hook = fn;
}

namespace dxvk {

  bool g_dxvkDepthClamp = true;
  bool g_dxvkUbyte4AsUint = false;

  DxvkDevice::DxvkDevice(
    const Rc<DxvkInstance>&         instance,
    const Rc<DxvkAdapter>&          adapter,
    const Rc<vk::DeviceFn>&         vkd,
    const DxvkDeviceExtensions&     extensions,
    const DxvkDeviceFeatures&       features)
  : m_options           (instance->options()),
    m_instance          (instance),
    m_adapter           (adapter),
    m_vkd               (vkd),
    m_extensions        (extensions),
    m_features          (features),
    m_properties        (adapter->devicePropertiesExt()),
    m_perfHints         (getPerfHints()),
    m_objects           (this),
    m_submissionQueue   (this) {
    g_dxvkDepthClamp = features.core.features.depthClamp;
    {
      VkFormatProperties fp = adapter->formatProperties(VK_FORMAT_R8G8B8A8_USCALED);
      const char* force = std::getenv("LB_NOSCALED");
      g_dxvkUbyte4AsUint = !(fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)
                        || (force && *force == '1');
#ifdef __ANDROID__
      g_dxvkUbyte4AsUint = true;
#endif
      Logger::info(str::format("Port: USCALED vertex fetch ",
        (fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) ? "supported" : "NOT supported",
        g_dxvkUbyte4AsUint ? ", UBYTE4/SHORT fetched normalized + rescaled" : ""));
    }
    auto queueFamilies = m_adapter->findQueueFamilies();
    m_queues.graphics = getQueue(queueFamilies.graphics, 0);
    m_queues.transfer = getQueue(queueFamilies.transfer, 0);

    if (g_lsfg_vk_ctx_hook)
      g_lsfg_vk_ctx_hook(
        reinterpret_cast<void*>(m_vkd->device()),
        reinterpret_cast<void*>(m_adapter->handle()),
        reinterpret_cast<void*>(m_queues.graphics.queueHandle),
        m_queues.graphics.queueFamily);
    else if (g_lsfg_vk_device_hook)
      g_lsfg_vk_device_hook(reinterpret_cast<void*>(m_vkd->device()));
  }

  DxvkDevice::~DxvkDevice() {
    // wait for GPU idle before destroying device-owned resources
    this->waitForIdle();
  }

  bool DxvkDevice::isUnifiedMemoryArchitecture() const {
    auto memoryTypes = m_adapter->memoryProperties().memoryTypes;
    for (uint32_t i = 0; i < m_adapter->memoryProperties().memoryTypeCount; i++) {
      uint32_t flags = memoryTypes[i].propertyFlags;
      if ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
       && (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
        return true;
    }
    return false;
  }

  DxvkDevicePerfHints DxvkDevice::getPerfHints() {
    DxvkDevicePerfHints hints;
    hints.preferFbDepthStencilCopy = false;
    hints.preferFbResolve          = false;
    return hints;
  }

  DxvkDeviceQueue DxvkDevice::getQueue(uint32_t family, uint32_t index) {
    DxvkDeviceQueue queue;
    queue.queueFamily = family;
    queue.queueIndex  = index;
    m_vkd->vkGetDeviceQueue(m_vkd->device(), queue.queueFamily, queue.queueIndex, &queue.queueHandle);
    return queue;
  }

  void DxvkDevice::waitForIdle() {
    this->lockSubmission();
    m_vkd->vkDeviceWaitIdle(m_vkd->device());
    this->unlockSubmission();
  }

}

/* C ABI trampolines — public symbols for PresentImage / swapchain */
extern "C" int lsfg_step3hj_should_inject(void) {
  return g_lsfg_should_inject_hook ? g_lsfg_should_inject_hook() : 0;
}

extern "C" void lsfg_step3hj_insert_inject(
    void* ctx, void* wsi_view, void* device,
    uint32_t w, uint32_t h, uint32_t vkfmt) {
  if (g_lsfg_inject_hook)
    g_lsfg_inject_hook(ctx, wsi_view, device, w, h, vkfmt);
}

extern "C" void lsfg_step3g_insert_copy(
    void* ctx, void* src_view, void* device,
    uint32_t w, uint32_t h, uint32_t vkfmt) {
  if (g_lsfg_step3g_copy_hook)
    g_lsfg_step3g_copy_hook(ctx, src_view, device, w, h, vkfmt);
}
