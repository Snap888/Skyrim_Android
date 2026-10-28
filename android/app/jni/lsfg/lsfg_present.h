#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

void lsfg_host_on_present(uint32_t width, uint32_t height, uint32_t vk_format);
void lsfg_host_set_spirv_count(int n);
int  lsfg_host_spirv_count(void);

#ifdef __cplusplus
}
#endif
