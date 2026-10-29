#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

void lsfg_host_on_present(uint32_t width, uint32_t height, uint32_t vk_format);
void lsfg_host_on_present_ex(uint32_t width, uint32_t height, uint32_t vk_format, void *vk_device);
void lsfg_host_set_spirv_count(int n);
int  lsfg_host_spirv_count(void);
void lsfg_host_set_cache_dir(const char *path);
const char *lsfg_host_cache_dir(void);

void lsfg_set_vk_device_hook(void (*fn)(void*));
void lsfg_set_vk_context_hook(void (*fn)(void *device, void *phys, void *queue, unsigned qfam));
void lsfg_set_present_image_hook(void (*fn)(void *device, void *image, unsigned w, unsigned h, unsigned fmt));
void lsfg_register_vk_device(void *dev);
void lsfg_register_vk_context(void *dev, void *phys, void *queue, unsigned qfam);

#ifdef __cplusplus
}
#endif
