#pragma once

#include "snapframe_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Captures real DXVK frames, estimates bidirectional motion and writes a
 * GPU midpoint reconstruction image and can schedule one additional WSI
 * present per real frame when the midpoint output is valid. */
void snapframe_config_defaults(SnapFrameConfig *config);
/* Opens a dedicated per-game diagnostic log, separate from stderr/game logs. */
int  snapframe_host_set_log_path(const char *path);
int  snapframe_host_set_gpu_log_path(const char *path);
int  snapframe_host_set_flow_log_path(const char *path);
int  snapframe_host_set_flow_vectors_log_path(const char *path);
int  snapframe_host_set_event_log_path(const char *path);
void snapframe_host_log(const char *message);
int  snapframe_host_init(const SnapFrameConfig *config);
void snapframe_host_shutdown(void);
int  snapframe_host_active(void);
int  snapframe_host_set_enabled(int enabled);
void snapframe_host_get_stats(SnapFrameStats *stats);

#ifdef __cplusplus
}
#endif
