/*
 * SnapFrame DXVK Native 2.6.2 host.
 *
 * This is intentionally a separate implementation from snapframe_host.cpp:
 * that file consumes private DXVK 1.9.2b APIs and must never be linked to the
 * 2.6.2 candidate. The host records luma, bidirectional flow, consistency,
 * owned RGBA16F color history and midpoint reconstruction through DxvkContext.
 * DXVK owns barriers, descriptor allocation, pipeline compilation, command
 * submission and resource lifetime tracking; this host never submits directly.
 */
#include "snapframe_dxvk262_api.h"

#include <android/log.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <mutex>
#include <unordered_map>
#include <utility>

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
  std::array<Rc<DxvkImage>, 2> flowImages;
  std::array<Rc<DxvkImageView>, 2> flowViews;
  std::array<Rc<DxvkImage>, 2> colorHistoryImages;
  std::array<Rc<DxvkImageView>, 2> colorHistoryViews;
  Rc<DxvkImage> midpointImage;
  Rc<DxvkImageView> midpointView;
  Rc<DxvkImage> consistencyImage;
  Rc<DxvkImageView> consistencyView;
  Rc<DxvkSampler> sampler;
  Rc<DxvkShader> lumaShader;
  Rc<DxvkShader> flowShader;
  Rc<DxvkShader> consistencyShader;
  Rc<DxvkShader> colorHistoryShader;
  Rc<DxvkShader> midpointWarpShader;
  uint32_t flowWidth = 0;
  uint32_t flowHeight = 0;
  bool hasPreviousLuma = false;
  uint64_t flowPairsRecorded = 0;
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

  // The existing V1 block matcher uses one flow cell per 16 luma texels
  // at the default 50% scale. Keep this scale fixed until AI settings are
  // plumbed into the 2.6.2 host; vectors remain in luma-texel units.
  constexpr uint32_t flowStride = 16u;
  state.flowWidth = (lumaWidth + flowStride - 1u) / flowStride;
  state.flowHeight = (lumaHeight + flowStride - 1u) / flowStride;
  const VkFormat flowFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
  const VkFormatProperties flowProperties = device->adapter()->formatProperties(flowFormat);
  const VkFormatFeatureFlags flowRequired =
    VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
  VkImageFormatProperties flowImageProperties{};
  const VkResult flowResult = device->adapter()->imageFormatProperties(
    flowFormat, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, flowImageProperties);
  if ((flowProperties.optimalTilingFeatures & flowRequired) != flowRequired
      || flowResult != VK_SUCCESS
      || state.flowWidth > flowImageProperties.maxExtent.width
      || state.flowHeight > flowImageProperties.maxExtent.height
      || !device->features().core.features.shaderStorageImageExtendedFormats)
    return false;

  DxvkImageCreateInfo flowInfo{};
  flowInfo.type = VK_IMAGE_TYPE_2D;
  flowInfo.format = flowFormat;
  flowInfo.sampleCount = VK_SAMPLE_COUNT_1_BIT;
  flowInfo.extent = { state.flowWidth, state.flowHeight, 1 };
  flowInfo.numLayers = 1;
  flowInfo.mipLevels = 1;
  flowInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  flowInfo.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
  flowInfo.access = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
  flowInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  flowInfo.layout = VK_IMAGE_LAYOUT_GENERAL;
  flowInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  flowInfo.shared = VK_FALSE;
  flowInfo.debugName = "SnapFrame262 bidirectional flow";

  DxvkImageViewKey flowViewInfo{};
  flowViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
  flowViewInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  flowViewInfo.format = flowFormat;
  flowViewInfo.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
  flowViewInfo.mipIndex = 0;
  flowViewInfo.mipCount = 1;
  flowViewInfo.layerIndex = 0;
  flowViewInfo.layerCount = 1;
  for (uint32_t i = 0; i < state.flowImages.size(); ++i) {
    state.flowImages[i] = device->createImage(
      flowInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!state.flowImages[i])
      return false;
    state.flowViews[i] = state.flowImages[i]->createView(flowViewInfo);
    if (!state.flowViews[i])
      return false;
  }

  const DxvkBindingInfo flowBindings[] = {
    { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 0,
      VK_IMAGE_VIEW_TYPE_2D, VK_SHADER_STAGE_COMPUTE_BIT, VK_ACCESS_SHADER_READ_BIT },
    { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
      VK_IMAGE_VIEW_TYPE_2D, VK_SHADER_STAGE_COMPUTE_BIT, VK_ACCESS_SHADER_READ_BIT },
    { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2,
      VK_IMAGE_VIEW_TYPE_2D, VK_SHADER_STAGE_COMPUTE_BIT, VK_ACCESS_SHADER_WRITE_BIT },
  };
  DxvkShaderCreateInfo flowShaderInfo{};
  flowShaderInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  flowShaderInfo.bindingCount = uint32_t(std::size(flowBindings));
  flowShaderInfo.bindings = flowBindings;
  SpirvCodeBuffer flowShaderCode(
    snapframe::shaders::k_block_match_flow_word_count,
    snapframe::shaders::k_block_match_flow);
  state.flowShader = new DxvkShader(flowShaderInfo, std::move(flowShaderCode));
  if (!state.flowShader)
    return false;

  // Forward/backward cycle validation writes a soft R16F confidence mask.
  DxvkImageCreateInfo consistencyInfo = flowInfo;
  consistencyInfo.format = VK_FORMAT_R16_SFLOAT;
  state.consistencyImage = device->createImage(
    consistencyInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (!state.consistencyImage)
    return false;
  DxvkImageViewKey consistencyViewInfo = flowViewInfo;
  consistencyViewInfo.format = VK_FORMAT_R16_SFLOAT;
  state.consistencyView = state.consistencyImage->createView(consistencyViewInfo);
  if (!state.consistencyView)
    return false;

  const DxvkBindingInfo consistencyBindings[] = {
    { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 0,
      VK_IMAGE_VIEW_TYPE_2D, VK_SHADER_STAGE_COMPUTE_BIT, VK_ACCESS_SHADER_READ_BIT },
    { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
      VK_IMAGE_VIEW_TYPE_2D, VK_SHADER_STAGE_COMPUTE_BIT, VK_ACCESS_SHADER_READ_BIT },
    { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2,
      VK_IMAGE_VIEW_TYPE_2D, VK_SHADER_STAGE_COMPUTE_BIT, VK_ACCESS_SHADER_WRITE_BIT },
    { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3,
      VK_IMAGE_VIEW_TYPE_2D, VK_SHADER_STAGE_COMPUTE_BIT, VK_ACCESS_SHADER_READ_BIT },
  };
  DxvkShaderCreateInfo consistencyShaderInfo{};
  consistencyShaderInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  consistencyShaderInfo.bindingCount = uint32_t(std::size(consistencyBindings));
  consistencyShaderInfo.bindings = consistencyBindings;
  SpirvCodeBuffer consistencyShaderCode(
    snapframe::shaders::k_flow_consistency_word_count,
    snapframe::shaders::k_flow_consistency);
  state.consistencyShader = new DxvkShader(
    consistencyShaderInfo, std::move(consistencyShaderCode));
  if (!state.consistencyShader)
    return false;

  const VkFormat colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
  const VkFormatProperties colorProperties = device->adapter()->formatProperties(colorFormat);
  const VkFormatFeatureFlags colorRequired =
    VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
  VkImageFormatProperties colorImageProperties{};
  const VkResult colorResult = device->adapter()->imageFormatProperties(
    colorFormat, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, colorImageProperties);
  if ((colorProperties.optimalTilingFeatures & colorRequired) != colorRequired
      || colorResult != VK_SUCCESS
      || width > colorImageProperties.maxExtent.width
      || height > colorImageProperties.maxExtent.height)
    return false;

  DxvkImageCreateInfo colorInfo = flowInfo;
  colorInfo.format = colorFormat;
  colorInfo.extent = { width, height, 1 };
  colorInfo.debugName = "SnapFrame262 RGBA16F color history";
  DxvkImageViewKey colorViewInfo = flowViewInfo;
  colorViewInfo.format = colorFormat;
  for (uint32_t i = 0; i < state.colorHistoryImages.size(); ++i) {
    state.colorHistoryImages[i] = device->createImage(
      colorInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!state.colorHistoryImages[i])
      return false;
    state.colorHistoryViews[i] = state.colorHistoryImages[i]->createView(colorViewInfo);
    if (!state.colorHistoryViews[i])
      return false;
  }
  colorInfo.debugName = "SnapFrame262 midpoint output";
  state.midpointImage = device->createImage(
    colorInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (!state.midpointImage)
    return false;
  state.midpointView = state.midpointImage->createView(colorViewInfo);
  if (!state.midpointView)
    return false;

  const DxvkBindingInfo colorCopyBindings[] = {
    { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 0,
      VK_IMAGE_VIEW_TYPE_2D, VK_SHADER_STAGE_COMPUTE_BIT, VK_ACCESS_SHADER_READ_BIT },
    { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
      VK_IMAGE_VIEW_TYPE_2D, VK_SHADER_STAGE_COMPUTE_BIT, VK_ACCESS_SHADER_WRITE_BIT },
  };
  DxvkShaderCreateInfo colorCopyInfo{};
  colorCopyInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  colorCopyInfo.bindingCount = uint32_t(std::size(colorCopyBindings));
  colorCopyInfo.bindings = colorCopyBindings;
  SpirvCodeBuffer colorCopyCode(
    snapframe::shaders::k_color_history_copy_word_count,
    snapframe::shaders::k_color_history_copy);
  state.colorHistoryShader = new DxvkShader(colorCopyInfo, std::move(colorCopyCode));
  if (!state.colorHistoryShader)
    return false;

  const DxvkBindingInfo midpointBindings[] = {
    { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 0,
      VK_IMAGE_VIEW_TYPE_2D, VK_SHADER_STAGE_COMPUTE_BIT, VK_ACCESS_SHADER_READ_BIT },
    { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
      VK_IMAGE_VIEW_TYPE_2D, VK_SHADER_STAGE_COMPUTE_BIT, VK_ACCESS_SHADER_READ_BIT },
    { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2,
      VK_IMAGE_VIEW_TYPE_2D, VK_SHADER_STAGE_COMPUTE_BIT, VK_ACCESS_SHADER_READ_BIT },
    { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3,
      VK_IMAGE_VIEW_TYPE_2D, VK_SHADER_STAGE_COMPUTE_BIT, VK_ACCESS_SHADER_READ_BIT },
    { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4,
      VK_IMAGE_VIEW_TYPE_2D, VK_SHADER_STAGE_COMPUTE_BIT, VK_ACCESS_SHADER_READ_BIT },
    { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 5,
      VK_IMAGE_VIEW_TYPE_2D, VK_SHADER_STAGE_COMPUTE_BIT, VK_ACCESS_SHADER_WRITE_BIT },
  };
  DxvkShaderCreateInfo midpointInfo{};
  midpointInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  midpointInfo.bindingCount = uint32_t(std::size(midpointBindings));
  midpointInfo.bindings = midpointBindings;
  SpirvCodeBuffer midpointCode(
    snapframe::shaders::k_midpoint_warp_word_count,
    snapframe::shaders::k_midpoint_warp);
  state.midpointWarpShader = new DxvkShader(midpointInfo, std::move(midpointCode));
  if (!state.midpointWarpShader)
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
  state.midpointWarpShader = nullptr;
  state.colorHistoryShader = nullptr;
  state.consistencyShader = nullptr;
  state.flowShader = nullptr;
  state.lumaShader = nullptr;
  state.midpointView = nullptr;
  state.midpointImage = nullptr;
  for (auto& view : state.colorHistoryViews)
    view = nullptr;
  for (auto& image : state.colorHistoryImages)
    image = nullptr;
  state.consistencyView = nullptr;
  state.consistencyImage = nullptr;
  state.sampler = nullptr;
  for (auto& view : state.flowViews)
    view = nullptr;
  for (auto& image : state.flowImages)
    image = nullptr;
  for (auto& view : state.lumaViews)
    view = nullptr;
  for (auto& image : state.lumaImages)
    image = nullptr;
  state.width = 0;
  state.height = 0;
  state.sourceFormat = VK_FORMAT_UNDEFINED;
  state.flowWidth = 0;
  state.flowHeight = 0;
  state.hasPreviousLuma = false;
  state.flowPairsRecorded = 0;
  state.recordedFrames = 0;
  state.writeSlot = 0;
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
      || !(sourceView->image()->info().usage & VK_IMAGE_USAGE_SAMPLED_BIT)
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
    logOnce("DXVK 2.6.2 temporal compute host initialized");
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

  // Copy the real source into an owned linear RGBA16F history image. The
  // source swapchain image may be recycled immediately after presentation.
  dxvkContext->bindShader<VK_SHADER_STAGE_COMPUTE_BIT>(
    Rc<DxvkShader>(state.colorHistoryShader));
  dxvkContext->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, 0,
    Rc<DxvkImageView>(sourceView));
  dxvkContext->bindResourceSampler(VK_SHADER_STAGE_COMPUTE_BIT, 0,
    Rc<DxvkSampler>(state.sampler));
  dxvkContext->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, 1,
    Rc<DxvkImageView>(state.colorHistoryViews[slot]));
  dxvkContext->dispatch((frame_info->width + 7u) / 8u,
                        (frame_info->height + 7u) / 8u, 1u);

  // The first captured image only seeds temporal history. From the second
  // frame onward, record both directions into separate flow fields.
  if (state.hasPreviousLuma) {
    const uint32_t previousSlot = slot ^ 1u;
    dxvkContext->bindShader<VK_SHADER_STAGE_COMPUTE_BIT>(
      Rc<DxvkShader>(state.flowShader));

    // Current -> previous: compare the current patch against displaced
    // samples from the previous luma image.
    dxvkContext->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, 0,
      Rc<DxvkImageView>(state.lumaViews[previousSlot]));
    dxvkContext->bindResourceSampler(VK_SHADER_STAGE_COMPUTE_BIT, 0,
      Rc<DxvkSampler>(state.sampler));
    dxvkContext->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, 1,
      Rc<DxvkImageView>(state.lumaViews[slot]));
    dxvkContext->bindResourceSampler(VK_SHADER_STAGE_COMPUTE_BIT, 1,
      Rc<DxvkSampler>(state.sampler));
    dxvkContext->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, 2,
      Rc<DxvkImageView>(state.flowViews[0]));
    dxvkContext->dispatch((state.flowWidth + 7u) / 8u,
                          (state.flowHeight + 7u) / 8u, 1u);

    // Previous -> current: reverse the sampled luma pair.
    dxvkContext->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, 0,
      Rc<DxvkImageView>(state.lumaViews[slot]));
    dxvkContext->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, 1,
      Rc<DxvkImageView>(state.lumaViews[previousSlot]));
    dxvkContext->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, 2,
      Rc<DxvkImageView>(state.flowViews[1]));
    dxvkContext->dispatch((state.flowWidth + 7u) / 8u,
                          (state.flowHeight + 7u) / 8u, 1u);
    dxvkContext->bindShader<VK_SHADER_STAGE_COMPUTE_BIT>(
      Rc<DxvkShader>(state.consistencyShader));
    dxvkContext->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, 0,
      Rc<DxvkImageView>(state.flowViews[0]));
    dxvkContext->bindResourceSampler(VK_SHADER_STAGE_COMPUTE_BIT, 0,
      Rc<DxvkSampler>(state.sampler));
    dxvkContext->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, 1,
      Rc<DxvkImageView>(state.flowViews[1]));
    dxvkContext->bindResourceSampler(VK_SHADER_STAGE_COMPUTE_BIT, 1,
      Rc<DxvkSampler>(state.sampler));
    dxvkContext->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, 2,
      Rc<DxvkImageView>(state.consistencyView));
    dxvkContext->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, 3,
      Rc<DxvkImageView>(state.lumaViews[slot]));
    dxvkContext->bindResourceSampler(VK_SHADER_STAGE_COMPUTE_BIT, 3,
      Rc<DxvkSampler>(state.sampler));
    dxvkContext->dispatch((state.flowWidth + 7u) / 8u,
                          (state.flowHeight + 7u) / 8u, 1u);
    dxvkContext->bindShader<VK_SHADER_STAGE_COMPUTE_BIT>(
      Rc<DxvkShader>(state.midpointWarpShader));
    dxvkContext->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, 0,
      Rc<DxvkImageView>(state.colorHistoryViews[previousSlot]));
    dxvkContext->bindResourceSampler(VK_SHADER_STAGE_COMPUTE_BIT, 0,
      Rc<DxvkSampler>(state.sampler));
    dxvkContext->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, 1,
      Rc<DxvkImageView>(state.colorHistoryViews[slot]));
    dxvkContext->bindResourceSampler(VK_SHADER_STAGE_COMPUTE_BIT, 1,
      Rc<DxvkSampler>(state.sampler));
    dxvkContext->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, 2,
      Rc<DxvkImageView>(state.flowViews[0]));
    dxvkContext->bindResourceSampler(VK_SHADER_STAGE_COMPUTE_BIT, 2,
      Rc<DxvkSampler>(state.sampler));
    dxvkContext->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, 3,
      Rc<DxvkImageView>(state.flowViews[1]));
    dxvkContext->bindResourceSampler(VK_SHADER_STAGE_COMPUTE_BIT, 3,
      Rc<DxvkSampler>(state.sampler));
    dxvkContext->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, 4,
      Rc<DxvkImageView>(state.consistencyView));
    dxvkContext->bindResourceSampler(VK_SHADER_STAGE_COMPUTE_BIT, 4,
      Rc<DxvkSampler>(state.sampler));
    dxvkContext->bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT, 5,
      Rc<DxvkImageView>(state.midpointView));
    dxvkContext->dispatch((frame_info->width + 7u) / 8u,
                          (frame_info->height + 7u) / 8u, 1u);
    ++state.flowPairsRecorded;
  }

  state.hasPreviousLuma = true;
  ++state.recordedFrames;
  state.writeSlot ^= 1u;
  if (state.recordedFrames == 1u || (state.recordedFrames % 600u) == 0u) {
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
      "luma/flow/consistency/midpoint dispatch frame=%llu extent=%ux%u slot=%u flow_pairs=%llu flow_extent=%ux%u",
      static_cast<unsigned long long>(frame_info->frame_id),
      frame_info->width, frame_info->height, slot,
      static_cast<unsigned long long>(state.flowPairsRecorded),
      state.flowWidth, state.flowHeight);
  }

  // Luma, bidirectional flow and cycle consistency are now recorded. The
  // hook still skips FG until the midpoint image is wired into the WSI blit source.
  return 1;
}

extern "C" void snapframe_dxvk262_release_window(void*) {
  // Temporal history is device-scoped in this stage; window retirement does
  // not destroy resources shared by the DXVK device.
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
