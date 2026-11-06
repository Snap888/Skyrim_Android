#pragma once

/*
 * SnapFrame <-> DXVK Native 2.6.2 private integration boundary.
 *
 * This header is copied into the pinned upstream DXVK 2.6.2 checkout by
 * apply_native_262_android_port.py. It deliberately exposes no DXVK C++
 * declarations: the opaque pointers are valid only inside the matching
 * 2.6.2 binary and must never cross into the legacy 1.9.2b host.
 *
 * The first hook is capture/compute-recording only. It runs on the DXVK
 * command-stream worker before the source blit is recorded. Implementations
 * must enqueue GPU work into the supplied context, must not submit/flush it,
 * must not wait on fences, and must not read pixels/flow back to the CPU.
 * A missing weak symbol means the optional 2.6.2 SnapFrame host is not linked;
 * DXVK then follows its unmodified present path.
 */

#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SNAPFRAME_DXVK262_ABI_VERSION 1u

typedef struct SnapFrameDxvk262FrameInfo {
  uint32_t struct_size;
  uint32_t abi_version;
  uint64_t frame_id;
  uint32_t width;
  uint32_t height;
  VkFormat format;
  void* window_token;
  uint32_t reserved;
} SnapFrameDxvk262FrameInfo;

/*
 * Return 0 when the frame was accepted for asynchronous GPU processing.
 * Non-zero means SnapFrame skipped this frame; DXVK must still present the
 * original source image. context/device/source_view are DXVK 2.6.2 objects
 * and may only be consumed by a host compiled against the exact pinned
 * 2.6.2 headers.
 */
int snapframe_dxvk262_record_source(
    void* context,
    void* device,
    void* source_view,
    const SnapFrameDxvk262FrameInfo* frame_info)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((weak))
#endif
    ;

/* Optional non-blocking retirement notification for frame_info.window_token. */
void snapframe_dxvk262_release_window(void* window_token)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((weak))
#endif
    ;

/* Optional final notification after DXVK has drained the device and
 * stopped its pipeline workers. Must not wait, submit, or flush. */
void snapframe_dxvk262_release_device(void* device_token)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((weak))
#endif
    ;

#ifdef __cplusplus
}
#endif
