# SnapFrame Engine integration

## Current status

The portable SnapFrame v1 library is pinned and linked into the Android runtime. The existing DXVK-bound implementation in `android/app/jni/snapframe` remains the active GPU path. The portable engine is not yet the owner of frame processing because the concrete DXVK `SnapFrameBackendV1` adapter is still to be implemented.

## Existing GPU path

The Skyrim fork already contains the original compute implementation and DXVK-specific callbacks. It performs real-frame capture/history, luma preprocessing, bidirectional block-matching flow, consistency checks and midpoint warp reconstruction. It also contains GPU timing and diagnostic logging.

Keep this implementation intact until a new adapter has output parity and lifecycle coverage. Do not replace it with the portable mock backend.

## Portable API boundary

Header: `snap_frame/snap_frame.h`.

The portable core owns configuration validation, frame sequencing, bypass/fallback state and host-visible counters. The backend owns device-bound resources, shader dispatch, barriers, synchronization and generated-frame scheduling.

API v1 supports multiplier 1 (bypass) and multiplier 2 (one midpoint). The current legacy LSFG menu's x2/x3/x4 values are not interchangeable with SnapFrame v1 multiplier values; do not map x3/x4 into the new API.

## Backend migration requirements

1. Move resource ownership from process-global variables to per-DXVK-device state.
2. Map the real DXVK frame callback to `SnapFrameFrameInput` using a monotonically increasing frame id and an opaque native token.
3. Preserve the existing shader algorithms and resource formats.
4. Implement `initialize`, `update_config`, `reset_history`, `process_frame` and `shutdown`.
5. Keep DXVK synchronization and present ownership inside the adapter.
6. Count a generated frame only after the existing DXVK present hook accepts it.
7. Reset/release resources before device teardown and rebuild them after device recreation.
8. Preserve original-frame present as the fallback for every backend error.

## Hot settings

The portable core exposes hot enable/config/quality setters. GPU-dependent changes are only genuinely hot if the backend implements `update_config` and applies them at a safe synchronization point. The current legacy host now supports hot enable/disable and flow scale (25–100%): JNI updates atomics and requests a history reset; the serialized DXVK copy callback consumes the request, releases temporal resources and recreates them with the new scale. The UI does not directly mutate Vulkan objects. If a change requires resource recreation, defer it until the GPU is no longer using the old resources.

## Validation

Portable/mock tests do not validate Vulkan dispatch. Real validation requires Android ARM64 build, device execution, GPU timestamp checks, generated-frame acceptance counters and visual comparison against the unchanged legacy path.
