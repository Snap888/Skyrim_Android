#include "lsfg_present.h"
#include "lsfg_log.h"
#include "lsfg_host.h"

static int g_spirv_count;
static unsigned g_present_n;
static int g_present_logged_init;

void lsfg_host_set_spirv_count(int n) { g_spirv_count = n; }
int  lsfg_host_spirv_count(void) { return g_spirv_count; }

void lsfg_host_on_present(uint32_t width, uint32_t height, uint32_t vk_format) {
    if (!lsfg_host_active()) return;
    g_present_n++;

    if (!g_present_logged_init) {
        g_present_logged_init = 1;
        lsfg_log("I", "=== LSFG step 3: present hook LIVE ===");
        lsfg_log("I", "present: first frame %ux%u vkfmt=0x%x spirv_modules=%d",
                 width, height, vk_format, g_spirv_count);
        lsfg_log("I", "STATUS: hook receives frames; compute FG pipeline NOT wired yet");
        lsfg_log("I", "NEXT: build VkPipeline from *.spv + insert generates between real frames");
    }

    if (g_present_n == 60 || g_present_n == 300 || (g_present_n % 1800) == 0) {
        lsfg_log("I", "present: frame #%u %ux%u (still no interpolation)",
                 g_present_n, width, height);
    }
}
