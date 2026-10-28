/* Called from platform_init after g_game_dir is set. */
#include "lsfg_host.h"
#ifdef __ANDROID__
#include <android/log.h>
#define BOOT_LOG(...) __android_log_print(ANDROID_LOG_INFO, "skyrim-lsfg", __VA_ARGS__)
#else
#include <stdio.h>
#define BOOT_LOG(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#endif

void lsfg_bootstrap(void) {
    extern char g_game_dir[1024];
    BOOT_LOG("bootstrap: game_dir=%s", g_game_dir);
    LsfgConfig cfg;
    if (lsfg_load_config(g_game_dir, &cfg))
        lsfg_host_init(&cfg);
    else
        BOOT_LOG("bootstrap: no port/lsfg.conf — LSFG idle");
}

void lsfg_bootstrap_frame(void) {
    lsfg_host_on_frame();
}
