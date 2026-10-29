#include "dxvk_device.h"
#include "dxvk_instance.h"

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
      // Mobile drivers are unreliable with scaled formats; the UINT path is exact everywhere.
      g_dxvkUbyte4AsUint = true;
#endif
      Logger::info(str::format("Port: USCALED vertex fetch ",
        (fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) ? "supported" : "NOT supported",
        g_dxvkUbyte4AsUint ? ", UBYTE4/SHORT fetched normalized + rescaled" : ""));
    }
    auto queueFamilies = m_adapter->findQueueFamilies();
    m_queues.graphics = getQueue(queueFamilies.graphics, 0);
    m_queues.transfer = getQueue(queueFamilies.transfer, 0);

    // LSFG: register VkDevice for SPIR-V validation / future compute FG
    {
      extern "C" void lsfg_register_vk_device(void*);
      lsfg_register_vk_device(reinterpret_cast<void*>(m_vkd->device()));
    }
  }
  
  
  DxvkDevice::~DxvkDevice() {
    // Wait for all pending Vulkan commands to be
    // executed before we destroy any resources.
    this->waitForIdle();

    // Stop workers explicitly in order to prevent
    // access to structures that are being destroyed.
    m_objects.pipelineManager().stopWorkerThreads();
  }


  bool DxvkDevice::isUnifiedMemoryArchitecture() const {
    return m_adapter->isUnifiedMemoryArchitecture();
  }


  VkPipelineStageFlags DxvkDevice::getShaderPipelineStages() const {
    VkPipelineStageFlags result = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
  
    result |= m_adapter->getShaderPipelineStages();

    return result;
  }
  
  
  DxvkAdapterMemoryInfo DxvkDevice::getMemoryStats() {
    return m_adapter->getMemoryHeapInfo();
  }


  void DxvkDevice::recycleCommandList(
    const Rc<DxvkCommandList>& cmdList) {
    m_objects.commandListPool().recycleCommandList(cmdList);
  }
  

  void DxvkDevice::recycleDescriptorPool(
    const Rc<DxvkDescriptorPool>& pool) {
    m_objects.descriptorPoolTracker().recycleDescriptorPool(pool);
  }
  

  DxvkDeviceQueue DxvkDevice::getQueue(
          uint32_t                family,
          uint32_t                index) const {
    VkQueue queue = VK_NULL_HANDLE;
    m_vkd->vkGetDeviceQueue(m_vkd->device(), family, index, &queue);
    return DxvkDeviceQueue { queue, family, index };
  }

  void DxvkDevice::initSubmissionQueue() {
    // no-op, queue created in ctor
  }

}
