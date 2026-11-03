#pragma once

#include "snapframe_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Captures two real DXVK frames into a private GPU-resident history ring.
 * Optical flow, interpolation and generated-frame presentation are not yet
 * implemented. */
void snapframe_config_defaults(SnapFrameConfig *config);
/* Opens a dedicated per-game diagnostic log, separate from stderr/game logs. */
int  snapframe_host_set_log_path(const char *path);
void snapframe_host_log(const char *message);
int  snapframe_host_init(const SnapFrameConfig *config);
void snapframe_host_shutdown(void);
int  snapframe_host_active(void);
int  snapframe_host_set_enabled(int enabled);
void snapframe_host_get_stats(SnapFrameStats *stats);

#ifdef __cplusplus
}
#endif
