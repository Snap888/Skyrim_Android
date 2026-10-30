#include "lsfg_dxvk_generation.h"

#include <algorithm>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "dxvk_buffer.h"
#include "dxvk_context.h"
#include "dxvk_device.h"
#include "dxvk_image.h"
#include "dxvk_sampler.h"
#include "lsfg_dxvk_shader.h"
#include "lsfg_dxvk_shared.h"

namespace dxvk {

namespace {

constexpr uint32_t MipLevels = 7;
constexpr uint32_t HistorySlots = 3;
constexpr uint32_t GenerationSlots = 6;
constexpr uint32_t FirstDeltaLevel = 4;
constexpr uint32_t DeltaInstances = 3;

struct LsfgConstants {
  uint32_t inputOffset[2];
  uint32_t firstIter;
  uint32_t firstIterS;
  uint32_t advancedColorKind;
  uint32_t hdrSupport;
  float resolutionInvScale;
  float timestamp;
  float uiThreshold;
  uint32_t padding[3];
};
static_assert(sizeof(LsfgConstants) == 48, "LSFG UBO layout must match WinNative");

struct Image {
  Rc<DxvkImage> image;
  Rc<DxvkImageView> view;
  VkExtent2D extent = { 0, 0 };
};

struct GammaState {
  Image temp1[3];
  Image temp2[2];
  Image output;
  Rc<DxvkBuffer> constants[GenerationSlots];
};

struct DeltaState {
  Image temp1[3];
  Image temp2[2];
  Image output1;
  Image output2;
  Rc<DxvkBuffer> constants[GenerationSlots];
};

static Rc<DxvkShader> g_gammaShaders[5];
static Rc<DxvkShader> g_deltaShaders[10];
static Rc<DxvkShader> g_generateShader;
static Rc<DxvkSampler> g_sampler;
static Rc<DxvkSampler> g_whiteBorderSampler;
static Rc<DxvkSampler> g_edgeSampler;
static Rc<DxvkBuffer> g_generateConstants[GenerationSlots];
static Image g_dummyMotion;
static GammaState g_gamma[MipLevels];
static DeltaState g_delta[DeltaInstances];

static DxvkDevice* g_device = nullptr;
static uint32_t g_deviceGeneration = UINT32_MAX;
static VkExtent2D g_flowExtent = { 0, 0 };
static float g_flowScale = 0.0f;
static char g_cacheDir[512] = {};
static bool g_ready = false;
static bool g_errorLogged = false;

static Image g_target;
static DxvkDevice* g_targetDevice = nullptr;
static uint32_t g_targetGeneration = UINT32_MAX;
static VkExtent2D g_targetExtent = { 0, 0 };
static VkFormat g_targetFormat = VK_FORMAT_UNDEFINED;

static uint32_t generationSlot(uint32_t generationCount, uint32_t generationIndex) {
  return (generationCount - 1u) * generationCount / 2u + generationIndex;
}

static float slotTimestamp(uint32_t slot) {
  uint32_t count = 1;
  while (count * (count + 1u) / 2u <= slot) count++;
  const uint32_t first = (count - 1u) * count / 2u;
  return float(slot - first + 1u) / float(count + 1u);
}

static void clearResources() {
  for (auto& shader : g_gammaShaders) shader = Rc<DxvkShader>();
  for (auto& shader : g_deltaShaders) shader = Rc<DxvkShader>();
  g_generateShader = Rc<DxvkShader>();
  g_sampler = Rc<DxvkSampler>();
  g_whiteBorderSampler = Rc<DxvkSampler>();
  g_edgeSampler = Rc<DxvkSampler>();
  g_dummyMotion = Image();

  for (auto& gamma : g_gamma) {
    for (auto& image : gamma.temp1) image = Image();
    for (auto& image : gamma.temp2) image = Image();
    gamma.output = Image();
    for (auto& buffer : gamma.constants) buffer = Rc<DxvkBuffer>();
  }
  for (auto& delta : g_delta) {
    for (auto& image : delta.temp1) image = Image();
    for (auto& image : delta.temp2) image = Image();
    delta.output1 = Image();
    delta.output2 = Image();
    for (auto& buffer : delta.constants) buffer = Rc<DxvkBuffer>();
  }
  for (auto& buffer : g_generateConstants) buffer = Rc<DxvkBuffer>();
  g_ready = false;
}

static void resetResources() {
  clearResources();
  g_device = nullptr;
  g_deviceGeneration = UINT32_MAX;
  g_flowExtent = { 0, 0 };
  g_flowScale = 0.0f;
  g_cacheDir[0] = 0;
  g_errorLogged = false;
}

static bool loadSpirv(const char* path, std::vector<uint32_t>& code) {
  FILE* file = fopen(path, "rb");
  if (!file) return false;
  fseek(file, 0, SEEK_END);
  long size = ftell(file);
  fseek(file, 0, SEEK_SET);
  if (size < 20 || (size & 3)) { fclose(file); return false; }
  code.resize(size_t(size) / sizeof(uint32_t));
  bool ok = fread(code.data(), 1, size_t(size), file) == size_t(size);
  fclose(file);
  return ok && code[0] == 0x07230203u;
}

static Rc<DxvkShader> loadShader(
        DxvkDevice* device, const char* cacheDir, uint32_t id, std::string& error) {
  char path[768];
  snprintf(path, sizeof(path), "%s/%u.spv", cacheDir, id);
  std::vector<uint32_t> code;
  if (!loadSpirv(path, code)) {
    error = "cannot load LSFG shader " + std::to_string(id);
    return Rc<DxvkShader>();
  }
  return createLsfgComputeShader(device, code, error);
}

static Rc<DxvkSampler> createSampler(
        DxvkDevice* device, VkSamplerAddressMode address, bool whiteBorder) {
  DxvkSamplerCreateInfo info{};
  info.magFilter = VK_FILTER_LINEAR;
  info.minFilter = VK_FILTER_LINEAR;
  info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  info.mipmapLodBias = 0.0f;
  info.mipmapLodMin = 0.0f;
  info.mipmapLodMax = 0.0f;
  info.useAnisotropy = VK_FALSE;
  info.maxAnisotropy = 1.0f;
  info.addressModeU = address;
  info.addressModeV = address;
  info.addressModeW = address;
  info.compareToDepth = VK_FALSE;
  info.compareOp = VK_COMPARE_OP_NEVER;
  const float border = whiteBorder ? 1.0f : 0.0f;
  for (unsigned i = 0; i < 4; i++) info.borderColor.float32[i] = border;
  info.usePixelCoord = VK_FALSE;
  return device->createSampler(info);
}

static bool createImage(
        DxvkDevice* device, VkExtent2D extent, VkFormat format, Image& out,
        VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VkPipelineStageFlags stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VkAccessFlags access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT) {
  extent.width = std::max(1u, extent.width);
  extent.height = std::max(1u, extent.height);

  DxvkImageCreateInfo imageInfo{};
  imageInfo.type = VK_IMAGE_TYPE_2D;
  imageInfo.format = format;
  imageInfo.flags = 0;
  imageInfo.sampleCount = VK_SAMPLE_COUNT_1_BIT;
  imageInfo.extent = { extent.width, extent.height, 1 };
  imageInfo.numLayers = 1;
  imageInfo.mipLevels = 1;
  imageInfo.usage = usage;
  imageInfo.stages = stages;
  imageInfo.access = access;
  imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  imageInfo.layout = VK_IMAGE_LAYOUT_GENERAL;
  imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  imageInfo.shared = VK_FALSE;
  out.image = device->createImage(imageInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (out.image == nullptr) return false;

  DxvkImageViewCreateInfo viewInfo{};
  viewInfo.type = VK_IMAGE_VIEW_TYPE_2D;
  viewInfo.format = format;
  viewInfo.usage = usage;
  viewInfo.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
  viewInfo.minLevel = 0;
  viewInfo.numLevels = 1;
  viewInfo.minLayer = 0;
  viewInfo.numLayers = 1;
  out.view = device->createImageView(out.image, viewInfo);
  if (out.view == nullptr) return false;
  out.extent = extent;
  return true;
}

static Rc<DxvkBuffer> createConstants(
        DxvkDevice* device, float scale, float timestamp, bool firstIter, bool firstIterS) {
  DxvkBufferCreateInfo info{};
  info.size = sizeof(LsfgConstants);
  info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
  info.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
  info.access = VK_ACCESS_SHADER_READ_BIT;
  Rc<DxvkBuffer> buffer = device->createBuffer(
    info, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (buffer == nullptr || !buffer->mapPtr(0)) return Rc<DxvkBuffer>();

  LsfgConstants constants{};
  constants.firstIter = firstIter ? 1u : 0u;
  constants.firstIterS = firstIterS ? 1u : 0u;
  constants.resolutionInvScale = 1.0f / scale;
  constants.timestamp = timestamp;
  constants.uiThreshold = 0.5f;
  memcpy(buffer->mapPtr(0), &constants, sizeof(constants));
  return buffer;
}

static bool ensureResources(DxvkDevice* device, const char* cacheDir,
                            VkExtent2D flowExtent, float scale, uint32_t generation) {
  if (g_device != device || g_deviceGeneration != generation
      || g_flowExtent.width != flowExtent.width || g_flowExtent.height != flowExtent.height
      || g_flowScale != scale
      || strncmp(g_cacheDir, cacheDir ? cacheDir : "", sizeof(g_cacheDir)) != 0) {
    resetResources();
    g_device = device;
    g_deviceGeneration = generation;
    g_flowExtent = flowExtent;
    g_flowScale = scale;
    snprintf(g_cacheDir, sizeof(g_cacheDir), "%s", cacheDir ? cacheDir : "");
  }
  if (g_ready) return true;
  if (!device || !cacheDir || !cacheDir[0] || !lsfgSharedPassesReady()
      || scale <= 0.0f || scale > 1.0f) return false;

  std::string error;
  const uint32_t gammaIds[5] = { 280, 282, 283, 284, 285 };
  const uint32_t deltaIds[10] = { 280, 286, 287, 288, 289, 281, 294, 295, 296, 297 };
  for (unsigned i = 0; i < 5 && error.empty(); i++) {
    g_gammaShaders[i] = loadShader(device, cacheDir, gammaIds[i], error);
    if (g_gammaShaders[i] == nullptr && error.empty()) error = "gamma shader creation failed";
  }
  for (unsigned i = 0; i < 10 && error.empty(); i++) {
    g_deltaShaders[i] = loadShader(device, cacheDir, deltaIds[i], error);
    if (g_deltaShaders[i] == nullptr && error.empty()) error = "delta shader creation failed";
  }
  if (error.empty()) {
    g_generateShader = loadShader(device, cacheDir, 256, error);
    if (g_generateShader == nullptr && error.empty()) error = "generate shader creation failed";
  }

  if (error.empty()) {
    g_sampler = createSampler(device, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, false);
    g_whiteBorderSampler = createSampler(device, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, true);
    g_edgeSampler = createSampler(device, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, false);
    if (g_sampler == nullptr || g_whiteBorderSampler == nullptr || g_edgeSampler == nullptr)
      error = "generation sampler creation failed";
  }

  if (error.empty()
      && !createImage(device, { 1, 1 }, VK_FORMAT_R16G16B16A16_SFLOAT, g_dummyMotion))
    error = "dummy motion image allocation failed";

  for (unsigned i = 0; i < MipLevels && error.empty(); i++) {
    const uint32_t level = MipLevels - 1u - i;
    const VkExtent2D extent = lsfgAlphaHistoryExtent(level);
    if (extent.width == 0 || extent.height == 0) {
      error = "alpha history extent unavailable";
      break;
    }
    GammaState& gamma = g_gamma[i];
    for (auto& image : gamma.temp1) {
      if (!createImage(device, extent, VK_FORMAT_R8G8B8A8_UNORM, image)) {
        error = "gamma temporary image allocation failed";
        break;
      }
    }
    for (auto& image : gamma.temp2) {
      if (error.empty() && !createImage(device, extent, VK_FORMAT_R8G8B8A8_UNORM, image))
        error = "gamma temporary image allocation failed";
    }
    if (error.empty()
        && !createImage(device, extent, VK_FORMAT_R16G16B16A16_SFLOAT, gamma.output))
      error = "gamma motion output allocation failed";

    for (unsigned slot = 0; slot < GenerationSlots && error.empty(); slot++) {
      gamma.constants[slot] = createConstants(
        device, scale, slotTimestamp(slot), i == 0, false);
      if (gamma.constants[slot] == nullptr) error = "gamma uniform buffer allocation failed";
    }
  }

  for (unsigned i = 0; i < DeltaInstances && error.empty(); i++) {
    const uint32_t level = MipLevels - 1u - (FirstDeltaLevel + i);
    const VkExtent2D extent = lsfgAlphaHistoryExtent(level);
    if (extent.width == 0 || extent.height == 0) {
      error = "delta alpha history extent unavailable";
      break;
    }
    DeltaState& delta = g_delta[i];
    for (auto& image : delta.temp1) {
      if (!createImage(device, extent, VK_FORMAT_R8G8B8A8_UNORM, image)) {
        error = "delta temporary image allocation failed";
        break;
      }
    }
    for (auto& image : delta.temp2) {
      if (error.empty() && !createImage(device, extent, VK_FORMAT_R8G8B8A8_UNORM, image))
        error = "delta temporary image allocation failed";
    }
    if (error.empty()
        && !createImage(device, extent, VK_FORMAT_R16G16B16A16_SFLOAT, delta.output1))
      error = "delta first motion output allocation failed";
    if (error.empty()
        && !createImage(device, extent, VK_FORMAT_R16G16B16A16_SFLOAT, delta.output2))
      error = "delta second motion output allocation failed";

    for (unsigned slot = 0; slot < GenerationSlots && error.empty(); slot++) {
      delta.constants[slot] = createConstants(
        device, scale, slotTimestamp(slot), false, i == 0);
      if (delta.constants[slot] == nullptr) error = "delta uniform buffer allocation failed";
    }
  }

  for (unsigned slot = 0; slot < GenerationSlots && error.empty(); slot++) {
    g_generateConstants[slot] = createConstants(
      device, scale, slotTimestamp(slot), false, false);
    if (g_generateConstants[slot] == nullptr) error = "generate uniform buffer allocation failed";
  }

  if (!error.empty()) {
    if (!g_errorLogged) {
      lsfg_log("E", "step3j: gamma/delta/generate initialization failed: %s", error.c_str());
      g_errorLogged = true;
    }
    clearResources();
    return false;
  }

  g_ready = true;
  lsfg_log("I", "step3j: gamma/delta/generate DXVK resources ready (7 gamma levels, 3 paired delta levels)");
  return true;
}

static void bindImage(DxvkContext* context, uint32_t slot, const Rc<DxvkImageView>& view) {
  context->bindResourceView(slot, view, nullptr);
}

static void bindBuffer(DxvkContext* context, uint32_t slot, const Rc<DxvkBuffer>& buffer) {
  context->bindResourceBuffer(slot, DxvkBufferSlice(buffer));
}

static void dispatchGammaStep(DxvkContext* context, uint32_t frameIndex,
                              uint32_t slot, uint32_t index, uint32_t stage) {
  const uint32_t level = MipLevels - 1u - index;
  const uint32_t history = frameIndex % HistorySlots;
  const uint32_t previousHistory = (frameIndex + HistorySlots - 1u) % HistorySlots;
  GammaState& gamma = g_gamma[index];
  const Rc<DxvkImageView>& previous = index == 0
    ? g_dummyMotion.view : g_gamma[index - 1u].output.view;

  if (stage == 0) {
    bindBuffer(context, 0, gamma.constants[slot]);
    context->bindResourceSampler(1, g_whiteBorderSampler);
    context->bindResourceSampler(2, g_edgeSampler);
    bindImage(context, 3, lsfgAlphaHistoryView(level, previousHistory, 0));
    bindImage(context, 4, lsfgAlphaHistoryView(level, previousHistory, 1));
    bindImage(context, 5, lsfgAlphaHistoryView(level, history, 0));
    bindImage(context, 6, lsfgAlphaHistoryView(level, history, 1));
    bindImage(context, 7, previous);
    for (unsigned i = 0; i < 3; i++) bindImage(context, 8 + i, gamma.temp1[i].view);
  } else if (stage == 1) {
    context->bindResourceSampler(0, g_sampler);
    for (unsigned i = 0; i < 3; i++) bindImage(context, 1 + i, gamma.temp1[i].view);
    for (unsigned i = 0; i < 2; i++) bindImage(context, 4 + i, gamma.temp2[i].view);
  } else if (stage == 2) {
    context->bindResourceSampler(0, g_sampler);
    for (unsigned i = 0; i < 2; i++) bindImage(context, 1 + i, gamma.temp2[i].view);
    bindImage(context, 3, gamma.temp1[0].view);
    bindImage(context, 4, gamma.temp1[1].view);
  } else if (stage == 3) {
    context->bindResourceSampler(0, g_sampler);
    bindImage(context, 1, gamma.temp1[0].view);
    bindImage(context, 2, gamma.temp1[1].view);
    for (unsigned i = 0; i < 2; i++) bindImage(context, 3 + i, gamma.temp2[i].view);
  } else {
    bindBuffer(context, 0, gamma.constants[slot]);
    context->bindResourceSampler(1, g_sampler);
    context->bindResourceSampler(2, g_edgeSampler);
    for (unsigned i = 0; i < 2; i++) bindImage(context, 3 + i, gamma.temp2[i].view);
    bindImage(context, 5, previous);
    bindImage(context, 6, lsfgBetaOutputView(std::min(level, 5u)));
    bindImage(context, 7, gamma.output.view);
  }

  context->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, g_gammaShaders[stage]);
  const VkExtent2D extent = gamma.temp1[0].extent;
  context->dispatch((extent.width + 7u) >> 3, (extent.height + 7u) >> 3, 1);
}

static void dispatchDeltaStep(DxvkContext* context, uint32_t frameIndex,
                              uint32_t slot, uint32_t index, uint32_t stage) {
  const uint32_t level = MipLevels - 1u - (FirstDeltaLevel + index);
  const uint32_t gammaIndex = FirstDeltaLevel + index;
  const uint32_t history = frameIndex % HistorySlots;
  const uint32_t previousHistory = (frameIndex + HistorySlots - 1u) % HistorySlots;
  DeltaState& delta = g_delta[index];

  const Rc<DxvkImageView>& previousGamma = index == 0
    ? g_dummyMotion.view : g_gamma[gammaIndex - 1u].output.view;
  const Rc<DxvkImageView>& previous1 = index == 0
    ? g_dummyMotion.view : g_delta[index - 1u].output1.view;
  const Rc<DxvkImageView>& previous2 = index == 0
    ? g_dummyMotion.view : g_delta[index - 1u].output2.view;

  if (stage == 0) {
    bindBuffer(context, 0, delta.constants[slot]);
    context->bindResourceSampler(1, g_whiteBorderSampler);
    context->bindResourceSampler(2, g_edgeSampler);
    bindImage(context, 3, lsfgAlphaHistoryView(level, previousHistory, 0));
    bindImage(context, 4, lsfgAlphaHistoryView(level, previousHistory, 1));
    bindImage(context, 5, lsfgAlphaHistoryView(level, history, 0));
    bindImage(context, 6, lsfgAlphaHistoryView(level, history, 1));
    bindImage(context, 7, previousGamma);
    for (unsigned i = 0; i < 3; i++) bindImage(context, 8 + i, delta.temp1[i].view);
  } else if (stage == 1) {
    context->bindResourceSampler(0, g_sampler);
    for (unsigned i = 0; i < 3; i++) bindImage(context, 1 + i, delta.temp1[i].view);
    for (unsigned i = 0; i < 2; i++) bindImage(context, 4 + i, delta.temp2[i].view);
  } else if (stage == 2) {
    context->bindResourceSampler(0, g_sampler);
    for (unsigned i = 0; i < 2; i++) bindImage(context, 1 + i, delta.temp2[i].view);
    bindImage(context, 3, delta.temp1[0].view);
    bindImage(context, 4, delta.temp1[1].view);
  } else if (stage == 3) {
    context->bindResourceSampler(0, g_sampler);
    bindImage(context, 1, delta.temp1[0].view);
    bindImage(context, 2, delta.temp1[1].view);
    for (unsigned i = 0; i < 2; i++) bindImage(context, 3 + i, delta.temp2[i].view);
  } else if (stage == 4) {
    bindBuffer(context, 0, delta.constants[slot]);
    context->bindResourceSampler(1, g_sampler);
    context->bindResourceSampler(2, g_edgeSampler);
    for (unsigned i = 0; i < 2; i++) bindImage(context, 3 + i, delta.temp2[i].view);
    bindImage(context, 5, previousGamma);
    bindImage(context, 6, lsfgBetaOutputView(level));
    bindImage(context, 7, delta.output1.view);
  } else if (stage == 5) {
    bindBuffer(context, 0, delta.constants[slot]);
    context->bindResourceSampler(1, g_whiteBorderSampler);
    context->bindResourceSampler(2, g_edgeSampler);
    bindImage(context, 3, lsfgAlphaHistoryView(level, previousHistory, 0));
    bindImage(context, 4, lsfgAlphaHistoryView(level, previousHistory, 1));
    bindImage(context, 5, lsfgAlphaHistoryView(level, history, 0));
    bindImage(context, 6, lsfgAlphaHistoryView(level, history, 1));
    bindImage(context, 7, previousGamma);
    bindImage(context, 8, previous1);
    bindImage(context, 9, delta.temp2[0].view);
  } else if (stage == 6) {
    context->bindResourceSampler(0, g_sampler);
    bindImage(context, 1, delta.temp2[0].view);
    bindImage(context, 2, delta.temp1[0].view);
  } else if (stage == 7) {
    context->bindResourceSampler(0, g_sampler);
    bindImage(context, 1, delta.temp1[0].view);
    bindImage(context, 2, delta.temp2[0].view);
  } else if (stage == 8) {
    context->bindResourceSampler(0, g_sampler);
    bindImage(context, 1, delta.temp2[0].view);
    bindImage(context, 2, delta.temp1[0].view);
  } else {
    bindBuffer(context, 0, delta.constants[slot]);
    context->bindResourceSampler(1, g_sampler);
    context->bindResourceSampler(2, g_edgeSampler);
    bindImage(context, 3, delta.temp1[0].view);
    bindImage(context, 4, previous2);
    bindImage(context, 5, delta.output2.view);
  }

  context->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, g_deltaShaders[stage]);
  const VkExtent2D extent = delta.temp1[0].extent;
  context->dispatch((extent.width + 7u) >> 3, (extent.height + 7u) >> 3, 1);
}

} // anonymous namespace


Rc<DxvkImageView> getLsfgGenerationTarget(
        DxvkDevice* device, uint32_t width, uint32_t height,
        VkFormat format, uint32_t generation) {
  if (!device || !width || !height || format == VK_FORMAT_UNDEFINED)
    return Rc<DxvkImageView>();

  if (g_targetDevice != device || g_targetGeneration != generation
      || g_targetExtent.width != width || g_targetExtent.height != height
      || g_targetFormat != format) {
    g_target = Image();
    g_targetDevice = device;
    g_targetGeneration = generation;
    g_targetExtent = { width, height };
    g_targetFormat = format;

    const VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT
      | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    const VkPipelineStageFlags stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
      | VK_PIPELINE_STAGE_TRANSFER_BIT;
    const VkAccessFlags access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT
      | VK_ACCESS_TRANSFER_READ_BIT;
    if (!createImage(device, { width, height }, format, g_target, usage, stages, access)) {
      g_target = Image();
      return Rc<DxvkImageView>();
    }
  }
  return g_target.view;
}


bool dispatchLsfgGeneration(
        DxvkContext* context, DxvkDevice* device, const char* cacheDir,
        const Rc<DxvkImageView>* frameViews, const Rc<DxvkImageView>& targetView,
        uint32_t width, uint32_t height, uint32_t frameIndex,
        uint32_t generationCount, uint32_t generationIndex,
        float flowScale, uint32_t deviceGeneration) {
  if (!context || !device || !cacheDir || !frameViews || !targetView
      || !frameViews[0] || !frameViews[1]
      || generationCount == 0 || generationCount > 3
      || generationIndex >= generationCount
      || !lsfgSharedPassesReady()) return false;

  if (!ensureResources(device, cacheDir, { std::max(1u, uint32_t(float(width) * flowScale)),
                                           std::max(1u, uint32_t(float(height) * flowScale)) },
                       flowScale, deviceGeneration))
    return false;

  const uint32_t slot = generationSlot(generationCount, generationIndex);
  if (slot >= GenerationSlots) return false;

  // DXVK tracks image hazards and inserts the required compute barriers. Keep
  // gamma/delta interleaved at each level to preserve WinNative's paired order.
  for (uint32_t i = 0; i < MipLevels; i++) {
    if (i < FirstDeltaLevel) {
      for (uint32_t stage = 0; stage < 5; stage++)
        dispatchGammaStep(context, frameIndex, slot, i, stage);
      continue;
    }

    const uint32_t deltaIndex = i - FirstDeltaLevel;
    for (uint32_t stage = 0; stage < 10; stage++) {
      if (stage < 5)
        dispatchGammaStep(context, frameIndex, slot, i, stage);
      dispatchDeltaStep(context, frameIndex, slot, deltaIndex, stage);
    }
  }

  bindBuffer(context, 0, g_generateConstants[slot]);
  context->bindResourceSampler(1, g_sampler);
  context->bindResourceSampler(2, g_edgeSampler);
  const uint32_t current = frameIndex % 2u;
  bindImage(context, 3, frameViews[1u - current]);
  bindImage(context, 4, frameViews[current]);
  bindImage(context, 5, g_gamma[MipLevels - 1u].output.view);
  bindImage(context, 6, g_delta[DeltaInstances - 1u].output1.view);
  bindImage(context, 7, g_delta[DeltaInstances - 1u].output2.view);
  bindImage(context, 8, targetView);
  context->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, g_generateShader);
  context->dispatch((width + 15u) >> 4, (height + 15u) >> 4, 1);
  return true;
}

} // namespace dxvk
