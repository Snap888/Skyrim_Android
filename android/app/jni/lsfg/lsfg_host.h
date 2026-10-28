/* Host-side LSFG (Lossless Scaling Frame Generation) for Skyrim_Android.
 * Design: compositor-style FG after DXVK produces a VkImage (WinNative/Eden path),
 * NOT MediaProjection and NOT loading Lossless.dll as a PE module.
 *
 * Status: scaffolding — config load + lifecycle stubs. Full compute chain TBD.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LsfgConfig {
    int enabled;
    int multiplier;
    float flow_scale;
    int performance;
    char dll_path[512];
    char shader_cache[512];
} LsfgConfig;

bool lsfg_load_config(const char *game_dir, LsfgConfig *out);
bool lsfg_host_init(const LsfgConfig *cfg);
void lsfg_host_shutdown(void);
bool lsfg_host_active(void);
void lsfg_host_on_frame(void);

#ifdef __cplusplus
}
#endif
