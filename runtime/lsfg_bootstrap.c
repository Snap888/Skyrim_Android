#include "lsfg_host.h"
#include <stdint.h>

void lsfg_bootstrap(void) {
    extern char g_game_dir[1024];
    LsfgConfig cfg;
    if (lsfg_load_config(g_game_dir, &cfg))
        lsfg_host_init(&cfg);
}

void lsfg_bootstrap_frame(void) {
    int w = 0, h = 0;
    extern void platform_window_size(int *w, int *h);
    platform_window_size(&w, &h);
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    extern void lsfg_host_on_present(unsigned, unsigned, unsigned);
    lsfg_host_on_present((unsigned)w, (unsigned)h, 0);
    lsfg_host_on_frame();
}
