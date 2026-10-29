#include "lsfg_log.h"
#include <stdio.h>
#include <string.h>
#include <vector>
#include <unordered_map>
#include <algorithm>

#ifdef __ANDROID__
#include <vulkan/vulkan.h>
#else
#include <vulkan/vulkan_core.h>
#endif

static VkPhysicalDevice g_phys = VK_NULL_HANDLE;
static VkQueue g_queue = VK_NULL_HANDLE;
static int g_done;
static uint32_t g_fb_w, g_fb_h;
static VkImage g_img[2];
static VkDeviceMemory g_mem[2];
static VkImageView g_view[2];
static int g_fb_ready;
static unsigned g_fb_generation;

extern "C" void lsfg_step3d_set_phys_queue(void *phys, void *queue) {
    g_phys = (VkPhysicalDevice)phys;
    g_queue = (VkQueue)queue;
}

static bool load_spv(const char *path, std::vector<uint32_t> &out) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 20 || (sz % 4)) { fclose(f); return false; }
    out.resize((size_t)sz / 4);
    bool ok = fread(out.data(), 1, (size_t)sz, f) == (size_t)sz;
    fclose(f);
    return ok && out[0] == 0x07230203u;
}

static bool is_compute(const std::vector<uint32_t> &c) {
    for (size_t i = 5; i < c.size();) {
        uint32_t w = c[i], op = w & 0xffff, len = w >> 16;
        if (!len) break;
        if (op == 15 && i + 1 < c.size() && c[i + 1] == 5) return true;
        i += len;
    }
    return false;
}

struct RefBinding {
    uint32_t set, binding;
    VkDescriptorType type;
    uint32_t count;
};

static bool reflect(const std::vector<uint32_t> &c, std::vector<RefBinding> &out) {
    out.clear();
    std::unordered_map<uint32_t, uint32_t> id_set, id_bind, storage;
    std::unordered_map<uint32_t, uint32_t> pointer_pointee, pointer_storage;
    std::unordered_map<uint32_t, uint32_t> type_opcode, image_sampled, array_element, array_length_id;
    std::unordered_map<uint32_t, uint64_t> constants;
    std::unordered_map<uint32_t, uint32_t> decorations;
    struct Var { uint32_t type, id, storage; };
    std::vector<Var> vars;

    for (size_t i = 5; i < c.size();) {
        uint32_t w = c[i], op = w & 0xffffu, len = w >> 16;
        if (!len || i + len > c.size()) return false;
        const uint32_t *a = c.data() + i;
        if (op == 71 && len >= 3) {
            uint32_t target = a[1], dec = a[2];
            if (dec == 34 && len >= 4) id_set[target] = a[3];
            if (dec == 33 && len >= 4) id_bind[target] = a[3];
            if (dec == 2) decorations[target] |= 1u;       // Block
            if (dec == 3) decorations[target] |= 2u;       // BufferBlock
        } else if (op == 32 && len >= 4) {                // OpTypePointer
            type_opcode[a[1]] = op;
            pointer_storage[a[1]] = a[2];
            pointer_pointee[a[1]] = a[3];
        } else if ((op >= 25 && op <= 30) && len >= 2) {  // image/sampler/struct/array types
            type_opcode[a[1]] = op;
            if (op == 25 && len >= 8) image_sampled[a[1]] = a[7];
            if (op == 28 && len >= 4) { array_element[a[1]] = a[2]; array_length_id[a[1]] = a[3]; }
        } else if (op == 43 && len >= 4) {                // OpConstant
            uint64_t value = a[3];
            if (len >= 5) value |= (uint64_t)a[4] << 32;
            constants[a[2]] = value;
        } else if (op == 59 && len >= 4) {                // OpVariable
            vars.push_back({a[1], a[2], a[3]});
        }
        i += len;
    }

    for (const Var &v : vars) {
        if (!id_bind.count(v.id)) continue;
        uint32_t set = id_set.count(v.id) ? id_set[v.id] : 0;
        uint32_t binding = id_bind[v.id];
        if (set > 7 || binding > 63) return false;
        auto pit = pointer_pointee.find(v.type);
        if (pit == pointer_pointee.end()) return false;
        uint32_t tid = pit->second, count = 1;
        while (type_opcode.count(tid) && type_opcode[tid] == 28) {
            uint32_t len_id = array_length_id[tid];
            if (!constants.count(len_id) || constants[len_id] == 0 || constants[len_id] > 1024) return false;
            count *= (uint32_t)constants[len_id];
            tid = array_element[tid];
        }

        VkDescriptorType dtype;
        if (v.storage == 12) { // StorageBuffer
            dtype = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        } else if (v.storage == 2) { // Uniform: distinguish Block and legacy BufferBlock
            if (decorations[tid] & 2u) dtype = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            else if (decorations[tid] & 1u) dtype = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            else return false;
        } else if (v.storage == 0) { // UniformConstant
            uint32_t op = type_opcode.count(tid) ? type_opcode[tid] : 0;
            if (op == 26) dtype = VK_DESCRIPTOR_TYPE_SAMPLER;
            else if (op == 27) dtype = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            else if (op == 25) {
                uint32_t sampled = image_sampled.count(tid) ? image_sampled[tid] : 1;
                dtype = sampled == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            } else return false;
        } else {
            continue; // push constants and non-descriptor variables
        }
        out.push_back({set, binding, dtype, count});
    }
    std::sort(out.begin(), out.end(), [](const RefBinding &a, const RefBinding &b) {
        return a.set != b.set ? a.set < b.set : a.binding < b.binding;
    });
    for (size_t i = 1; i < out.size(); i++)
        if (out[i-1].set == out[i].set && out[i-1].binding == out[i].binding) return false;
    return true;
}

static bool probe_reflected_layout(VkDevice dev, const std::vector<uint32_t> &code,
                                   unsigned id, int *binding_count) {
    std::vector<RefBinding> bindings;
    if (!reflect(code, bindings)) {
        lsfg_log("W", "step3d: id=%u descriptor reflection unsupported/invalid; layout skipped", id);
        return false;
    }
    uint32_t max_set = 0;
    for (const auto &b : bindings) if (b.set > max_set) max_set = b.set;
    std::vector<VkDescriptorSetLayout> sets;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    bool ok = true;
    if (!bindings.empty()) {
        sets.resize((size_t)max_set + 1, VK_NULL_HANDLE);
        for (uint32_t set = 0; set <= max_set && ok; set++) {
            std::vector<VkDescriptorSetLayoutBinding> entries;
            for (const auto &b : bindings) if (b.set == set) {
                VkDescriptorSetLayoutBinding e{};
                e.binding = b.binding;
                e.descriptorType = b.type;
                e.descriptorCount = b.count;
                e.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
                entries.push_back(e);
            }
            VkDescriptorSetLayoutCreateInfo ci{};
            ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            ci.bindingCount = (uint32_t)entries.size();
            ci.pBindings = entries.empty() ? nullptr : entries.data();
            ok = vkCreateDescriptorSetLayout(dev, &ci, nullptr, &sets[set]) == VK_SUCCESS;
        }
    }
    if (ok) {
        VkPipelineLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        lci.setLayoutCount = (uint32_t)sets.size();
        lci.pSetLayouts = sets.empty() ? nullptr : sets.data();
        ok = vkCreatePipelineLayout(dev, &lci, nullptr, &pipeline_layout) == VK_SUCCESS;
    }
    if (ok) {
        VkShaderModuleCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        sci.codeSize = code.size() * sizeof(uint32_t);
        sci.pCode = code.data();
        VkShaderModule module = VK_NULL_HANDLE;
        ok = vkCreateShaderModule(dev, &sci, nullptr, &module) == VK_SUCCESS;
        if (ok) {
            VkComputePipelineCreateInfo pci{};
            pci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            pci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            pci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            pci.stage.module = module;
            pci.stage.pName = "main";
            pci.layout = pipeline_layout;
            VkPipeline pipeline = VK_NULL_HANDLE;
            ok = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pci, nullptr, &pipeline) == VK_SUCCESS;
            if (pipeline) vkDestroyPipeline(dev, pipeline, nullptr);
            vkDestroyShaderModule(dev, module, nullptr);
        }
    }
    if (pipeline_layout) vkDestroyPipelineLayout(dev, pipeline_layout, nullptr);
    for (auto layout : sets) if (layout) vkDestroyDescriptorSetLayout(dev, layout, nullptr);
    if (binding_count) *binding_count = (int)bindings.size();
    if (ok) {
        char buf[384];
        int n = snprintf(buf, sizeof buf, "step3d: id=%u reflected-layout OK bindings=%zu", id, bindings.size());
        for (const auto &b : bindings) {
            if (n >= (int)sizeof(buf)-1) break;
            n += snprintf(buf+n, sizeof(buf)-(size_t)n, " [s%u:b%u t%d x%u]",
                          b.set, b.binding, (int)b.type, b.count);
        }
        lsfg_log("I", "%s", buf);
    } else {
        lsfg_log("W", "step3d: id=%u reflected layout/pipeline probe failed", id);
    }
    return ok;
}

static uint32_t find_mem(uint32_t bits, VkMemoryPropertyFlags flags) {
    if (!g_phys) return UINT32_MAX;
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(g_phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    return UINT32_MAX;
}

static void destroy_fb(VkDevice dev) {
    ++g_fb_generation;
    for (int i = 0; i < 2; i++) {
        if (g_view[i]) { vkDestroyImageView(dev, g_view[i], nullptr); g_view[i] = VK_NULL_HANDLE; }
        if (g_img[i])  { vkDestroyImage(dev, g_img[i], nullptr); g_img[i] = VK_NULL_HANDLE; }
        if (g_mem[i])  { vkFreeMemory(dev, g_mem[i], nullptr); g_mem[i] = VK_NULL_HANDLE; }
    }
    g_fb_ready = 0;
}

static bool create_fb(VkDevice dev, uint32_t w, uint32_t h) {
    if (!dev || !g_phys || !w || !h) return false;
    if (g_fb_ready && g_fb_w == w && g_fb_h == h) return true;

    /*
     * History images are referenced by DXVK command lists recorded on the
     * graphics queue. On a resize, wait for those lists to finish before
     * destroying/replacing the externally-owned VkImages and their memory.
     * This is intentionally only on create/resize, never on the frame path.
     */
    if (g_fb_ready) {
        VkResult idle = vkDeviceWaitIdle(dev);
        if (idle != VK_SUCCESS) {
            lsfg_log("W", "step3d: resize aborted; vkDeviceWaitIdle failed %d", (int)idle);
            return false;
        }
        lsfg_log("I", "step3d: GPU idle before history resize %ux%u -> %ux%u",
                 g_fb_w, g_fb_h, w, h);
    }

    destroy_fb(dev);
    g_fb_w = w; g_fb_h = h;
    for (int i = 0; i < 2; i++) {
        VkImageCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = {w, h, 1};
        ici.mipLevels = 1; ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(dev, &ici, nullptr, &g_img[i]) != VK_SUCCESS) {
            destroy_fb(dev); return false;
        }
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(dev, g_img[i], &req);
        uint32_t mt = find_mem(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mt == UINT32_MAX) { destroy_fb(dev); return false; }
        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = mt;
        if (vkAllocateMemory(dev, &mai, nullptr, &g_mem[i]) != VK_SUCCESS ||
            vkBindImageMemory(dev, g_img[i], g_mem[i], 0) != VK_SUCCESS) {
            destroy_fb(dev); return false;
        }
        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = g_img[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.levelCount = 1;
        vci.subresourceRange.layerCount = 1;
        if (vkCreateImageView(dev, &vci, nullptr, &g_view[i]) != VK_SUCCESS) {
            destroy_fb(dev); return false;
        }
    }
    g_fb_ready = 1;
    lsfg_log("I", "step3d: frame pair %ux%u OK", w, h);
    return true;
}

extern "C" void lsfg_step3d_run(void *device, const char *cache_dir, uint32_t w, uint32_t h) {
    VkDevice dev = (VkDevice)device;
    if (!dev || !cache_dir || !cache_dir[0]) return;

    if (w && h && g_phys)
        create_fb(dev, w, h);

    if (g_done) return;

    char man[600];
    snprintf(man, sizeof man, "%s/manifest_spirv.txt", cache_dir);
    FILE *f = fopen(man, "r");
    if (!f) {
        static int missing_manifest_logged;
        if (!missing_manifest_logged) {
            missing_manifest_logged = 1;
            lsfg_log("W", "step3d: no manifest yet at %s; will retry", man);
        }
        return;
    }

    g_done = 1;
    lsfg_log("I", "=== LSFG step 3d: descriptor reflection + frame pair ===");

    int manifest_entries = 0;
    int cn = 0, wb = 0, maxb = 0, layout_ok = 0, layout_fail = 0;
    char line[256];
    /* Audit every manifest entry: shader IDs and variants are not guaranteed
       to be ordered, and a fixed sample can miss the actual LSFG pass set. */
    while (fgets(line, sizeof line, f)) {
        unsigned id = 0;
        if (sscanf(line, "%u", &id) != 1) continue;
        manifest_entries++;
        if (strstr(line, " fail ") || strstr(line, " skip ")) continue;
        char path[700];
        snprintf(path, sizeof path, "%s/%u.spv", cache_dir, id);
        std::vector<uint32_t> code;
        if (!load_spv(path, code) || !is_compute(code)) continue;
        cn++;
        std::vector<RefBinding> binds;
        bool reflected = reflect(code, binds);
        if (reflected && !binds.empty()) {
            wb++;
            if ((int)binds.size() > maxb) maxb = (int)binds.size();
        }
        int count = 0;
        if (probe_reflected_layout(dev, code, id, &count)) layout_ok++;
        else layout_fail++;
    }
    fclose(f);
    lsfg_log("I", "step3d manifest audit: entries=%d compute_spv=%d with_bindings=%d max_bindings=%d layout_pipeline_ok=%d failed=%d", manifest_entries, cn, wb, maxb, layout_ok, layout_fail);
    lsfg_log("I", "step3d frames: ready=%d %ux%u phys=%p queue=%p",
             g_fb_ready, g_fb_w, g_fb_h, (void*)g_phys, (void*)g_queue);
    lsfg_log("I", "step3d: layout/pipeline probes only; no descriptors bound and no dispatch submitted");
}

extern "C" int lsfg_step3d_frames_ready(void) { return g_fb_ready; }

extern "C" void *lsfg_step3d_get_image(int idx) {
    if (!g_fb_ready || idx < 0 || idx > 1) return nullptr;
    return (void*)g_img[idx];
}
extern "C" unsigned lsfg_step3d_fb_width(void) { return g_fb_w; }
extern "C" unsigned lsfg_step3d_fb_height(void) { return g_fb_h; }
extern "C" unsigned lsfg_step3d_fb_generation(void) { return g_fb_generation; }
