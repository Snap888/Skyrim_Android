# SnapFrame Engine integration

## Current status

The portable SnapFrame v1 library is pinned and linked into the Android runtime. A concrete `SnapFrameBackendV1` adapter routes DXVK frame callbacks through the portable core and invokes the real DXVK GPU pipeline using an opaque native-frame token. SnapFrame is the sole frame-generation backend. The adapter is currently a single host-level instance; migrating process-global GPU state to per-device adapter objects remains outstanding.

## Existing GPU path

The Skyrim fork already contains the original compute implementation and DXVK-specific callbacks. It performs real-frame capture/history, luma preprocessing, bidirectional block-matching flow, consistency checks and midpoint warp reconstruction. It also contains GPU timing and diagnostic logging.

Keep this implementation intact until a new adapter has output parity and lifecycle coverage. Do not replace it with the portable mock backend.

## Portable API boundary

Header: `snap_frame/snap_frame.h`.

The portable core owns configuration validation, frame sequencing, bypass/fallback state and host-visible counters. The adapter passes an opaque stack-scoped native-frame token synchronously; it is never retained. The backend delegates GPU resource ownership, shader dispatch, barriers, synchronization and generated-frame scheduling to the existing DXVK host.

API v1 supports multiplier 1 (bypass) and multiplier 2 (one midpoint). SnapFrame is the only frame-generation backend exposed by the Skyrim runtime.

## Backend migration requirements

1. **Outstanding:** move resource ownership from process-global variables to per-DXVK-device state.
2. **Implemented:** map the real DXVK frame callback to `SnapFrameFrameInput` with a monotonic frame id and opaque native token.
3. **Preserved:** existing shader algorithms and resource formats are unchanged.
4. **Implemented:** `initialize`, `update_config`, `reset_history`, `process_frame` and `shutdown` are wired through `SnapFrameBackendV1`.
5. **Preserved:** DXVK synchronization and present ownership remain in the legacy DXVK host.
6. **Preserved:** generated-frame counters update only after the DXVK present-result hook accepts the generated present. The portable output reports zero at dispatch time because acceptance is asynchronous.
7. **Preserved:** the device-destroy hook releases old resources; the host adapter rebuilds resources on the next device callback. Per-device engine instances remain outstanding.
8. **Preserved:** backend errors bypass generated output and leave the real-frame present path intact.

## Hot settings

The portable core exposes hot enable/config/quality setters. The adapter currently accepts only enable/disable and flow scale (25–100%); it rejects changes to quality, multiplier, motion analysis, profiling and diagnostic fields because those are not wired to the legacy GPU host. JNI updates the portable core config, the adapter forwards validated values to atomics and requests a history reset, and the serialized DXVK copy callback releases/recreates temporal resources with the new scale. The UI does not directly mutate Vulkan objects.

## Validation

The existing host's asynchronous GPU timestamp queries and CSV logs remain authoritative. `SnapFrameFrameOutput` timing fields are currently zero because legacy timing values arrive asynchronously; generated-frame output is also zero at dispatch time because WSI acceptance is reported later by DXVK. This avoids inventing timing or accepted-frame values.

Portable API tests do not validate Vulkan dispatch. The concrete adapter compiled for Android ARM64 in CI run #41738044737. It still requires device execution, GPU timestamp checks, generated-frame acceptance counters and visual comparison against the unchanged pre-adapter path.


## Shared-swapchain present mode

The launcher writes `present_mode=0..3` to `port/snapframe.conf` and applies it at the next launch:

- `0` — Auto; preserve DXVK's current VSync/tear-free selection.
- `1` — FIFO.
- `2` — MAILBOX, then FIFO fallback if unsupported.
- `3` — IMMEDIATE, then FIFO fallback if unsupported.

The choice is made by DXVK when creating the shared swapchain. Game and generated frames therefore use the same Vulkan present mode; independent source/generated present modes are not supported by this integration. The DXVK D3D9 library obtains the setting through a function-pointer callback registered by SnapFrame Host at initialization and cleared during shutdown. This keeps the shared-library boundary free of a direct unresolved reference to `snapframe_host_get_present_mode`.

For A/B testing, use Auto first, then FIFO/MAILBOX/IMMEDIATE with the same scene and VSync setting. Record the selected/actual present mode and compare source FPS, source frame times, generated-frame acceptance and pacing. Do not treat queued/present-submission FPS as measured panel scanout.
