#include "lsfg_log.h"
#include "lsfg_host.h"
#include "lsfg_present.h"

#include <stdio.h>
#include <string.h>
#include <atomic>

#ifdef __ANDROID__
#include <vulkan/vulkan.h>
#else
#include <vulkan/vulkan_core.h>
#endif

extern "C" void lsfg_set_present_image_hook(void (*fn)(void*, void*, uint32_t, uint32_t, uint32_t));
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
        lsfg_log("I", "=== LSFG step 3e: real present-image hook LIVE ===");
        lsfg_log("I", "step3e: first real WSI image %ux%u fmt=0x%x image=%p device=%p",
                 w, h, vkfmt, image, device);
    }

    /*
     * History allocation is host-owned and does not submit work. All image
     * copies and compute passes are recorded later into DXVK's active command
     * list, so this hook must never call vkQueueSubmit or clear a live frame.
     */
    if (!g_3f_logged) {
        g_3f_logged = 1;
        lsfg_log("I", "=== LSFG step 3f: DXVK-owned history and compute path ===");
        lsfg_log("I", "step3f: step3d prepares two frame images; step3g records the copy and GPU passes");
    }

    if (n == 1 || n == 60 || n == 300 || (n % 1800) == 0) {
        lsfg_log("I", "step3f: WSI image #%u %ux%u fmt=0x%x img=%p history=step3d",
                 n, w, h, vkfmt, image);
    }
}

extern "C" void lsfg_step3e_install(void) {
    lsfg_set_present_image_hook(lsfg_step3e_on_image);
    lsfg_log("I", "LSFG: present-image hook installed (step 3e/3f \u2014 real image only)");
}
