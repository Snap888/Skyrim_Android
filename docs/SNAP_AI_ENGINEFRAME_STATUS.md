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


## Device log audit — 2026-11-04 (new upload)

- Active branch head at log upload: `1c038b09259dce0ca03e64acd510113a32ebc3cc` (log-only upload on top of `0699e983`).
- DXVK present path reached at 2448x1080; portable `SnapFrameBackendV1` adapter initialized and the GPU pipeline accepted capture, luma, bidirectional flow, consistency and midpoint-warp dispatches.
- Runtime config is now physically line-separated: `enabled=1`, `flow_scale_percent=75`.
- Flow readback is enabled with `motion_analysis=1`, `profiling=1`, `diagnostic_suite=0`, interval 120; aggregate CSV contains 51 rows from frames 121 through 6121. Vector dump completion is reported at frames 601, 1201, ... 6001 (5600 rows per dump).
- GPU CSV contains 206 successful samples per stage (1442 rows). Means: capture/copy 0.568 ms, luma 0.404 ms, flow forward 4.456 ms, flow backward 4.450 ms, consistency 0.012 ms, midpoint warp 1.612 ms, total chain 11.508 ms. Total-chain P95 14.270 ms. These are SnapFrame stage timings, not whole-game frame time.
- Flow quality remains scene-dependent: average sampled mean confidence is about 0.230 in both directions; approximately 53.7–53.8% of all sampled vectors have confidence below 0.1, and approximately 9.8–9.9% are pinned to the search boundary. This is a reason to improve motion estimation/quality validation, not to shrink the search range blindly.
- LSFG config reports `enabled=0`; no LSFG ON/OFF conclusion can be drawn.
- No skipped frames are reported in the visible progress checkpoints, but the uploaded log does not contain a final session summary; do not treat this as a full-session zero-skip certification.
- Event CSV was 3.43 MB. Source review found a per-frame `frame_complete` event and per-stage success events flushed synchronously on each event. The logger was changed to buffer output, sample routine successes, and preserve immediate failure/lifecycle logging. Device verification is pending.


## SnapMotionV2 preparation — 2026-11-04

- Created isolated upstream development branch: [Snap_AI_Engine/feature/snap-motion-v2](https://github.com/Snap888/Snap_AI_Engine/tree/feature/snap-motion-v2), based on pinned portable engine commit `f60acfcb7b09722d5927215957c46045b11f7897`.
- Added backend-neutral `SnapFrame::MotionV2` library and public `snap_frame/motion_v2.h` API. It currently provides validated configuration, reset reasons and frame-tagged asynchronous telemetry storage/retrieval. It has no Vulkan/DXVK dependency and does not yet estimate motion or generate images.
- Added tests for defaults, invalid values, frame ordering, stale readback rejection, bounded metric age and reset epochs.
- Added `docs/CronyFG_Analysis.md` and `docs/SnapMotionV2_Architecture.md` in the upstream repository. The supplied CronyFG Vulkan engine/chain/pacer is explicitly excluded from the port; only algorithmic stages will be adapted.
- The upstream branch is intentionally not yet pinned into this Skyrim build. No Skyrim runtime code or V1 shader path has been switched to V2. Do not enable V2 or connect Snap AI decisions until the GPU motion pipeline and frame-associated async reduction are implemented and validated.
- Latest upstream CI for the new branch is run [#41810018682](https://github.com/Snap888/Snap_AI_Engine/actions/runs/41810018682); its final result is pending at the time of this status update.


## SnapMotionV2 shader preparation — 2026-11-04

- Created safety backup before the next Skyrim-side V2 integration work: [backup/Snap-AI-EngineFrame-pre-snapmotion-v2-shaders-2026-11-04](https://github.com/Snap888/Skyrim_Android/tree/backup/Snap-AI-EngineFrame-pre-snapmotion-v2-shaders-2026-11-04), based on this branch's audited state.
- In upstream [Snap_AI_Engine/feature/snap-motion-v2](https://github.com/Snap888/Snap_AI_Engine/tree/feature/snap-motion-v2), added an isolated V2 shader namespace containing L0 luma, L0-to-L1 downsample and a shared coarse/fine block-search kernel. The shader compiler now validates these in addition to the existing V1 shaders.
- Search shader explicitly rejects out-of-frame previous-image samples and clamps spatial seed coordinates. Motion vectors remain expressed in L0 pixels at both pyramid levels.
- These shaders are currently compile-validated source only. They are not dispatched by Skyrim, have no DXVK resource bindings yet, and do not change the V1 rendering path.
- Upstream shader CI run [#41810539818](https://github.com/Snap888/Snap_AI_Engine/actions/runs/41810539818) completed successfully. It built the portable libraries, passed all 3 portable unit-test suites, compiled and validated the V1 + V2 shader set as Vulkan 1.1 SPIR-V, and passed the standalone SnapFrame build/tests.
- Upstream V2 branch head at this audit: `9c278d55ecb47a90cca2259ec043494f624f45ec`.


### V2 shader coverage update

The upstream V2 shader set now includes luma, downsample, coarse/fine search, refinement, forward/backward validation, static-region mask and bidirectional interpolation with gamut clamp. These are still isolated source assets. The Skyrim branch does not yet compile or dispatch them, and the external engine dependency remains pinned to the previously validated commit until the DXVK adapter contract and GPU resources are implemented.
