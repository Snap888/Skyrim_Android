#include "lsfg_dxvk_shader.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <utility>
#include <unordered_map>

#include "dxvk_device.h"
#include "dxvk_limits.h"
#include "dxvk_pipelayout.h"
#include "spirv_code_buffer.h"

namespace dxvk {

namespace {

struct TypeInfo {
  uint32_t op = 0;
  uint32_t a = 0;
  uint32_t b = 0;
  uint32_t c = 0;
  uint32_t d = 0;
  std::vector<uint32_t> members;
};

struct VariableInfo {
  uint32_t type = 0;
  uint32_t id = 0;
  uint32_t storage = 0;
};

struct MemberDecoration {
  uint32_t offset = UINT32_MAX;
  uint32_t matrixStride = 0;
  bool rowMajor = false;
};

static uint64_t memberKey(uint32_t type, uint32_t member) {
  return (uint64_t(type) << 32) | member;
}

static void setError(std::string& error, const char* message, uint32_t id = 0) {
  error = message;
  if (id) error += " (id " + std::to_string(id) + ")";
}

static VkImageViewType imageViewType(uint32_t dim, uint32_t arrayed) {
  switch (dim) {
    case 0: return arrayed ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
    case 1: return arrayed ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    case 2: return VK_IMAGE_VIEW_TYPE_3D;
    case 3: return arrayed ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
    default: return VK_IMAGE_VIEW_TYPE_MAX_ENUM;
  }
}

} // anonymous namespace


Rc<DxvkShader> createLsfgComputeShader(
        DxvkDevice*            device,
  const std::vector<uint32_t>& code,
        std::string&           error) {
  error.clear();
  if (!device || code.size() < 5 || code[0] != 0x07230203u) {
    setError(error, "invalid device or SPIR-V module");
    return Rc<DxvkShader>();
  }

  std::unordered_map<uint32_t, TypeInfo> types;
  std::unordered_map<uint32_t, uint32_t> pointerPointee;
  std::unordered_map<uint32_t, uint32_t> descriptorSet;
  std::unordered_map<uint32_t, uint32_t> descriptorBinding;
  std::unordered_map<uint32_t, uint32_t> decorations;
  std::unordered_map<uint32_t, uint32_t> arrayStride;
  std::unordered_map<uint32_t, uint64_t> constants;
  std::unordered_map<uint64_t, MemberDecoration> memberDecorations;
  std::vector<VariableInfo> variables;
  bool computeEntry = false;

  for (size_t i = 5; i < code.size();) {
    const uint32_t word = code[i];
    const uint32_t op = word & 0xffffu;
    const uint32_t len = word >> 16;
    if (!len || i + len > code.size()) {
      setError(error, "malformed SPIR-V instruction", uint32_t(i));
      return Rc<DxvkShader>();
    }

    const uint32_t* a = code.data() + i;
    switch (op) {
      case 15: // OpEntryPoint
        if (len >= 4 && a[1] == 5)
          computeEntry = true;
        break;

      case 20: case 21: case 22: case 23: case 24:
      case 25: case 26: case 27: case 28: case 29:
      case 30: case 32: {
        if (len < 2) break;
        TypeInfo t;
        t.op = op;
        if (op == 21 || op == 22) {
          if (len >= 3) t.a = a[2];
        } else if (op == 23 || op == 24 || op == 27) {
          if (len >= 4) { t.a = a[2]; t.b = a[3]; }
        } else if (op == 25) {
          if (len >= 9) {
            t.a = a[2];
            t.b = a[3];
            t.c = a[4];
            t.d = a[5] | (a[6] << 1) | (a[7] << 2);
            t.members.push_back(a[8]);
          }
        } else if (op == 28) {
          if (len >= 4) { t.a = a[2]; t.b = a[3]; }
        } else if (op == 29) {
          if (len >= 3) t.a = a[2];
        } else if (op == 30) {
          for (uint32_t n = 2; n < len; n++) t.members.push_back(a[n]);
        } else if (op == 32) {
          if (len >= 4) pointerPointee[a[1]] = a[3];
        }
        types[a[1]] = std::move(t);
        break;
      }

      case 43: // OpConstant
        if (len >= 4) {
          uint64_t value = a[3];
          if (len >= 5) value |= uint64_t(a[4]) << 32;
          constants[a[2]] = value;
        }
        break;

      case 59: // OpVariable
        if (len >= 4)
          variables.push_back({ a[1], a[2], a[3] });
        break;

      case 71: // OpDecorate
        if (len >= 3) {
          if (a[2] == 2) decorations[a[1]] |= 1u; // Block
          if (a[2] == 3) decorations[a[1]] |= 2u; // BufferBlock
          if (a[2] == 33 && len >= 4) descriptorBinding[a[1]] = a[3];
          if (a[2] == 34 && len >= 4) descriptorSet[a[1]] = a[3];
          if (a[2] == 6 && len >= 4) arrayStride[a[1]] = a[3];
        }
        break;

      case 72: // OpMemberDecorate
        if (len >= 4) {
          auto& md = memberDecorations[memberKey(a[1], a[2])];
          if (a[3] == 35 && len >= 5) md.offset = a[4];
          if (a[3] == 7 && len >= 5) md.matrixStride = a[4];
          if (a[3] == 4) md.rowMajor = true;
          if (a[3] == 5) md.rowMajor = false;
        }
        break;
    }

    i += len;
  }

  if (!computeEntry) {
    setError(error, "SPIR-V module has no GLCompute entry point");
    return Rc<DxvkShader>();
  }

  // LSFG's host-side binder uses a compact, declaration-order-independent
  // slot convention: UBOs, samplers, sampled images, then storage images.
  // DXBC-translated modules encode the same resources at bindings such as
  // 800/816/832/1088, while embedded SPIR-V uses 0/16/32/48. Compact the
  // sorted binding numbers and rewrite OpDecorate Binding in the module so
  // the Vulkan shader interface and DXVK resource-slot metadata stay aligned.
  std::vector<uint32_t> orderedBindings;
  orderedBindings.reserve(descriptorBinding.size());
  for (const auto& entry : descriptorBinding)
    orderedBindings.push_back(entry.second);
  std::sort(orderedBindings.begin(), orderedBindings.end());
  orderedBindings.erase(
    std::unique(orderedBindings.begin(), orderedBindings.end()),
    orderedBindings.end());

  if (orderedBindings.size() > MaxNumResourceSlots) {
    setError(error, "LSFG shader has too many descriptor bindings");
    return Rc<DxvkShader>();
  }

  std::unordered_map<uint32_t, uint32_t> normalizedBindings;
  for (uint32_t i = 0; i < orderedBindings.size(); i++)
    normalizedBindings.emplace(orderedBindings[i], i);

  std::vector<uint32_t> normalizedCode = code;
  for (size_t i = 5; i < normalizedCode.size();) {
    const uint32_t word = normalizedCode[i];
    const uint32_t op = word & 0xffffu;
    const uint32_t len = word >> 16;
    if (!len || i + len > normalizedCode.size()) {
      setError(error, "malformed SPIR-V while normalizing descriptor bindings", uint32_t(i));
      return Rc<DxvkShader>();
    }

    if (op == 71 && len >= 4 && normalizedCode[i + 2] == 33) {
      auto binding = descriptorBinding.find(normalizedCode[i + 1]);
      if (binding != descriptorBinding.end())
        normalizedCode[i + 3] = normalizedBindings.at(binding->second);
    }
    i += len;
  }

  // DxvkContext sources push-constant bytes from its D3D state. LSFG has
  // no D3D constant-state provider, so reject these modules instead of
  // dispatching with unrelated or uninitialized push-constant data.
  for (const auto& v : variables) {
    if (v.storage == 9) {
      setError(error, "LSFG compute shader declares unsupported push constants", v.id);
      return Rc<DxvkShader>();
    }
  }

  std::function<uint64_t(uint32_t, uint32_t, bool, unsigned)> typeSize;
  typeSize = [&](uint32_t id, uint32_t matrixStride, bool rowMajor, unsigned depth) -> uint64_t {
    if (depth > 32) return 0;
    auto it = types.find(id);
    if (it == types.end()) return 0;
    const TypeInfo& t = it->second;
    switch (t.op) {
      case 20: return 4;
      case 21: case 22: return t.a / 8;
      case 23: return typeSize(t.a, 0, false, depth + 1) * t.b;
      case 24: {
        auto col = types.find(t.a);
        uint64_t rows = col != types.end() && col->second.op == 23 ? col->second.b : 0;
        uint64_t major = rowMajor ? rows : t.b;
        return matrixStride ? uint64_t(matrixStride) * major
                            : typeSize(t.a, 0, false, depth + 1) * t.b;
      }
      case 28: {
        auto n = constants.find(t.b);
        if (n == constants.end() || n->second > (1u << 20)) return 0;
        auto stride = arrayStride.find(id);
        uint64_t elem = typeSize(t.a, matrixStride, rowMajor, depth + 1);
        return (stride != arrayStride.end() ? stride->second : elem) * n->second;
      }
      case 30: {
        uint64_t end = 0;
        for (uint32_t m = 0; m < t.members.size(); m++) {
          auto md = memberDecorations.find(memberKey(id, m));
          if (md == memberDecorations.end() || md->second.offset == UINT32_MAX) return 0;
          uint64_t size = typeSize(t.members[m], md->second.matrixStride, md->second.rowMajor, depth + 1);
          if (!size) return 0;
          end = std::max(end, uint64_t(md->second.offset) + size);
        }
        return end;
      }
      default: return 0;
    }
  };

  DxvkInterfaceSlots iface = { };
  for (const auto& v : variables) {
    if (v.storage != 9) continue;
    auto p = pointerPointee.find(v.type);
    if (p == pointerPointee.end()) {
      setError(error, "push-constant variable has no pointer type", v.id);
      return Rc<DxvkShader>();
    }
    uint64_t size = typeSize(p->second, 0, false, 0);
    if (!size || size > MaxPushConstantSize || (size & 3)) {
      setError(error, "unsupported or oversized push-constant layout", v.id);
      return Rc<DxvkShader>();
    }
    iface.pushConstOffset = 0;
    iface.pushConstSize = uint32_t(size);
  }

  std::vector<DxvkResourceSlot> slots;
  for (const auto& v : variables) {
    if (!descriptorBinding.count(v.id)) continue;
    uint32_t set = descriptorSet.count(v.id) ? descriptorSet[v.id] : 0;
    if (set != 0) {
      setError(error, "DXVK LSFG adapter supports descriptor set zero only", v.id);
      return Rc<DxvkShader>();
    }

    auto ptr = pointerPointee.find(v.type);
    if (ptr == pointerPointee.end()) {
      setError(error, "descriptor variable has no pointer type", v.id);
      return Rc<DxvkShader>();
    }

    uint32_t typeId = ptr->second;
    auto typeIt = types.find(typeId);
    if (typeIt == types.end()) {
      setError(error, "descriptor points to an unknown SPIR-V type", v.id);
      return Rc<DxvkShader>();
    }
    if (typeIt->second.op == 28 || typeIt->second.op == 29) {
      setError(error, "DXVK LSFG adapter does not support descriptor arrays", v.id);
      return Rc<DxvkShader>();
    }

    const uint32_t binding = normalizedBindings.at(descriptorBinding[v.id]);

    VkDescriptorType descriptorType = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_MAX_ENUM;
    VkAccessFlags access = VK_ACCESS_SHADER_READ_BIT;

    if (v.storage == 12) {
      descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      access |= VK_ACCESS_SHADER_WRITE_BIT;
    } else if (v.storage == 2) {
      if (decorations[typeId] & 2u) {
        descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        access |= VK_ACCESS_SHADER_WRITE_BIT;
      } else if (decorations[typeId] & 1u) {
        descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      } else {
        setError(error, "uniform block is missing Block decoration", v.id);
        return Rc<DxvkShader>();
      }
    } else if (v.storage == 0) {
      if (typeIt->second.op == 26) {
        descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
      } else if (typeIt->second.op == 27) {
        descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        auto image = types.find(typeIt->second.a);
        if (image == types.end() || image->second.op != 25) {
          setError(error, "combined sampler has no image type", v.id);
          return Rc<DxvkShader>();
        }
        viewType = imageViewType(image->second.b, image->second.d & 1u);
      } else if (typeIt->second.op == 25) {
        const TypeInfo& image = typeIt->second;
        uint32_t sampled = (image.d >> 2) & 3u;
        if (sampled == 1) {
          descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
          viewType = imageViewType(image.b, image.d & 1u);
        } else if (sampled == 2) {
          descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
          viewType = imageViewType(image.b, image.d & 1u);
          access |= VK_ACCESS_SHADER_WRITE_BIT;
        } else {
          setError(error, "image descriptor has unknown sampled/storage mode", v.id);
          return Rc<DxvkShader>();
        }
      }
    }

    if (descriptorType == VK_DESCRIPTOR_TYPE_MAX_ENUM) {
      setError(error, "unsupported LSFG descriptor type", v.id);
      return Rc<DxvkShader>();
    }
    if ((descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
      || descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
      || descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
      && viewType == VK_IMAGE_VIEW_TYPE_MAX_ENUM) {
      setError(error, "unsupported image dimensionality", v.id);
      return Rc<DxvkShader>();
    }

    slots.push_back({ binding, descriptorType, viewType, access });
  }

  std::sort(slots.begin(), slots.end(), [](const DxvkResourceSlot& a, const DxvkResourceSlot& b) {
    return a.slot < b.slot;
  });
  for (size_t i = 1; i < slots.size(); i++) {
    if (slots[i - 1].slot == slots[i].slot) {
      setError(error, "duplicate descriptor binding", slots[i].slot);
      return Rc<DxvkShader>();
    }
  }

  SpirvCodeBuffer spirv(uint32_t(normalizedCode.size()), normalizedCode.data());
  Rc<DxvkShader> shader = device->createShader(
    VK_SHADER_STAGE_COMPUTE_BIT,
    uint32_t(slots.size()),
    slots.data(),
    iface,
    spirv);
  if (shader != nullptr)
    device->registerShader(shader);
  return shader;
}

} // namespace dxvk
