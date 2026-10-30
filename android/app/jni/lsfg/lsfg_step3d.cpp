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

extern "C" void lsfg_step3hb_set_phys(void *phys);
extern "C" void lsfg_step3hc_set_phys(void *phys);
extern "C" void lsfg_step3he_set_phys(void *phys);
extern "C" void lsfg_step3hf_set_phys(void *phys);
extern "C" void lsfg_step3hg_set_phys(void *phys);
extern "C" void lsfg_step3hh_set_phys(void *phys);
extern "C" void lsfg_step3hi_set_phys(void *phys);
extern "C" void lsfg_step3hj_set_phys(void *phys);
extern "C" void lsfg_step3d_set_phys_queue(void *phys, void *queue) {
    lsfg_step3hb_set_phys(phys);
    lsfg_step3hc_set_phys(phys);
    lsfg_step3he_set_phys(phys);
    lsfg_step3hf_set_phys(phys);
    lsfg_step3hg_set_phys(phys);
    lsfg_step3hh_set_phys(phys);
    lsfg_step3hi_set_phys(phys);
    lsfg_step3hj_set_phys(phys);
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
    if (c.size() < 5) return false;
    for (size_t i = 5; i < c.size();) {
        uint32_t w = c[i], op = w & 0xffff, len = w >> 16;
        if (!len) break;
        if (op == 15 && i + 1 < c.size() && c[i + 1] == 5) return true;
        i += len;
    }
    return false;
}

static void reflect_bindings(const std::vector<uint32_t> &c, std::vector<uint32_t> &out) {
    out.clear();
    std::unordered_map<uint32_t, uint32_t> id_to_binding;
    for (size_t i = 5; i < c.size();) {
        uint32_t w = c[i], op = w & 0xffff, len = w >> 16;
        if (!len) break;
        if (op == 71 && len >= 3 && i + 2 < c.size()) {
            id_to_binding[c[i + 1]] = c[i + 2];
        }
        i += len;
    }
    std::vector<uint32_t> binds;
    for (auto &kv : id_to_binding) binds.push_back(kv.second);
    std::sort(binds.begin(), binds.end());
    binds.erase(std::unique(binds.begin(), binds.end()), binds.end());
    out = binds;
}

static uint32_t find_mem(VkPhysicalDevice phys, uint32_t bits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props)
            return i;
    return 0;
}

static bool make_hist(VkDevice dev, uint32_t w, uint32_t h) {
    if (!g_phys || !w || !h) return false;
    for (int i = 0; i < 2; i++) {
        VkImageCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = { w, h, 1 };
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                  | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(dev, &ici, nullptr, &g_img[i]) != VK_SUCCESS) return false;
        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(dev, g_img[i], &mr);
        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = find_mem(g_phys, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(dev, &mai, nullptr, &g_mem[i]) != VK_SUCCESS) return false;
        if (vkBindImageMemory(dev, g_img[i], g_mem[i], 0) != VK_SUCCESS) return false;
        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = g_img[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.levelCount = 1;
        vci.subresourceRange.layerCount = 1;
        if (vkCreateImageView(dev, &vci, nullptr, &g_view[i]) != VK_SUCCESS) return false;
    }
    g_fb_w = w; g_fb_h = h; g_fb_ready = 1;
    return true;
}

extern "C" void lsfg_step3d_run(void *device, const char *cache_dir, uint32_t w, uint32_t h) {
    VkDevice dev = (VkDevice)device;
    if (!dev || !cache_dir || !cache_dir[0]) return;
    if (g_done) return;
    g_done = 1;
    lsfg_log("I", "=== LSFG step 3d: descriptor reflection + frame pair ===");
    if (w && h && !g_fb_ready) {
        if (make_hist(dev, w, h))
            lsfg_log("I", "step3d: frame pair %ux%u OK", w, h);
        else
            lsfg_log("W", "step3d: frame pair create failed");
    }
    char man[600];
    snprintf(man, sizeof man, "%s/manifest_spirv.txt", cache_dir);
    FILE *f = fopen(man, "r");
    if (!f) { lsfg_log("W", "step3d: no manifest_spirv.txt"); return; }
    char line[256];
    int compute = 0, with_b = 0, max_b = 0, nlog = 0;
    while (fgets(line, sizeof line, f) && compute < 40) {
        unsigned id = 0;
        if (sscanf(line, "%u", &id) != 1) continue;
        char path[700];
        snprintf(path, sizeof path, "%s/%u.spv", cache_dir, id);
        std::vector<uint32_t> code;
        if (!load_spv(path, code) || !is_compute(code)) continue;
        compute++;
        std::vector<uint32_t> binds;
        reflect_bindings(code, binds);
        if (!binds.empty()) { with_b++; if ((int)binds.size() > max_b) max_b = (int)binds.size(); }
        if (nlog < 8) {
            char buf[256];
            int pos = snprintf(buf, sizeof buf, "step3d: id=%u bindings:", id);
            for (uint32_t b : binds)
                if (pos < (int)sizeof(buf) - 16)
                    pos += snprintf(buf + pos, sizeof(buf) - pos, " set0:b%u", b);
            lsfg_log("I", "%s", buf);
            nlog++;
        }
    }
    fclose(f);
    lsfg_log("I", "step3d reflect: compute=%d with_bindings=%d max_bindings=%d", compute, with_b, max_b);
    lsfg_log("I", "step3d frames: ready=%d %ux%u phys=%p queue=%p",
             g_fb_ready, g_fb_w, g_fb_h, (void*)g_phys, (void*)g_queue);
}

extern "C" int lsfg_step3d_frames_ready(void) { return g_fb_ready; }
extern "C" void *lsfg_step3d_get_image(int idx) {
    if (!g_fb_ready || idx < 0 || idx > 1) return nullptr;
    return (void*)g_img[idx];
}
extern "C" unsigned lsfg_step3d_fb_width(void) { return g_fb_w; }
extern "C" unsigned lsfg_step3d_fb_height(void) { return g_fb_h; }
extern "C" void *lsfg_step3d_get_view(int idx) {
    if (!g_fb_ready || idx < 0 || idx > 1) return nullptr;
    return (void*)g_view[idx];
}
extern "C" void *lsfg_step3d_get_queue(void) { return (void*)g_queue; }
