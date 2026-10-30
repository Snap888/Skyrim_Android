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
    this->waitForIdle();
    m_objects.pipelineManager().stopWorkerThreads();
  }

  bool DxvkDevice::isUnifiedMemoryArchitecture() const {
    return m_adapter->isUnifiedMemoryArchitecture();
  }

  VkPipelineStageFlags DxvkDevice::getShaderPipelineStages() const {
    VkPipelineStageFlags result = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                                | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT
                                | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    if (m_features.core.features.geometryShader)
      result |= VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT;
    if (m_features.core.features.tessellationShader) {
      result |= VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT
             |  VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT;
    }
    return result;
  }

  DxvkDeviceOptions DxvkDevice::options() const {
    DxvkDeviceOptions options;
    options.maxNumDynamicUniformBuffers = m_properties.core.properties.limits.maxDescriptorSetUniformBuffersDynamic;
    options.maxNumDynamicStorageBuffers = m_properties.core.properties.limits.maxDescriptorSetStorageBuffersDynamic;
    return options;
  }

  Rc<DxvkCommandList> DxvkDevice::createCommandList() {
    Rc<DxvkCommandList> cmdList = m_recycledCommandLists.retrieveObject();
    if (cmdList == nullptr)
      cmdList = new DxvkCommandList(this);
    return cmdList;
  }

  Rc<DxvkDescriptorPool> DxvkDevice::createDescriptorPool() {
    Rc<DxvkDescriptorPool> pool = m_recycledDescriptorPools.retrieveObject();
    if (pool == nullptr)
      pool = new DxvkDescriptorPool(m_vkd);
    return pool;
  }

  Rc<DxvkContext> DxvkDevice::createContext() {
    return new DxvkContext(this);
  }

  Rc<DxvkGpuEvent> DxvkDevice::createGpuEvent() {
    return new DxvkGpuEvent(m_vkd);
  }

  Rc<DxvkGpuQuery> DxvkDevice::createGpuQuery(
          VkQueryType           type,
          VkQueryControlFlags   flags,
          uint32_t              index) {
    return new DxvkGpuQuery(m_vkd, type, flags, index);
  }

  Rc<DxvkFramebuffer> DxvkDevice::createFramebuffer(
    const DxvkRenderTargets& renderTargets) {
    const DxvkFramebufferSize defaultSize = {
      m_properties.core.properties.limits.maxFramebufferWidth,
      m_properties.core.properties.limits.maxFramebufferHeight,
      m_properties.core.properties.limits.maxFramebufferLayers };
    auto renderPassFormat = DxvkFramebuffer::getRenderPassFormat(renderTargets);
    auto renderPassObject = m_objects.renderPassPool().getRenderPass(renderPassFormat);
    return new DxvkFramebuffer(m_vkd, renderPassObject, renderTargets, defaultSize);
  }

  Rc<DxvkBuffer> DxvkDevice::createBuffer(
    const DxvkBufferCreateInfo& createInfo,
          VkMemoryPropertyFlags memoryType) {
    return new DxvkBuffer(this, createInfo, m_objects.memoryManager(), memoryType);
  }

  Rc<DxvkBufferView> DxvkDevice::createBufferView(
    const Rc<DxvkBuffer>&           buffer,
    const DxvkBufferViewCreateInfo& createInfo) {
    return new DxvkBufferView(m_vkd, buffer, createInfo);
  }

  Rc<DxvkImage> DxvkDevice::createImage(
    const DxvkImageCreateInfo&  createInfo,
          VkMemoryPropertyFlags memoryType) {
    return new DxvkImage(m_vkd, createInfo, m_objects.memoryManager(), memoryType);
  }

  Rc<DxvkImage> DxvkDevice::createImageFromVkImage(
    const DxvkImageCreateInfo&  createInfo,
          VkImage               image) {
    return new DxvkImage(m_vkd, createInfo, image);
  }

  Rc<DxvkImageView> DxvkDevice::createImageView(
    const Rc<DxvkImage>&            image,
    const DxvkImageViewCreateInfo&  createInfo) {
    return new DxvkImageView(m_vkd, image, createInfo);
  }

  Rc<DxvkSampler> DxvkDevice::createSampler(
    const DxvkSamplerCreateInfo& createInfo) {
    return new DxvkSampler(this, createInfo);
  }

  Rc<DxvkShader> DxvkDevice::createShader(
    const DxvkShaderCreateInfo& createInfo) {
    return new DxvkShader(createInfo);
  }

  DxvkStatCounters DxvkDevice::getStatCounters() {
    DxvkMemoryAllocator& memAlloc = m_objects.memoryManager();
    DxvkStatCounters result;
    {
      std::lock_guard<dxvk::mutex> lock(m_statLock);
      result = m_statCounters;
    }
    result.setCtr(DxvkStatCounter::MemoryAllocated,
      memAlloc.getMemoryStats().memoryAllocated);
    result.setCtr(DxvkStatCounter::MemoryUsed,
      memAlloc.getMemoryStats().memoryUsed);
    return result;
  }

  DxvkMemoryStats DxvkDevice::getMemoryStats(uint32_t heap) {
    return m_objects.memoryManager().getMemoryStats(heap);
  }

  uint32_t DxvkDevice::getCurrentFrameId() const {
    return m_frameId.load();
  }

  void DxvkDevice::initResources() {
    m_objects.dummyResources().clearResources(this);
  }

  void DxvkDevice::registerShader(const Rc<DxvkShader>& shader) {
    m_objects.pipelineManager().registerShader(shader);
  }

  void DxvkDevice::presentImage(
    const Rc<Presenter>&            presenter,
          DxvkSubmitStatus*         status,
          VkSemaphore               semaphore) {
    status->result.store(VK_NOT_READY);
    m_submissionQueue.present(presenter, semaphore, status);
  }

  void DxvkDevice::submitCommandList(
    const Rc<DxvkCommandList>&      commandList,
          VkSemaphore               waitSync,
          VkSemaphore               wakeSync) {
    m_submissionQueue.submit(DxvkSubmitInfo {
      commandList, waitSync, wakeSync });
    std::lock_guard<dxvk::mutex> statLock(m_statLock);
    m_statCounters.merge(commandList->statCounters());
    m_statCounters.addCtr(DxvkStatCounter::QueueSubmitCount, 1);
  }

  VkResult DxvkDevice::waitForSubmission(DxvkSubmitStatus* status) {
    VkResult result = status->result.load();
    if (result == VK_NOT_READY) {
      m_submissionQueue.synchronizeSubmission(status);
      result = status->result.load();
    }
    return result;
  }

  void DxvkDevice::waitForIdle() {
    this->lockSubmission();
    if (m_vkd->vkDeviceWaitIdle(m_vkd->device()) != VK_SUCCESS)
      Logger::err("DxvkDevice: waitForIdle: Operation failed");
    this->unlockSubmission();
  }

  DxvkDevicePerfHints DxvkDevice::getPerfHints() {
    DxvkDevicePerfHints hints;
    hints.preferFbDepthStencilCopy = m_extensions.extShaderStencilExport
      && (m_adapter->matchesDriver(DxvkGpuVendor::Amd, VK_DRIVER_ID_MESA_RADV_KHR, 0, 0)
       || m_adapter->matchesDriver(DxvkGpuVendor::Amd, VK_DRIVER_ID_AMD_OPEN_SOURCE_KHR, 0, 0)
       || m_adapter->matchesDriver(DxvkGpuVendor::Amd, VK_DRIVER_ID_AMD_PROPRIETARY_KHR, 0, 0));
    hints.preferFbResolve = m_extensions.amdShaderFragmentMask
      && (m_adapter->matchesDriver(DxvkGpuVendor::Amd, VK_DRIVER_ID_AMD_OPEN_SOURCE_KHR, 0, 0)
       || m_adapter->matchesDriver(DxvkGpuVendor::Amd, VK_DRIVER_ID_AMD_PROPRIETARY_KHR, 0, 0));
    return hints;
  }

  void DxvkDevice::recycleCommandList(const Rc<DxvkCommandList>& cmdList) {
    m_recycledCommandLists.returnObject(cmdList);
  }

  void DxvkDevice::recycleDescriptorPool(const Rc<DxvkDescriptorPool>& pool) {
    m_recycledDescriptorPools.returnObject(pool);
  }

  DxvkDeviceQueue DxvkDevice::getQueue(
          uint32_t                family,
          uint32_t                index) const {
    VkQueue queue = VK_NULL_HANDLE;
    m_vkd->vkGetDeviceQueue(m_vkd->device(), family, index, &queue);
    return DxvkDeviceQueue { queue, family, index };
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
