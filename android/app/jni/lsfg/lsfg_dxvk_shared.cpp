#include "lsfg_dxvk_shared.h"

#include <algorithm>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "dxvk_buffer.h"
#include "dxvk_context.h"
#include "dxvk_device.h"
#include "dxvk_image.h"
#include "dxvk_limits.h"
#include "dxvk_sampler.h"
#include "lsfg_dxvk_shader.h"

namespace dxvk {

namespace {

constexpr unsigned AlphaLevels = 7;
constexpr unsigned HistorySlots = 3;
constexpr unsigned AlphaOutputs = 2;
constexpr unsigned BetaOutputs = 6;

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

struct FlowImage {
  Rc<DxvkImage> image;
  Rc<DxvkImageView> view;
  VkExtent2D extent = { 0, 0 };
};

static Rc<DxvkShader> g_alphaShaders[4];
static Rc<DxvkShader> g_betaShaders[5];
static Rc<DxvkSampler> g_sampler;
static Rc<DxvkSampler> g_whiteBorderSampler;
static Rc<DxvkBuffer> g_betaConstants;

static FlowImage g_alphaTemp1[AlphaLevels];
static FlowImage g_alphaTemp2[AlphaLevels];
static FlowImage g_alphaTemp3[AlphaLevels][AlphaOutputs];
static FlowImage g_alphaHistory[AlphaLevels][HistorySlots][AlphaOutputs];
static FlowImage g_betaTemp1[2];
static FlowImage g_betaTemp2[2];
static FlowImage g_betaOutputs[BetaOutputs];

static DxvkDevice* g_device = nullptr;
static uint32_t g_generation = UINT32_MAX;
static VkExtent2D g_extent = { 0, 0 };
static float g_scale = 0.0f;
static char g_cacheDir[512] = {};
static bool g_ready = false;
static bool g_errorLogged = false;

static void clearResources() {
  for (auto& shader : g_alphaShaders) shader = Rc<DxvkShader>();
  for (auto& shader : g_betaShaders) shader = Rc<DxvkShader>();
  g_sampler = Rc<DxvkSampler>();
  g_whiteBorderSampler = Rc<DxvkSampler>();
  g_betaConstants = Rc<DxvkBuffer>();

  for (unsigned level = 0; level < AlphaLevels; level++) {
    g_alphaTemp1[level] = FlowImage();
    g_alphaTemp2[level] = FlowImage();
    for (unsigned i = 0; i < AlphaOutputs; i++) {
      g_alphaTemp3[level][i] = FlowImage();
      for (unsigned history = 0; history < HistorySlots; history++)
        g_alphaHistory[level][history][i] = FlowImage();
    }
  }
  for (unsigned i = 0; i < 2; i++) {
    g_betaTemp1[i] = FlowImage();
    g_betaTemp2[i] = FlowImage();
  }
  for (auto& image : g_betaOutputs) image = FlowImage();
  g_ready = false;
}

static void resetResources() {
  clearResources();
  g_device = nullptr;
  g_generation = UINT32_MAX;
  g_extent = { 0, 0 };
  g_scale = 0.0f;
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

static Rc<DxvkSampler> createSampler(DxvkDevice* device, bool whiteBorder) {
  DxvkSamplerCreateInfo info{};
  info.magFilter = VK_FILTER_LINEAR;
  info.minFilter = VK_FILTER_LINEAR;
  info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  info.mipmapLodBias = 0.0f;
  info.mipmapLodMin = 0.0f;
  info.mipmapLodMax = 0.0f;
  info.useAnisotropy = VK_FALSE;
  info.maxAnisotropy = 1.0f;
  info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
  info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
  info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
  info.compareToDepth = VK_FALSE;
  info.compareOp = VK_COMPARE_OP_NEVER;
  const float border = whiteBorder ? 1.0f : 0.0f;
  for (unsigned i = 0; i < 4; i++) info.borderColor.float32[i] = border;
  info.usePixelCoord = VK_FALSE;
  return device->createSampler(info);
}

static bool createImage(DxvkDevice* device, VkExtent2D extent,
                        VkFormat format, FlowImage& out) {
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
  imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  imageInfo.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
  imageInfo.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  imageInfo.layout = VK_IMAGE_LAYOUT_GENERAL;
  imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  imageInfo.shared = VK_FALSE;
  out.image = device->createImage(imageInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (out.image == nullptr) return false;

  DxvkImageViewCreateInfo viewInfo{};
  viewInfo.type = VK_IMAGE_VIEW_TYPE_2D;
  viewInfo.format = format;
  viewInfo.usage = imageInfo.usage;
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

static bool ensureResources(DxvkDevice* device, const char* cacheDir,
                            VkExtent2D flowExtent, float scale, uint32_t generation) {
  if (g_device != device || g_generation != generation
      || g_extent.width != flowExtent.width || g_extent.height != flowExtent.height
      || g_scale != scale
      || strncmp(g_cacheDir, cacheDir ? cacheDir : "", sizeof(g_cacheDir)) != 0) {
    resetResources();
    g_device = device;
    g_generation = generation;
    g_extent = flowExtent;
    g_scale = scale;
    snprintf(g_cacheDir, sizeof(g_cacheDir), "%s", cacheDir ? cacheDir : "");
  }
  if (g_ready) return true;
  if (!device || !cacheDir || !cacheDir[0] || scale <= 0.0f || scale > 1.0f)
    return false;

  std::string error;
  const uint32_t alphaIds[4] = { 290, 291, 292, 293 };
  const uint32_t betaIds[5] = { 298, 299, 300, 301, 302 };
  for (unsigned i = 0; i < 4 && error.empty(); i++) {
    g_alphaShaders[i] = loadShader(device, cacheDir, alphaIds[i], error);
    if (g_alphaShaders[i] == nullptr && error.empty()) error = "alpha shader creation failed";
  }
  for (unsigned i = 0; i < 5 && error.empty(); i++) {
    g_betaShaders[i] = loadShader(device, cacheDir, betaIds[i], error);
    if (g_betaShaders[i] == nullptr && error.empty()) error = "beta shader creation failed";
  }

  if (error.empty()) {
    g_sampler = createSampler(device, false);
    g_whiteBorderSampler = createSampler(device, true);
    if (g_sampler == nullptr || g_whiteBorderSampler == nullptr)
      error = "sampler creation failed";
  }

  if (error.empty()) {
    DxvkBufferCreateInfo bufferInfo{};
    bufferInfo.size = sizeof(LsfgConstants);
    bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    bufferInfo.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    bufferInfo.access = VK_ACCESS_SHADER_READ_BIT;
    g_betaConstants = device->createBuffer(
      bufferInfo, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (g_betaConstants == nullptr || !g_betaConstants->mapPtr(0)) {
      error = "beta uniform buffer allocation failed";
    } else {
      LsfgConstants constants{};
      constants.resolutionInvScale = 1.0f / scale;
      constants.timestamp = 0.5f;
      constants.uiThreshold = 0.5f;
      memcpy(g_betaConstants->mapPtr(0), &constants, sizeof(constants));
    }
  }

  for (unsigned level = 0; level < AlphaLevels && error.empty(); level++) {
    const VkExtent2D input = {
      std::max(1u, flowExtent.width >> level),
      std::max(1u, flowExtent.height >> level),
    };
    const VkExtent2D half = { (input.width + 1u) >> 1, (input.height + 1u) >> 1 };
    const VkExtent2D quarter = { (half.width + 1u) >> 1, (half.height + 1u) >> 1 };

    if (!createImage(device, half, VK_FORMAT_R8G8B8A8_UNORM, g_alphaTemp1[level])
        || !createImage(device, half, VK_FORMAT_R8G8B8A8_UNORM, g_alphaTemp2[level])) {
      error = "alpha intermediate image allocation failed";
      break;
    }
    for (unsigned i = 0; i < AlphaOutputs && error.empty(); i++) {
      if (!createImage(device, quarter, VK_FORMAT_R8G8B8A8_UNORM, g_alphaTemp3[level][i])) {
        error = "alpha quarter-resolution image allocation failed";
        break;
      }
      for (unsigned history = 0; history < HistorySlots; history++) {
        if (!createImage(device, quarter, VK_FORMAT_R8G8B8A8_UNORM,
                         g_alphaHistory[level][history][i])) {
          error = "alpha history image allocation failed";
          break;
        }
      }
    }
  }

  if (error.empty()) {
    const VkExtent2D extent = g_alphaHistory[0][0][0].extent;
    for (unsigned i = 0; i < 2 && error.empty(); i++) {
      if (!createImage(device, extent, VK_FORMAT_R8G8B8A8_UNORM, g_betaTemp1[i])
          || !createImage(device, extent, VK_FORMAT_R8G8B8A8_UNORM, g_betaTemp2[i]))
        error = "beta intermediate image allocation failed";
    }
    for (unsigned i = 0; i < BetaOutputs && error.empty(); i++) {
      VkExtent2D levelExtent = {
        std::max(1u, extent.width >> i),
        std::max(1u, extent.height >> i),
      };
      if (!createImage(device, levelExtent, VK_FORMAT_R8_UNORM, g_betaOutputs[i]))
        error = "beta flow pyramid allocation failed";
    }
  }

  if (!error.empty()) {
    if (!g_errorLogged) {
      lsfg_log("E", "step3i: alpha/beta initialization failed: %s", error.c_str());
      g_errorLogged = true;
    }
    clearResources();
    return false;
  }

  g_ready = true;
  lsfg_log("I", "step3i: alpha/beta DXVK resources initialized: 7 levels, 3 history slots");
  return true;
}

static void bindImage(DxvkContext* context, uint32_t slot, const FlowImage& image) {
  context->bindResourceView(slot, image.view, nullptr);
}

static void dispatchAlpha(DxvkContext* context, const Rc<DxvkImageView>* flowViews,
                          uint32_t history) {
  for (unsigned stage = 0; stage < 4; stage++) {
    for (unsigned level = 0; level < AlphaLevels; level++) {
      context->bindResourceSampler(0, g_sampler);
      if (stage == 0) {
        context->bindResourceView(1, flowViews[level], nullptr);
        bindImage(context, 2, g_alphaTemp1[level]);
      } else if (stage == 1) {
        bindImage(context, 1, g_alphaTemp1[level]);
        bindImage(context, 2, g_alphaTemp2[level]);
      } else if (stage == 2) {
        bindImage(context, 1, g_alphaTemp2[level]);
        bindImage(context, 2, g_alphaTemp3[level][0]);
        bindImage(context, 3, g_alphaTemp3[level][1]);
      } else {
        bindImage(context, 1, g_alphaTemp3[level][0]);
        bindImage(context, 2, g_alphaTemp3[level][1]);
        bindImage(context, 3, g_alphaHistory[level][history][0]);
        bindImage(context, 4, g_alphaHistory[level][history][1]);
      }

      context->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, g_alphaShaders[stage]);
      const VkExtent2D extent = stage < 2
        ? g_alphaTemp1[level].extent : g_alphaTemp3[level][0].extent;
      context->dispatch((extent.width + 7u) >> 3, (extent.height + 7u) >> 3, 1);
    }
  }
}

static void dispatchBeta(DxvkContext* context, uint32_t history) {
  const VkExtent2D extent = g_betaTemp1[0].extent;
  const uint32_t groupsX = (extent.width + 7u) >> 3;
  const uint32_t groupsY = (extent.height + 7u) >> 3;

  context->bindResourceSampler(0, g_whiteBorderSampler);
  uint32_t binding = 1;
  for (unsigned order = 0; order < HistorySlots; order++) {
    const unsigned source = (history + order + 1u) % HistorySlots;
    for (unsigned i = 0; i < AlphaOutputs; i++)
      bindImage(context, binding++, g_alphaHistory[0][source][i]);
  }
  for (unsigned i = 0; i < 2; i++)
    bindImage(context, binding++, g_betaTemp1[i]);
  context->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, g_betaShaders[0]);
  context->dispatch(groupsX, groupsY, 1);

  for (unsigned stage = 1; stage < 4; stage++) {
    context->bindResourceSampler(0, g_sampler);
    const FlowImage* input = stage == 1 ? g_betaTemp1
                            : stage == 2 ? g_betaTemp2 : g_betaTemp1;
    const FlowImage* output = stage == 1 ? g_betaTemp2
                             : stage == 2 ? g_betaTemp1 : g_betaTemp2;
    for (unsigned i = 0; i < 2; i++) bindImage(context, 1 + i, input[i]);
    for (unsigned i = 0; i < 2; i++) bindImage(context, 3 + i, output[i]);
    context->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, g_betaShaders[stage]);
    context->dispatch(groupsX, groupsY, 1);
  }

  context->bindResourceBuffer(0, DxvkBufferSlice(g_betaConstants));
  context->bindResourceSampler(1, g_sampler);
  for (unsigned i = 0; i < 2; i++) bindImage(context, 2 + i, g_betaTemp2[i]);
  for (unsigned i = 0; i < BetaOutputs; i++) bindImage(context, 4 + i, g_betaOutputs[i]);
  context->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, g_betaShaders[4]);
  context->dispatch((extent.width + 31u) >> 5, (extent.height + 31u) >> 5, 1);
}

} // anonymous namespace


bool dispatchLsfgSharedPasses(
        DxvkContext*               context,
        DxvkDevice*                device,
        const char*                cacheDir,
  const Rc<DxvkImageView>*         flowViews,
        VkExtent2D                 flowExtent,
        uint32_t                   frameIndex,
        float                      flowScale,
        uint32_t                   generation) {
  if (!context || !device || !flowViews) return false;
  if (!ensureResources(device, cacheDir, flowExtent, flowScale, generation))
    return false;

  const uint32_t history = frameIndex % HistorySlots;
  dispatchAlpha(context, flowViews, history);
  dispatchBeta(context, history);
  return true;
}

} // namespace dxvk
