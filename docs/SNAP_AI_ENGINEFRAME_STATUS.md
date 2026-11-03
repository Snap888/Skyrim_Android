# Snap AI EngineFrame integration status

Last audited: 2026-11-04

## Working branch and safety

- Working branch: `Snap-AI-EngineFrame` (Git ref names cannot contain spaces).
- Base: `frame_gen`.
- Base commit at branch creation: `c385a66daef920f83a86e7eaf16ff822e507e3c7`.
- Backup before the next integration phase: `backup/Snap-AI-EngineFrame-2026-11-04`.
- `main`, `frame_gen`, and other work branches are not modified.

## Audited upstream

Snap AI Engine repository: `Snap888/Snap_AI_Engine`, branch `snapframe-engine`, pinned commit:
`f60acfcb7b09722d5927215957c46045b11f7897`.

The corresponding PR #1 is still a draft and open. Its portable C APIs and core tests are present, but its own PR description explicitly says that the DXVK implementation under `framegen/backends/dxvk/legacy` has not yet been adapted to `SnapFrameBackendV1`. It also makes no Android APK or real-device validation claim.

## Existing Skyrim implementation

- `android/app/jni/snapframe/snapframe_host.cpp` is the current real DXVK-bound implementation.
- It uses DXVK internal classes and owns the current luma, bidirectional block-flow, consistency and midpoint-warp GPU resources.
- The existing DXVK fork already has Skyrim-specific copy-frame, device-destroy, logging and generated-frame/present-result hooks.
- `runtime/snapframe_bootstrap.c` initializes the legacy host and its per-game logs.
- `SkyrimActivity.java` currently exposes the LSFG menu, not a Snap AI / SnapFrame settings section.
- The app currently builds C++17 for Android `arm64-v8a`, with CMake 3.22.1 and NDK 29.0.14206865 in CI.

## Changes made in this branch

1. The Android JNI CMake project now fetches Snap AI Engine at the pinned commit above and disables upstream tests/examples for the Android app build.
2. The Android game shared library links `SnapAI::Engine` and `SnapFrame::Engine`, so the portable libraries participate in the native link/package dependency graph.
3. The existing DXVK-coupled `snapframe_host` remains enabled and unchanged as the current GPU implementation. It must not be removed until the new adapter has parity-tested GPU output.
4. Android CI push/PR branch filters now include this branch.

These changes establish the portable library build/link boundary. They do **not** yet mean that the new portable SnapFrame API is driving frame generation or that Snap AI recommendations affect GPU dispatches.

## API constraints discovered

### SnapFrame v1

- The portable API v1 accepts multiplier 1 (bypass) or 2 (one midpoint frame); it does not implement x3/x4.
- Default configuration is disabled.
- Hot enable/quality/config changes are exposed, but GPU-affecting changes are only truly hot when the backend implements `update_config`.
- Backend lifecycle and device-bound resource management belong to the host adapter.

### Snap AI v1

- The API exposes Performance, Balanced and Quality profiles, plus enable/disable.
- The controller consumes scalar metrics and returns bounded decisions for flow scale, search radius, refinement passes, bidirectional flow and artifact checks.
- It does not own Vulkan objects, command buffers, queues or present synchronization.
- The current API does not expose arbitrary live mutation of every AI config field; profile and enabled state are hot setters.

## Required next implementation steps

1. Implement a per-DXVK-device `SnapFrameBackendV1` adapter by migrating the existing GPU dispatches without changing their algorithms.
2. Move ownership from process-global SnapFrame resources to per-device adapter state; preserve all destruction/reset hooks.
3. Define how the host passes frame metadata and an opaque frame token into `snap_frame_process` on the serialized render/present path.
4. Preserve current midpoint present acceptance/fallback semantics; report generated frames only after DXVK accepts the generated present.
5. Export scalar telemetry from asynchronous GPU timestamp completion and GPU-side flow summaries. Do not add synchronous pixel/flow readback for AI.
6. Connect Snap AI decisions to the next safe frame's backend configuration, clamp all values to capabilities, and bypass AI on invalid metrics/errors.
7. Add JNI settings backed by real native state, then expose only controls that the adapter actually supports.
8. Add separate SnapFrame/SnapAI logs, automated lifecycle tests and a full ARM64 APK workflow run.

## Validation status

- Branch creation and base equality: verified with Git commit comparison.
- Portable Snap AI + SnapFrame ARM64 shared-library build: passed Android NDK CI (run #41736778626).
- DXVK adapter parity: not implemented.
- Snap AI telemetry/control loop: not implemented.
- In-game Snap AI / Frame Generation section: added with working Frame Generation and Flow Scale controls for the legacy host; Snap AI adaptive control is explicitly marked unavailable until the telemetry adapter is connected.
- APK and physical-device GPU validation: not run.

Do not treat a successful portable/mock test as proof of GPU frame generation. Do not claim performance gains until same-device, same-scene GPU timings and output quality have been compared.
