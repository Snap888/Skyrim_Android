/* Host-side LSFG for Skyrim_Android.
 * Step 1: PE RCDATA extract from user Lossless.dll into shader_cache.
 * Step 2+: DXBC\u2192SPIR-V, Vulkan present (not yet).
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
    int game_fps_limit;
    char dll_path[512];
    char shader_cache[512];
    char state_cache[512];
} LsfgConfig;

bool lsfg_load_config(const char *game_dir, LsfgConfig *out);
bool lsfg_host_init(const LsfgConfig *cfg);
void lsfg_host_shutdown(void);
bool lsfg_host_active(void);
int  lsfg_host_extract_count(void);
void lsfg_host_on_frame(void);
float lsfg_host_flow_scale(void);
unsigned lsfg_host_generation_count(void);
void lsfg_host_begin_frame_settings(void);
int lsfg_host_get_settings(int *multiplier, int *flow_percent, int *performance, int *game_fps_limit);
bool lsfg_host_update_settings(int multiplier, int flow_percent, int performance, int game_fps_limit);
int lsfg_host_game_fps_limit(void);

#ifdef __cplusplus
}
#endif
