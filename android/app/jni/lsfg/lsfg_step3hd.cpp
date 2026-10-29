/* Step 3h-d: present-path scaffold for generated frame.
 * Does NOT enable per-frame FG. After 3h-c probe, logs that out image is available
 * and documents the DXVK inject point for a future safe present.
 */
#include "lsfg_log.h"

#include <stdio.h>

#ifdef __ANDROID__
#include <vulkan/vulkan.h>
#else
#include <vulkan/vulkan_core.h>
#endif

extern "C" void *lsfg_step3hc_get_out_view(void);
extern "C" void *lsfg_step3hc_get_out_image(void);
extern "C" int lsfg_step3hc_chain_ready(void);
extern "C" unsigned lsfg_step3d_fb_width(void);
extern "C" unsigned lsfg_step3d_fb_height(void);

static int g_logged;

extern "C" void lsfg_step3hd_after_probe(void) {
    if (g_logged) return;
    if (!lsfg_step3hc_chain_ready()) return;
    void *out = lsfg_step3hc_get_out_image();
    void *view = lsfg_step3hc_get_out_view();
    g_logged = 1;
    lsfg_log("I", "=== LSFG step 3h-d: present path scaffold ===");
    lsfg_log("I", "step3h-d: generate out image=%p view=%p size=%ux%u",
             out, view, lsfg_step3d_fb_width(), lsfg_step3d_fb_height());
    lsfg_log("I", "step3h-d: NOT blitting to swapchain yet (wrong/partial FG would corrupt frame)");
    lsfg_log("I", "step3h-d: inject point = D3D9SwapChainEx::PresentImage after step3g copy");
    lsfg_log("I", "step3h-d: enable per-frame only after full flow chain + pacing");
    lsfg_log("I", "STATUS: descriptors OK, probe OK, present DEFERRED — FPS safe");
}

extern "C" void lsfg_step3hd_install(void) {
    lsfg_log("I", "LSFG: step3h-d present scaffold installed (no swapchain inject)");
}
