/* Called from platform_init after g_game_dir is set. */
#include "lsfg_host.h"

void lsfg_bootstrap(void) {
    extern char g_game_dir[1024];
    LsfgConfig cfg;
    if (lsfg_load_config(g_game_dir, &cfg))
        lsfg_host_init(&cfg);
}

void lsfg_bootstrap_frame(void) {
    lsfg_host_on_frame();
}
