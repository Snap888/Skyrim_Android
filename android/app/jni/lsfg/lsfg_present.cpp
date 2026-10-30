#include "lsfg_present.h"
#include "lsfg_log.h"
#include "lsfg_host.h"

#include <stdio.h>
#include <string.h>

#ifdef __ANDROID__
#include <vulkan/vulkan.h>
#else
#include <vulkan/vulkan_core.h>
#endif

extern "C" void lsfg_step3d_set_phys_queue(void *phys, void *queue);
extern "C" void lsfg_step3d_run(void *device, const char *cache_dir, unsigned w, unsigned h);
extern "C" int  lsfg_step3d_frames_ready(void);

static int g_spirv_count;
static unsigned g_present_n;
static int g_present_logged_init;
static char g_cache_dir[512];
static VkDevice g_vk_device = VK_NULL_HANDLE;

void lsfg_host_set_spirv_count(int n) { g_spirv_count = n; }
int  lsfg_host_spirv_count(void) { return g_spirv_count; }

void lsfg_host_set_cache_dir(const char *path) {
    if (!path) { g_cache_dir[0] = 0; return; }
    snprintf(g_cache_dir, sizeof g_cache_dir, "%s", path);
}
const char *lsfg_host_cache_dir(void) { return g_cache_dir; }

extern "C" void lsfg_register_vk_device(void *dev) {
    g_vk_device = reinterpret_cast<VkDevice>(dev);
    lsfg_log("I", "LSFG: VkDevice registered %p", (void*)dev);
}

extern "C" void lsfg_register_vk_context(void *dev, void *phys, void *queue, unsigned qfam) {
    g_vk_device = reinterpret_cast<VkDevice>(dev);
    lsfg_step3d_set_phys_queue(phys, queue);
    extern void lsfg_step3e_set_queue(void *queue, unsigned family);
    lsfg_step3e_set_queue(queue, qfam);
    lsfg_log("I", "LSFG: Vk context registered device=%p phys=%p queue=%p family=%u",
             dev, phys, queue, qfam);
}

void lsfg_host_on_present(uint32_t width, uint32_t height, uint32_t vk_format) {
    lsfg_host_on_present_ex(width, height, vk_format, nullptr);
}

void lsfg_host_on_present_ex(uint32_t width, uint32_t height, uint32_t vk_format, void *vk_device) {
    if (!lsfg_host_active()) return;
    g_present_n++;
    VkDevice dev = vk_device ? reinterpret_cast<VkDevice>(vk_device) : g_vk_device;
    if (!g_present_logged_init) {
        g_present_logged_init = 1;
        lsfg_log("I", "=== LSFG step 3: present hook LIVE ===");
        lsfg_log("I", "present: first frame %ux%u vkfmt=0x%x spirv_modules=%d device=%p",
                 width, height, vk_format, g_spirv_count, (void*)dev);
        lsfg_log("I", "STATUS: hook receives frames; dimensions from SDL window");
    }
    if (dev) {
        // step3d allocates the external frame-history images only. Shader
        // creation, compute dispatch and image hazards are handled by DXVK's
        // active command list from the step3g/step3j callbacks.
        lsfg_step3d_run(dev, g_cache_dir, width, height);
    }
    if (g_present_n == 60 || g_present_n == 300 || (g_present_n % 1800) == 0) {
        lsfg_log("I", "present: frame #%u %ux%u history_images_ready=%d compute_path=DXVK-command-list",
                 g_present_n, width, height, lsfg_step3d_frames_ready());
    }
}
