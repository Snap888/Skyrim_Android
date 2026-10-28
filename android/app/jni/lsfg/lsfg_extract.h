#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int lsfg_extract_dll(const char *dll_path, const char *cache_dir);
bool lsfg_extract_cache_ready(const char *cache_dir);

#ifdef __cplusplus
}
#endif
