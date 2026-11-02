#pragma once

#include "snapframe_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Scaffold API. The current implementation manages state only; it does not
 * capture, interpolate, or present frames yet. */
void snapframe_config_defaults(SnapFrameConfig *config);
int  snapframe_host_init(const SnapFrameConfig *config);
void snapframe_host_shutdown(void);
int  snapframe_host_active(void);
int  snapframe_host_set_enabled(int enabled);
void snapframe_host_get_stats(SnapFrameStats *stats);

#ifdef __cplusplus
}
#endif
