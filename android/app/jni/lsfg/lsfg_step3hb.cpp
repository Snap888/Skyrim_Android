/* Step 3h-b: DISABLED at runtime for Adreno (light 3h-j owns FG).
 */
#include "lsfg_log.h"

#include <stdio.h>
#include <string.h>
#include <vector>

#ifdef __ANDROID__
#include <vulkan/vulkan.h>
#else
#include <vulkan/vulkan_core.h>
#endif

static VkDevice g_dev = VK_NULL_HANDLE;
static VkPhysicalDevice g_phys = VK_NULL_HANDLE;
static uint32_t g_qfam;
static int g_graph_ready;
static int g_logged;

extern "C" void lsfg_step3hb_set_phys(void *phys) {
    g_phys = (VkPhysicalDevice)phys;
}

extern "C" int lsfg_step3hb_graph_ready(void) { return g_graph_ready; }

extern "C" void lsfg_step3hb_after_copy(void *device, unsigned qfam) {
    (void)device; (void)qfam;
    if (!g_logged) {
        g_logged = 1;
        g_graph_ready = 1;
        lsfg_log("I", "step3h-b: DISABLED (light 3h-j path — no 25-pipe build)");
    }
}

extern "C" void lsfg_step3hb_install(void) {
    lsfg_log("I", "LSFG: step3h-b pass-graph builder installed");
}
