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
/* Runtime-safe settings supported by the current DXVK host. Flow-scale
 * changes are applied at the next serialized frame callback after history reset. */
int  snapframe_host_get_settings(int *enabled, int *flow_scale_percent);
/* Returns the requested shared-swapchain present mode (SnapFramePresentMode). */
int  snapframe_host_get_present_mode(int *present_mode);
/* Queues a shared-swapchain present-mode change for the next DXVK present. */
int  snapframe_host_update_present_mode(int present_mode);
int  snapframe_host_update_settings(int enabled, int flow_scale_percent);
int  snapframe_host_get_last_telemetry(uint64_t *frame, float *gpu_time_ms, float *frame_time_ms);

#ifdef __cplusplus
}
#endif
