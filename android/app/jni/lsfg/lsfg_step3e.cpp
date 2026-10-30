#include "lsfg_log.h"
#include "lsfg_host.h"

#include <stdio.h>
#include <string.h>
#include <atomic>

#ifdef __ANDROID__
#include <vulkan/vulkan.h>
#else
#include <vulkan/vulkan_core.h>
#endif

extern "C" void lsfg_set_present_image_hook(void (*fn)(void*, void*, uint32_t, uint32_t, uint32_t));
extern void lsfg_host_on_present_ex(uint32_t width, uint32_t height, uint32_t vk_format, void *vk_device);
static std::atomic<unsigned> g_n{0};
static int g_logged_init;
static int g_3f_logged;
static int g_null_skipped;

extern "C" void lsfg_step3e_set_queue(void *queue, uint32_t family) {
    (void)queue;
    (void)family;
}

extern "C" void lsfg_step3e_on_image(void *device, void *image,
                                     uint32_t w, uint32_t h, uint32_t vkfmt) {
    if (!lsfg_host_active()) return;

    if (!image) {
        g_null_skipped++;
        return;
    }

    lsfg_host_on_present_ex(w, h, vkfmt, device);

    unsigned n = ++g_n;
    if (!g_logged_init) {
        g_logged_init = 1;
        lsfg_log("I", "=== LSFG step 3e/3f: present-image hook LIVE (real VkImage) ===");
        lsfg_log("I", "step3f: first real image %ux%u fmt=0x%x image=%p device=%p",
                 w, h, vkfmt, image, device);
    }

    /*
     * Do not submit raw Vulkan command buffers from this present hook.
     * DXVK owns and asynchronously submits work to this queue; a direct
     * vkQueueSubmit here is not coordinated with its submission queue.
     * The old step3f probe also cleared history[0], potentially racing
     * with the DXVK history copy and destroying the pixels being tested.
     * Keep this hook observational until diagnostics can be inserted into
     * the DXVK command list with proper resource/layout tracking.
     */
    if (!g_3f_logged) {
        g_3f_logged = 1;
        lsfg_log("I", "=== LSFG step 3f: observational present hook ===");
        lsfg_log("I", "step3f: raw queue submit/history clear disabled; waiting for DXVK cmd-stream probe");
    }

    if (n == 1 || n == 60 || n == 300 || (n % 1800) == 0) {
        lsfg_log("I", "step3f: image #%u %ux%u fmt=0x%x img=%p history_probe=disabled",
                 n, w, h, vkfmt, image);
    }
}

extern "C" void lsfg_step3e_install(void) {
    lsfg_set_present_image_hook(lsfg_step3e_on_image);
    lsfg_log("I", "LSFG: present-image hook installed (step 3e/3f \u2014 real image only)");
}
