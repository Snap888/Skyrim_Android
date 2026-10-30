#pragma once

#include "dxvk_image.h"

namespace dxvk {

class DxvkContext;
class DxvkDevice;

/** Records WinNative-compatible alpha and beta compute passes into DXVK's active command list. */
bool dispatchLsfgSharedPasses(
        DxvkContext*                    context,
        DxvkDevice*                     device,
        const char*                     cacheDir,
  const Rc<DxvkImageView>*              flowViews,
        VkExtent2D                       flowExtent,
        uint32_t                         frameIndex,
        float                            flowScale,
        uint32_t                         generation);

bool lsfgSharedPassesReady();
const Rc<DxvkImageView>& lsfgAlphaHistoryView(uint32_t level, uint32_t history, uint32_t output);
const Rc<DxvkImageView>& lsfgBetaOutputView(uint32_t level);
VkExtent2D lsfgAlphaHistoryExtent(uint32_t level);

}
