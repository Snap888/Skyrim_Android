#pragma once

#include "dxvk_image.h"

namespace dxvk {

class DxvkContext;
class DxvkDevice;

Rc<DxvkImageView> getLsfgGenerationTarget(
        DxvkDevice* device,
        uint32_t width,
        uint32_t height,
        VkFormat format,
        uint32_t generation);

bool dispatchLsfgGeneration(
        DxvkContext* context,
        DxvkDevice* device,
        const char* cacheDir,
        const Rc<DxvkImageView>* frameViews,
        const Rc<DxvkImageView>& targetView,
        uint32_t width,
        uint32_t height,
        uint32_t frameIndex,
        uint32_t generationCount,
        uint32_t generationIndex,
        float flowScale,
        uint32_t deviceGeneration);

}
