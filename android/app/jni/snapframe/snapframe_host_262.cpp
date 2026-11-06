/*
 * SnapFrame DXVK Native 2.6.2 host.
 *
 * This is intentionally a separate implementation from snapframe_host.cpp:
 * that file consumes private DXVK 1.9.2b APIs and must never be linked to the
 * 2.6.2 candidate. This first 2.6.2 stage records the luma preprocessing pass
 * through DxvkContext so DXVK owns barriers, descriptor allocation, pipeline
 * compilation, command submission and resource lifetime tracking.
 */
#include "snapframe_dxvk262_api.h"

#include <android/log.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "dxvk_context.h"
#include "dxvk_device.h"
#include "dxvk_image.h"
#include "dxvk_sampler.h"
#include "dxvk_shader.h"
#include "spirv_code_buffer.h"

#include "snapframe_shaders.h"

namespace dxvk {
namespace {

constexpr const char* kLogTag = "SnapFrame262";

struct DeviceState {
  DxvkDevice* device = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  VkFormat sourceFormat = VK_FORMAT_UNDEFINED;
  std::array<Rc<DxvkImage>, 2> lumaImages;
  std::array<Rc<DxvkImageView>, 2> lumaViews;
  Rc<DxvkSampler> sampler;
  Rc<DxvkShader> lumaShader;
  uint64_t recordedFrames = 0;
  uint32_t writeSlot = 0;
};

std::mutex g_stateMutex;
std::unordered_map<DxvkDevice*, DeviceState> g_deviceStates;

void logOnce(const char* message) {
  __android_log_print(ANDROID_LOG_INFO, kLogTag, "%s", message);
}

bool supportsLumaFormat(DxvkDevice* device, uint32_t width, uint32_t height) {
  if (!device || !device->features().core.features.shaderStorageImageExtendedFormats)
    return false;

  const VkFormatProperties formatProperties =
    device->adapter()->formatProperties(VK_FORMAT_R16_SFLOAT);
  const VkFormatFeatureFlags required =
    VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
  if ((formatProperties.optimalTilingFeatures & required) != required)
    return false;

  VkImageFormatProperties imageProperties{};
  const VkResult result = device->adapter()->imageFormatProperties(
    VK_FORMAT_R16_SFLOAT, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, imageProperties);

  const uint32_t lumaWidth = width / 2u + width % 2u;
  const uint32_t lumaHeight = height / 2u + height % 2u;
  return result == VK_SUCCESS
      && lumaWidth <= imageProperties.maxExtent.width
      && lumaHeight <= imageProperties.maxExtent.height;
}

bool initializeDeviceState(DeviceState& state, DxvkDevice* device,
                           uint32_t width, uint32_t height, VkFormat sourceFormat) {
  if (!supportsLumaFormat(device, width, height))
    return false;

  state.device = device;
  state.width = width;
  state.height = height;
  state.sourceFormat = sourceFormat;

  const uint32_t lumaWidth = width / 2u + width % 2u;
  const uint32_t lumaHeight = height / 2u + height % 2u;

  DxvkImageCreateInfo imageInfo{};
  imageInfo.type = VK_IMAGE_TYPE_2D;
  imageInfo.format = VK_FORMAT_R16_SFLOAT;
  imageInfo.sampleCount = VK_SAMPLE_COUNT_1_BIT;
  imageInfo.extent = { lumaWidth, lumaHeight, 1 };
  imageInfo.numLayers = 1;
  imageInfo.mipLevels = 1;
  imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  imageInfo.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
  imageInfo.access = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
  imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  imageInfo.layout = VK_IMAGE_LAYOUT_GENERAL;
  imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  imageInfo.shared = VK_FALSE;
  imageInfo.debugName = "SnapFrame262 luma history";

  DxvkImageViewKey viewInfo{};
  viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
  viewInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  viewInfo.format = VK_FORMAT_R16_SFLOAT;
  viewInfo.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
  viewInfo.mipIndex = 0;
  viewInfo.mipCount = 1;
  viewInfo.layerIndex = 0;
  viewInfo.layerCount = 1;

  for (uint32_t i = 0; i < state.lumaImages.size(); ++i) {
    state.lumaImages[i] = device->createImage(
      imageInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!state.lumaImages[i])
      return false;
    state.lumaViews[i] = state.lumaImages[i]->createView(viewInfo);
    if (!state.lumaViews[i])
      return false;
  }

  DxvkSamplerKey samplerKey{};
  samplerKey.setFilter(VK_FILTER_NEAREST, VK_FILTER_NEAREST,
                       VK_SAMPLER_MIPMAP_MODE_NEAREST);
  samplerKey.setAddressModes(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                             VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                             VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
  samplerKey.setLodRange(0.0f, 0.0f, 0.0f);
  samplerKey.setUsePixelCoordinates(false);
  state.sampler = device->createSampler(samplerKey);
  if (!state.sampler)
    return false;

  const DxvkBindingInfo bindings[] = {
    {
      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 0,
      VK_IMAGE_VIEW_TYPE_2D, VK_SHADER_STAGE_COMPUTE_BIT,
      VK_ACCESS_SHADER_READ_BIT
    },
    {
      VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
      VK_IMAGE_VIEW_TYPE_2D, VK_SHADER_STAGE_COMPUTE_BIT,
      VK_ACCESS_SHADER_WRITE_BIT
    },
  };

  DxvkShaderCreateInfo shaderInfo{};
  shaderInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  shaderInfo.bindingCount = uint32_t(std::size(bindings));
  shaderInfo.bindings = bindings;

  SpirvCodeBuffer shaderCode(
    snapframe::shaders::k_luma_downsample_word_count,
    snapframe::shaders::k_luma_downsample);
  state.lumaShader = new DxvkShader(shaderInfo, std::move(shaderCode));
  return state.lumaShader != nullptr;
}

void resetDeviceState(DeviceState& state) {
  state.lumaShader = nullptr;
  state.sampler = nullptr;
  for (auto& view : state.lumaViews)
    view = nullptr;
  for (auto& image : state.lumaImages)
    image = nullptr;
  state.device = nullptr;
}

} // anonymous namespace
} // namespace dxvk

extern "C" int snapframe_dxvk262_record_source(
    void* context,
    void* context_objects,
    void* device,
    void* source_view_ref,
    const SnapFrameDxvk262FrameInfo* frame_info) {
  using namespace dxvk;

  if (!context || !context_objects || !device || !source_view_ref || !frame_info)
    return 1;
  if (frame_info->struct_size < sizeof(SnapFrameDxvk262FrameInfo)
      || frame_info->abi_version != SNAPFRAME_DXVK262_ABI_VERSION
      || !frame_info->width || !frame_info->height)
    return 1;

  auto* dxvkContext = static_cast<DxvkContext*>(context);
  auto* dxvkDevice = static_cast<DxvkDevice*>(device);
  auto* contextObjects = static_cast<DxvkContextObjects*>(context_objects);
  auto* sourceViewRef = static_cast<Rc<DxvkImageView>*>(source_view_ref);

  if (!contextObjects->cmd || !contextObjects->descriptorPool || !*sourceViewRef)
    return 1;

  Rc<DxvkImageView> sourceView = *sourceViewRef;
  if (sourceView->info().viewType != VK_IMAGE_VIEW_TYPE_2D
      || sourceView->image()->info().sampleCount != VK_SAMPLE_COUNT_1_BIT)
    return 1;

  const VkExtent3D sourceExtent = sourceView->image()->info().extent;
  if (sourceExtent.width != frame_info->width
      || sourceExtent.height != frame_info->height
      || sourceView->image()->info().format != frame_info->format)
    return 1;

  std::lock_guard<std::mutex> lock(g_stateMutex);
  DeviceState& state = g_deviceStates[dxvkDevice];

  if (state.device != dxvkDevice
      || state.width != frame_info->width
      || state.height != frame_info->height
      || state.sourceFormat != frame_info->format) {
    resetDeviceState(state);
    if (!initializeDeviceState(state, dxvkDevice,
                               frame_info->width, frame_info->height,
                               frame_info->format)) {
      __android_log_print(ANDROID_LOG_WARN, kLogTag,
        "luma initialization skipped format=0x%x extent=%ux%u",
        uint32_t(frame_info->format), frame_info->width, frame_info->height);
      resetDeviceState(state);
      return 1;
    }
    logOnce("DXVK 2.6.2 luma compute host initialized");
  }

  const uint32_t slot = state.writeSlot;
  dxvkContext->bindShader<VK_SHADER_STAGE_COMPUTE_BIT>(
    Rc<DxvkShader>(state.lumaShader));
  dxvkContext->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, 0,
    Rc<DxvkImageView>(sourceView));
  dxvkContext->bindResourceSampler(VK_SHADER_STAGE_COMPUTE_BIT, 0,
    Rc<DxvkSampler>(state.sampler));
  dxvkContext->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, 1,
    Rc<DxvkImageView>(state.lumaViews[slot]));

  const uint32_t lumaWidth = frame_info->width / 2u + frame_info->width % 2u;
  const uint32_t lumaHeight = frame_info->height / 2u + frame_info->height % 2u;
  dxvkContext->dispatch((lumaWidth + 7u) / 8u, (lumaHeight + 7u) / 8u, 1u);

  ++state.recordedFrames;
  state.writeSlot ^= 1u;
  if (state.recordedFrames == 1u || (state.recordedFrames % 600u) == 0u) {
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
      "luma dispatch recorded frame=%llu extent=%ux%u slot=%u",
      static_cast<unsigned long long>(frame_info->frame_id),
      frame_info->width, frame_info->height, slot);
  }

  // Luma preprocessing is active, but frame interpolation is not yet
  // advertised as accepted/generated until bidirectional flow, validation,
  // midpoint warp and presentation are connected.
  return 1;
}

extern "C" void snapframe_dxvk262_release_window(void*) {
  // Per-window history is not allocated in the preprocessing-only stage.
}

extern "C" void snapframe_dxvk262_release_device(void* device_token) {
  using namespace dxvk;
  auto* device = static_cast<DxvkDevice*>(device_token);
  if (!device)
    return;

  std::lock_guard<std::mutex> lock(g_stateMutex);
  auto it = g_deviceStates.find(device);
  if (it == g_deviceStates.end())
    return;
  resetDeviceState(it->second);
  g_deviceStates.erase(it);
}
