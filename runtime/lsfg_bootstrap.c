#include "lsfg_host.h"

void lsfg_bootstrap(void) {
    extern char g_game_dir[1024];
    LsfgConfig cfg;
    if (lsfg_load_config(g_game_dir, &cfg))
        lsfg_host_init(&cfg);
}

void lsfg_bootstrap_frame(void) {
    /* Each frame from platform_present_hook (d3d9_bridge). */
    extern void lsfg_host_on_present(unsigned, unsigned, unsigned);
    lsfg_host_on_present(0, 0, 0);
    lsfg_host_on_frame();
}
