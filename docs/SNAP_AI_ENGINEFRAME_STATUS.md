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
- `SkyrimActivity.java` exposes LSFG plus a Snap AI / Frame Generation section with live Frame Generation and Flow Scale controls. Snap AI itself is explicitly marked unavailable until telemetry is connected.
- The app currently builds C++17 for Android `arm64-v8a`, with CMake 3.22.1 and NDK 29.0.14206865 in CI.

## Changes made in this branch

1. The Android JNI CMake project now fetches Snap AI Engine at the pinned commit above and disables upstream tests/examples for the Android app build.
2. The Android game shared library links `SnapAI::Engine` and `SnapFrame::Engine`, so the portable libraries participate in the native link/package dependency graph.
3. The existing DXVK-coupled `snapframe_host` remains the real GPU implementation. It now exposes safe live settings and is called through a concrete `SnapFrameBackendV1` adapter. Its shaders and WSI present hooks are preserved.
4. Android CI push/PR branch filters now include this branch.

The portable SnapFrame core now drives the DXVK frame callback through the concrete adapter; the adapter delegates to the existing real GPU pipeline. Snap AI remains linked but its telemetry/control loop is not yet active.

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

1. **Implemented:** `SnapFrameBackendV1` adapter routes the serialized DXVK copy callback through the portable core into the existing GPU pipeline.
2. Move ownership from process-global SnapFrame resources to per-device adapter state; preserve all destruction/reset hooks. The current adapter is a single host-level instance.
3. **Implemented:** frame metadata, monotonic frame id and a synchronous opaque native-frame token are passed into `snap_frame_process`.
4. Preserve current midpoint present acceptance/fallback semantics; report generated frames only after DXVK accepts the generated present.
5. **Partially implemented:** completed asynchronous total GPU timing and CPU frame interval are exported as scalar values. GPU-side flow magnitude/confidence/complexity reduction is still pending. Do not add synchronous pixel/flow readback for AI.
6. Connect Snap AI decisions to the next safe frame's backend configuration, clamp all values to capabilities, and bypass AI on invalid metrics/errors.
7. **Partially implemented:** JNI settings and menu controls exist for enable/disable and Flow Scale 25–100%. Other SnapFrame and Snap AI controls remain hidden until supported.
8. Add separate SnapFrame/SnapAI logs, automated lifecycle tests and a full ARM64 APK workflow run.

## Validation status

- Branch creation and base equality: verified with Git commit comparison.
- Portable Snap AI + SnapFrame ARM64 shared-library build: passed Android NDK CI (run #41736778626).
- DXVK adapter + JNI bridge + Snap AI API-version probe + scalar GPU/frame timing export: passed Android ARM64 compile CI (run #41738939416); visual parity/device validation remains outstanding.
- Snap AI telemetry/control loop: not implemented.
- In-game Snap AI / Frame Generation section: added with Frame Generation and Flow Scale controls routed through the portable SnapFrame core; Snap AI adaptive control is explicitly marked unavailable until scalar telemetry is connected.
- APK and physical-device GPU validation: not run.

Do not treat a successful portable/mock test as proof of GPU frame generation. Do not claim performance gains until same-device, same-scene GPU timings and output quality have been compared.

## Latest device-log audit — 2026-11-04
- The uploaded log set records 7,650 captured frames at 2448x1080 with zero skipped frames.
- `snapframe_gpu.csv` contains 255 samples for each of seven stages (1,785 rows total); all rows report `status=ok`. Mean / median / P95 total_chain GPU time is 11.44 / 11.48 / 14.56 ms. Forward and backward flow average 4.39 and 4.43 ms respectively and dominate the measured SnapFrame GPU chain.
- `snapframe_flow.csv` and `snapframe_flow_vectors.csv` contained headers only. The root cause was found in `recordFlowReadback`: it was gated by `diagnostic_suite`, although the user's configuration had `motion_analysis=1` and `diagnostic_suite=0`. The gate now follows `motion_analysis`; device confirmation is pending.
- LSFG was disabled in the uploaded `lsfg.log`; this run is not an LSFG-on comparison.
- The GPU timing results describe the SnapFrame GPU chain, not full game frame time or measured FPS uplift.

## Configuration and diagnostic fixes — 2026-11-04
- The uploaded `Logs/snapframe.conf` exposed literal `\\n` separators. Fixed double-escaped newline literals in `runtime/main.c`, which could serialize multiple settings on one physical line and prevent later keys from being parsed.
- Fixed the same newline escaping in host/bootstrap diagnostics.
- Fixed sparse flow diagnostics to follow `motion_analysis` independently of the optional diagnostic suite.
- Both fixes are committed on `Snap-AI-EngineFrame`; Android CI/device verification is pending.
