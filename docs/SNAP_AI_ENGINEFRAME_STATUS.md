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
- Full APK build: not run. Physical-device gameplay logs have now been supplied and audited, but visual parity, generated-image acceptance and measured FPS uplift remain unverified.

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

The upstream V2 shader set now includes luma, downsample, coarse/fine search, refinement, forward/backward validation, static-region mask and bidirectional interpolation with gamut clamp. These are still isolated source assets. The Skyrim build compiles and validates these sources into its generated SPIR-V header from the pinned external engine tree, but it does not yet create V2 DXVK resources or dispatch them. The external engine dependency remains pinned until the DXVK adapter contract and GPU resources are implemented.


## SnapMotionV2 quality and telemetry shader progress — 2026-11-04

- Added V2-only refinement (3x3 median, subpixel refinement and local confidence), separate forward/backward round-trip validation, static-region mask, and time-parametric interpolation with confidence fallback and neighborhood gamut clamp.
- Added two-stage GPU metrics reduction: workgroup partials followed by one compact frame-tagged record. Metrics include mean motion magnitude, normalized local motion complexity, mean confidence, low-confidence ratio, occlusion ratio, static ratio, artifact-risk heuristic and photometric scene-cut candidate.
- Reduction is designed to asynchronously read back only a small scalar record. It does not read back full flow vectors and does not insert a GPU wait. The record carries frame ID, timestamp and extent; less than 90% valid flow-cell coverage marks telemetry invalid.
- The full-resolution static mask is sampled by normalized flow UV when reduced to the lower-resolution flow grid.
- Latest upstream CI run [#41810959946](https://github.com/Snap888/Snap_AI_Engine/actions/runs/41810959946) passed, including portable builds/tests and Vulkan 1.1 SPIR-V compile/validation for the complete current V2 shader set.
- The reduction's artifact-risk and scene-cut outputs are provisional heuristics. They are not yet connected to Snap AI and must not be used for adaptive decisions before device calibration and frame/history validation.
- Skyrim still runs V1 only. V2 shaders are not yet wired to DXVK resources, dispatch, synchronization or Present. The pinned external Snap AI Engine commit remains unchanged.


## Portable MotionV2 core linked into Skyrim — 2026-11-04

- Updated the pinned Snap AI Engine dependency from the earlier f60acf revision to reviewed feature-branch commit bc040d08b021b63d3d437455663be014a1e9f383.
- Linked SnapFrame::MotionV2 into both snapframe_host and the game target. Android explicitly loads libsnap_motion_v2.so before main.
- Android CI now builds/tests the updated pinned engine revision in both portable jobs and compiles the Skyrim ARM64 DXVK host against the new target.
- This is the portable core linkage milestone only. The current host does not yet call snap_motion_v2_*; V2 shaders are not part of Skyrim runtime shader dispatch, and the active rendering path remains V1.
- Latest Skyrim CI run #41817208666 passed portable API tests, portable ARM64 libraries, DXVK host ARM64 compile and Java compilation. The full APK job was skipped.


## Second device log audit — 2026-11-04

- The uploaded branch head containing the logs is `d20835e8e6588ba7a6cbd5c205693b54b8ca5fe0`.
- SnapFrame ran at 2448x1080 and reached frame 840. The DXVK present hook, portable V1 adapter, history capture, Luma, bidirectional block matching, consistency, midpoint warp and additional WSI-present queue path are visible in the log. The event CSV now contains sparse samples at frame 1 and 120-frame/300-frame progress intervals rather than per-frame routine success rows.
- `snapframe_gpu.csv`: 28 successful samples per stage. Mean/P95 total chain is 12.389/16.106 ms; forward/backward flow average 4.754/4.824 ms. Flow consumes about 77% of the measured chain.
- `snapframe_flow.csv`: seven samples at frames 121–841. Mean confidence and consistency vary strongly by scene; this is not a uniformly reliable motion field.
- `snapframe_flow_vectors.csv`: 5,600 vectors for frame 601. Mean forward/backward confidence ~0.105; ~62.5% below 0.1; ~12% at the search boundary; mean consistency ~0.023 and ~91.7% zero. This is one difficult-motion snapshot, not the whole session.
- GPU stage times increase around frame 720 while resolution remains 2448x1080. No temperature/frequency data is present, so thermal throttling is only one possible explanation.
- LSFG's own log runs to present frame 5400 and must not be frame-correlated with SnapFrame's 840-frame stream. It starts enabled at multiplier 2, then records a runtime change to multiplier 1. The final config is multiplier 1. Generate dispatch commands and requested/queued counters exist, but actual generated WSI-present acceptance and displayed FPS are not proven by these files.
- Latest Android CI run [#41817208666](https://github.com/Snap888/Skyrim_Android/actions/runs/41817208666) passed portable API tests, Snap AI/SnapFrame ARM64 shared-library build, DXVK SnapFrame host ARM64 compile and Java compilation. The full APK job was skipped because `build_apk` was not requested.
- A safety backup was created before this follow-up work: [backup/Snap-AI-EngineFrame-pre-2026-11-04-logs2](https://github.com/Snap888/Skyrim_Android/tree/backup/Snap-AI-EngineFrame-pre-2026-11-04-logs2).

The active Skyrim runtime remains V1. The portable MotionV2 core is linked, and its V2 shader sources are compiled into the generated shader header and validated by CI. The V2 GPU resources, descriptor bindings, dispatch sequence and async readback are not yet connected to DXVK; Snap AI adaptive control remains unavailable. Do not treat shader compile validation or queued generate commands as proof of actual frame generation.


## Full DXVK/LSFG and audio log audit — 2026-11-04

- `dxvk_lsfg_gpu.csv` was parsed in full: 215,880 stage samples, 76 stage names, all `status=ok`, covering frame IDs 0–2842. Derived sum of stage durations averages ~8.558 ms/frame, P95 ~9.944 ms.
- Highest LSFG GPU costs: GammaDelta_L6 ~1.446 ms, Generate ~1.166 ms, Alpha ~0.838 ms, Mipmaps ~0.732 ms, Beta ~0.594 ms. These are separate from SnapFrame's GPU measurements and should be optimized as a distinct path.
- The LSFG GPU, LSFG present, DXVK CPU-present and SnapFrame frame counters do not cover identical ranges. Do not align or combine their timings as one frame.
- Full Android `log.txt` has 6,133 lines and no observed fatal signal, Vulkan device loss, fatal exception or abnormal process exit; it ends with `ExitProcess(0)`.
- Audio diagnostics show repeated per-source empty-queue counters, but output mixer telemetry remains `queue_low=0`, `enqueue_fail=0`, `slow_mix=0`, maximum mix time 1 ms. One 34 ms refill gap is explicitly marked end-of-stream. No output mixer underrun is evidenced; audio code was left untouched.
 

## SnapMotionV2 coordinate correction and pin update — 2026-11-04

- During shader-contract review, found that V2 validation and interpolation converted L0-pixel motion vectors to normalized UV using flow-grid dimensions. This would over-scale motion by the flow block stride.
- Corrected both shaders to normalize with L0 luma width/height; updated the documented push-constant contract.
- Upstream backup: [backup/feature-snap-motion-v2-pre-coordinate-fix-2026-11-04](https://github.com/Snap888/Snap_AI_Engine/tree/backup/feature-snap-motion-v2-pre-coordinate-fix-2026-11-04).
- Upstream corrected revision: [ccf4589ca19d5f788adc56c7248d94f83ca0a506](https://github.com/Snap888/Snap_AI_Engine/commit/ccf4589ca19d5f788adc56c7248d94f83ca0a506). Shader compile/validation CI passed on the corrected shader commit and the subsequent documentation commit.
- Skyrim CMake and Android CI are now pinned to the corrected upstream revision. This only updates the compiled V2 shader assets and portable core revision; no V2 shader is dispatched at runtime.
- Keep V1 as default. The V2 GPU resource/descriptor/dispatch chain, asynchronous reduction readback and device visual validation remain incomplete.


## Third device log audit and present-acceptance correction — 2026-11-04

- Latest log upload commit: `b126917c7221c23f85b58df94f167dcb072e1fd5`, based on the previous source head; new files include `runtime_tuning.conf` and `gpu-driver.conf`.
- Test configuration changed during the run: SnapFrame started disabled, was enabled at 75%, changed to 25%, then disabled. The custom ICD is Turnip Gen8 V38. CPU affinity and worker spin-wait settings are explicitly marked not implemented.
- At 75% (112x50 flow grid), mean forward/backward flow cost is 4.460/4.470 ms and total chain 11.508 ms. At 25% (39x17), these are 0.592/0.638 ms and 3.933 ms. Different scene intervals mean this is not a controlled A/B test.
- Flow quality is scene-dependent. Frames 121/241/361 and 826/946/1066 have low confidence; frame 601 is mostly static and high-confidence. Its full 5,600-cell vector dump gives confidence 0.88464, consistency 0.90590, 4.625% below confidence 0.1 and 0.5% at the +/-10 search boundary. This explicitly supersedes the previous incorrect frame-601 interpretation in this document.
- The current LSFG profile covers frame IDs 0–1219; the highest mean GPU stages are GammaDelta_L6 1.093 ms, Generate 0.988 ms, Alpha 0.642 ms, Mipmaps 0.607 ms and Beta 0.460 ms. Do not compare its absolute totals directly with the earlier longer LSFG session.
- Audio output mixer remains healthy in the log: queue_low=0, enqueue_fail=0, slow_mix=0, max_mix <=5 ms. Source empty_queue counters alone do not prove output underrun. Keep audio unchanged.
- Corrected DXVK generated-present accounting: SnapFrame's result callback and HUD generated-frame counter now wait for the existing asynchronous WSI result in SynchronizePresent. This distinguishes a generated image being dispatched/queued from a completed successful WSI present. It does not prove compositor scanout or FPS uplift; verify the new `midpoint_present_accepted` event on device.


### Final runtime configuration from third upload

- `snapframe.conf`: disabled, flow scale 25% (the run itself includes an enabled 75% interval and an enabled 25% interval before final disable).
- `lsfg.conf`: enabled, multiplier 2. The LSFG runtime log records a temporary 1x setting followed by a return to 2x.
- `dxvk_perf_on.csv`: 48 samples through frame 5760; mean CPU-present interval 18.52 ms, P95 32.96 ms; mean generated requested/queued count 0.407. This is not a displayed-FPS or generated-present-acceptance measurement.

## Stutter-scene upload — 2026-11-04

The latest uploaded scene is 2448x1080, SnapFrame enabled at 75% flow scale. Across 130 GPU timestamp samples, total SnapFrame chain mean is 10.646 ms (P95 11.802 ms, max 14.243 ms); bidirectional flow accounts for about 8.22 ms mean. This is enough additional GPU work to be a serious performance suspect, but the CSV does not time-align the GPU samples to the observed game slowdowns. Flow confidence is low in 25 of 32 aggregate samples (<0.15 forward mean); mean boundary-saturated vectors are 616/5600. Keep the V1 search radius unchanged; implement coarse-to-fine V2 rather than blindly increasing the V1 radius.

The DXVK pass files show CPU PresentImage window-average mean/median/P95 of 24.22/26.72/45.65 ms for the OFF-labelled file and 18.52/18.73/31.40 ms for the ON-labelled file. Maximum single samples are 678.35/499.06 ms. Do not infer LSFG uplift: the uploaded config/log snapshot says LSFG disabled, SnapFrame is enabled, and the old CSV header falsely claimed runtime observation. The old queued-generated counter conflates LSFG and SnapFrame fallback output.

The LSFG A/B harness is now corrected to bypass SnapFrame capture and generation for both explicit passes. New DXVK CSV rows include per-window LSFG-ready and SnapFrame-ready frame counts, and the header reports the requested pass label without claiming to observe LSFG mode. Re-run A/B using the updated build; run a separate SnapFrame test with LSFG disabled. No device-side performance improvement is claimed yet.


## Fourth device log audit — difficult stutter scene — 2026-11-04

- Uploaded log set is from branch `Snap-AI-EngineFrame`. Runtime extent is 2448x1080, flow scale 75%, Turnip Gen8 V38. `snapframe.conf` ends with `enabled=1`; the runtime log starts with the host disabled and records a hot config update to enabled=1. The portable `SnapFrameBackendV1` adapter and DXVK copy hook are active.
- SnapFrame event CSV: 51 successful samples each for flow pair, consistency dispatch, midpoint warp, midpoint present request and WSI present acceptance. All recorded flow readbacks complete with finite values. The sampled progress log reaches 6,180 captured frames; no skip/failure is visible in the supplied SnapFrame event samples. This is not a complete end-of-session frame-loss certification.
- `snapframe_gpu.csv`: 206 successful samples per stage. Mean/median/P95/max total chain: 10.147/10.524/11.935/13.288 ms. Mean forward/backward flow: 3.842/3.824 ms; combined mean 7.666 ms. Capture copy 0.473 ms, luma 0.407 ms, consistency 0.011 ms, midpoint warp 1.584 ms. These are asynchronous GPU stage timings, not full game frame time.
- `snapframe_flow.csv`: 51 samples at frames 121–6121. Motion quality changes sharply by scene. Early/moving samples have low confidence and low forward/backward consistency; around frames 5521–6121 the field becomes nearly static/zero-motion and confidence/consistency rise to ~0.64–0.78. This can be a genuinely static interval or repeated/frozen source image; these aggregate records alone cannot distinguish them. Correlate source-image hashes/timestamps and game-frame intervals before classifying it as a game hang.
- `snapframe_flow_vectors.csv` is ~4.70 MB and the runtime log reports repeated 5,600-cell dumps at 600-frame intervals. Source review found that the full vector dump was triggered every 600 flow pairs even when `diagnostic_suite=0`. This caused avoidable per-dump formatting, file writes and flushes on the command-completion callback path. **Fixed in this branch:** full vector dumps now require `diagnostic_suite=1`; sparse aggregate flow telemetry remains available with `motion_analysis=1` and profiling. Device confirmation is pending.
- `lsfg.log` and `lsfg.conf` both show LSFG disabled for this run. `dxvk_lsfg_gpu.csv` contains only its header and `profiling_disabled`. Therefore this upload validates the SnapFrame fallback path, not an LSFG-on run or a valid DXVK/LSFG A/B comparison.
- The game-side slowdown/acceleration is not yet attributable from these files: the SnapFrame GPU chain is measured, but this upload has no valid same-run alignment between game frame interval, CPU `PresentImage`, LSFG GPU stages, and SnapFrame GPU timestamps. Do not infer thermal throttling, a CPU bottleneck, or FPS uplift from these logs.
- Next instrumentation priority: low-overhead per-window source-frame interval and CPU `PresentImage` P50/P95/max plus effective LSFG-ready/SnapFrame-ready counters, all on one monotonic frame/session timeline. Keep the existing WSI acceptance callback as the only source of accepted-generated-frame counts. Do not add synchronous GPU readback or waits.
- Next performance priority: keep V1 search radius unchanged; evaluate the isolated SnapFrame path with LSFG explicitly off, and LSFG A/B with SnapFrame bypassed. Compare identical scene paths and time-correlated counters. V2 remains un-dispatched; do not switch the runtime path before GPU resource/dispatch and output-parity validation.
