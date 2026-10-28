#pragma once
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

void lsfg_log_open(const char *game_dir);
void lsfg_log_close(void);
void lsfg_log(const char *level, const char *fmt, ...);

#ifdef __cplusplus
}
#endif
