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
    char dll_path[512];
    char shader_cache[512];
} LsfgConfig;

bool lsfg_load_config(const char *game_dir, LsfgConfig *out);
bool lsfg_host_init(const LsfgConfig *cfg);
void lsfg_host_shutdown(void);
bool lsfg_host_active(void);
int  lsfg_host_extract_count(void);
void lsfg_host_on_frame(void);

#ifdef __cplusplus
}
#endif
