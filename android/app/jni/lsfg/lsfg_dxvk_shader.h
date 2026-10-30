#pragma once

#include <string>
#include <vector>

#include "dxvk_shader.h"

namespace dxvk {

class DxvkDevice;

/**
 * Creates a compute shader through DXVK's shader/resource-slot path.
 *
 * LSFG SPIR-V currently uses Vulkan descriptor set zero. DXVK remaps
 * descriptor Binding decorations to its own compact descriptor layout,
 * so descriptor arrays and non-zero descriptor sets are rejected rather
 * than silently producing an incompatible pipeline layout.
 */
Rc<DxvkShader> createLsfgComputeShader(
        DxvkDevice*                 device,
  const std::vector<uint32_t>&      code,
        std::string&                error);

}
