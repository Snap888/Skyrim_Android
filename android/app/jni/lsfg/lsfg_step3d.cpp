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

static void reflect(const std::vector<uint32_t> &c, std::vector<std::pair<uint32_t,uint32_t>> &out) {
    out.clear();
    std::unordered_map<uint32_t, uint32_t> id_set, id_bind;
    for (size_t i = 5; i < c.size();) {
        uint32_t w = c[i], op = w & 0xffff, len = w >> 16;
        if (!len) break;
        if (op == 71 && len >= 4 && i + 3 < c.size()) {
            uint32_t tgt = c[i+1], dec = c[i+2], lit = c[i+3];
            if (dec == 34) id_set[tgt] = lit;
            if (dec == 33) id_bind[tgt] = lit;
        }
        i += len;
    }
    for (auto &kv : id_bind)
        out.push_back({id_set.count(kv.first) ? id_set[kv.first] : 0, kv.second});
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
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
    g_done = 1;

    lsfg_log("I", "=== LSFG step 3d: descriptor reflection + frame pair ===");

    char man[600];
    snprintf(man, sizeof man, "%s/manifest_spirv.txt", cache_dir);
    FILE *f = fopen(man, "r");
    if (!f) { lsfg_log("W", "step3d: no manifest"); return; }

    int cn = 0, wb = 0, maxb = 0;
    char line[256];
    while (fgets(line, sizeof line, f) && cn < 40) {
        unsigned id = 0;
        if (sscanf(line, "%u", &id) != 1) continue;
        if (strstr(line, " fail ") || strstr(line, " skip ")) continue;
        char path[700];
        snprintf(path, sizeof path, "%s/%u.spv", cache_dir, id);
        std::vector<uint32_t> code;
        if (!load_spv(path, code) || !is_compute(code)) continue;
        cn++;
        std::vector<std::pair<uint32_t,uint32_t>> binds;
        reflect(code, binds);
        if (!binds.empty()) {
            wb++;
            if ((int)binds.size() > maxb) maxb = (int)binds.size();
            if (wb <= 8) {
                char buf[256]; int n = snprintf(buf, sizeof buf, "step3d: id=%u bindings:", id);
                for (auto &b : binds)
                    n += snprintf(buf + n, sizeof buf - (size_t)n, " set%u:b%u", b.first, b.second);
                lsfg_log("I", "%s", buf);
            }
        }
    }
    fclose(f);
    lsfg_log("I", "step3d reflect: compute=%d with_bindings=%d max_bindings=%d", cn, wb, maxb);
    lsfg_log("I", "step3d frames: ready=%d %ux%u phys=%p queue=%p",
             g_fb_ready, g_fb_w, g_fb_h, (void*)g_phys, (void*)g_queue);
    lsfg_log("I", "NEXT step 3f: real VkImage path + history write probe");
}

extern "C" int lsfg_step3d_frames_ready(void) { return g_fb_ready; }

extern "C" void *lsfg_step3d_get_image(int idx) {
    if (!g_fb_ready || idx < 0 || idx > 1) return nullptr;
    return (void*)g_img[idx];
}
extern "C" unsigned lsfg_step3d_fb_width(void) { return g_fb_w; }
extern "C" unsigned lsfg_step3d_fb_height(void) { return g_fb_h; }
