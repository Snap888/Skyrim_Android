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
- Latest Android CI run [#41817208666](https://github.com/Snap888/Skyrim_Android/actions/runs/41817208666) passed portable API tests, Snap AI/SnapFrame ARM64 shared-library build, DXVK SnapFrame host ARM64 compile and Java compilation. The full APK job was skipped because `build_apk` was not requested.
- A safety backup was created before this follow-up work: [backup/Snap-AI-EngineFrame-pre-2026-11-04-logs2](https://github.com/Snap888/Skyrim_Android/tree/backup/Snap-AI-EngineFrame-pre-2026-11-04-logs2).

The active Skyrim runtime remains V1. The portable MotionV2 core is linked, and its V2 shader sources are compiled into the generated shader header and validated by CI. The V2 GPU resources, descriptor bindings, dispatch sequence and async readback are not yet connected to DXVK; Snap AI adaptive control remains unavailable. Do not treat shader compile validation or queued generate commands as proof of actual frame generation.



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
- Audio output mixer remains healthy in the log: queue_low=0, enqueue_fail=0, slow_mix=0, max_mix <=5 ms. Source empty_queue counters alone do not prove output underrun. Keep audio unchanged.
- Corrected DXVK generated-present accounting: SnapFrame's result callback and HUD generated-frame counter now wait for the existing asynchronous WSI result in SynchronizePresent. This distinguishes a generated image being dispatched/queued from a completed successful WSI present. It does not prove compositor scanout or FPS uplift; verify the new `midpoint_present_accepted` event on device.


### Final runtime configuration from third upload

- `snapframe.conf`: disabled, flow scale 25% (the run itself includes an enabled 75% interval and an enabled 25% interval before final disable).
- `dxvk_perf_on.csv`: 48 samples through frame 5760; mean CPU-present interval 18.52 ms, P95 32.96 ms; mean generated requested/queued count 0.407. This is not a displayed-FPS or generated-present-acceptance measurement.

## Stutter-scene upload — 2026-11-04

The latest uploaded scene is 2448x1080, SnapFrame enabled at 75% flow scale. Across 130 GPU timestamp samples, total SnapFrame chain mean is 10.646 ms (P95 11.802 ms, max 14.243 ms); bidirectional flow accounts for about 8.22 ms mean. This is enough additional GPU work to be a serious performance suspect, but the CSV does not time-align the GPU samples to the observed game slowdowns. Flow confidence is low in 25 of 32 aggregate samples (<0.15 forward mean); mean boundary-saturated vectors are 616/5600. Keep the V1 search radius unchanged; implement coarse-to-fine V2 rather than blindly increasing the V1 radius.




## Fourth device log audit — difficult stutter scene — 2026-11-04

- Uploaded log set is from branch `Snap-AI-EngineFrame`. Runtime extent is 2448x1080, flow scale 75%, Turnip Gen8 V38. `snapframe.conf` ends with `enabled=1`; the runtime log starts with the host disabled and records a hot config update to enabled=1. The portable `SnapFrameBackendV1` adapter and DXVK copy hook are active.
- SnapFrame event CSV: 51 successful samples each for flow pair, consistency dispatch, midpoint warp, midpoint present request and WSI present acceptance. All recorded flow readbacks complete with finite values. The sampled progress log reaches 6,180 captured frames; no skip/failure is visible in the supplied SnapFrame event samples. This is not a complete end-of-session frame-loss certification.
- `snapframe_gpu.csv`: 206 successful samples per stage. Mean/median/P95/max total chain: 10.147/10.524/11.935/13.288 ms. Mean forward/backward flow: 3.842/3.824 ms; combined mean 7.666 ms. Capture copy 0.473 ms, luma 0.407 ms, consistency 0.011 ms, midpoint warp 1.584 ms. These are asynchronous GPU stage timings, not full game frame time.
- `snapframe_flow.csv`: 51 samples at frames 121–6121. Motion quality changes sharply by scene. Early/moving samples have low confidence and low forward/backward consistency; around frames 5521–6121 the field becomes nearly static/zero-motion and confidence/consistency rise to ~0.64–0.78. This can be a genuinely static interval or repeated/frozen source image; these aggregate records alone cannot distinguish them. Correlate source-image hashes/timestamps and game-frame intervals before classifying it as a game hang.
- `snapframe_flow_vectors.csv` is ~4.70 MB and the runtime log reports repeated 5,600-cell dumps at 600-frame intervals. Source review found that the full vector dump was triggered every 600 flow pairs even when `diagnostic_suite=0`. This caused avoidable per-dump formatting, file writes and flushes on the command-completion callback path. **Fixed in this branch:** full vector dumps now require `diagnostic_suite=1`; sparse aggregate flow telemetry remains available with `motion_analysis=1` and profiling. Device confirmation is pending.


## Frame-generation backend consolidation — 2026-11-04

- Removed the legacy frame-generation backend and its Vulkan/device hooks, DXVK callbacks, native bootstrap, JNI settings bridge, and in-game menu.
- SnapFrame is now the sole frame-generation path. DXVK still owns source-frame presentation, SnapFrame capture/generation callbacks, generated-image blit, asynchronous WSI acceptance accounting, and the existing frame-latency synchronization.
- Removed legacy backend source, build target, historical backend-specific logs, and backend-specific documentation/tools from this branch. The Git history and pre-change backup branch remain available for audit/rollback.
- No change was made to `SyncFrameLatency()` or the normal WSI submission order. The old backend-specific game-FPS sleep limiter was removed; pacing optimization will be implemented as SnapFrame/DXVK policy only after frame-interval telemetry.
- Next: finish ARM64 compile/CI, then connect frame-tagged MotionV2 GPU metrics to Snap AI adaptive control. V2 remains disabled in runtime until dispatch/resource lifecycle and output parity pass device validation.


## Post-LSFG consolidation and AI implementation checkpoint — 2026-11-04

- Active branch head before this documentation checkpoint: `bd28e9d5977443f9adf0b97443ff5396c92cae68`. LSFG has been removed from the active branch; SnapFrame is the only frame-generation path.
- Commit `cbfb9f5c74fa61a90f4cca3f97c81b1f79a1b5d9` adds `docs/SNAP_AI_GPU_METRICS_IMPLEMENTATION.md`, defining the V1-compatible two-pass GPU reduction, frame-tagged asynchronous readback, MotionV2 publication, AI metric mapping and validation gates.
- CI run [#41861458011](https://github.com/Snap888/Skyrim_Android/actions/runs/41861458011): Java compilation, portable Snap AI/SnapFrame ARM64 build, portable API tests and DXVK SnapFrame host compilation all completed their build steps successfully. The DXVK job/run conclusion was still reported as in progress by GitHub at the time of this checkpoint. The full APK job was skipped.
- Runtime status is unchanged: SnapAI is not yet active in Skyrim. V1 flow vectors are still read back only for sparse diagnostics; they are not AI inputs. V2 shader SPIR-V compilation does not mean V2 resources or dispatch are active.
- The next code milestone is Phase A in `docs/SNAP_AI_GPU_METRICS_IMPLEMENTATION.md`: implement V1-compatible GPU-side partial/final reduction and an asynchronous scalar readback ring. Only after frame-tagged metrics pass validation should MotionV2 publication and SnapAI decisions be connected.
- Do not advertise adaptive AI, AI-controlled Flow Scale, V2 motion estimation or FPS improvement as implemented until runtime/device validation confirms them.


## V1 GPU metrics runtime implementation — 2026-11-04

- Added local V1-compatible partial/final reduction shaders under `android/app/jni/snapframe/shaders/` and included them in `tools/compile_snapframe_shaders.py`.
- Added DXVK context methods for the two-stage metrics dispatch while preserving/restoring the game's compute pipeline state and descriptor slots.
- Added host-side GPU buffers, three-slot asynchronous scalar readback ring, frame/timestamp/extent validation and MotionV2 metric publication on command-list completion.
- Metrics are sampled every 30 captured source frames. The output contains motion magnitude, local flow complexity, bidirectional confidence, low-confidence ratio, inconsistency ratio, search-boundary ratio, artifact-risk heuristic and valid-cell coverage. The readback record is 80 bytes.
- No static mask or calibrated scene-cut detector exists in the active V1 path. Static ratio is set to zero and scene_cut is not asserted; V2-only resources are not bound to V1.
- No SnapAI controller decisions are applied yet. Same-frame GPU-time correlation, device calibration, and quality/pacing validation remain required before any adaptive settings are enabled.
- The implementation is not yet CI/device validated. The latest branch commits have triggered Android CI runs; inspect the newest run before treating the code as buildable.


## DXVK / Vulkan present scheduling — 2026-11-04

- Audited SDL window/event integration versus the actual DXVK → Vulkan WSI image-present path. SDL is not the final image presenter; D3D9SwapChainEx and Vulkan Presenter own image acquisition, blit, queue submission and WSI presentation.
- Commit `b9a9c6ffd233e5957db0a3c9052ce6bc83f09018` introduces independent source/generated `DxvkSubmitStatus` objects and removes the intermediate CPU `SynchronizePresent()` wait before generated-frame recording/submission.
- Queue order remains source command list → source WSI present → generated command list → generated WSI present. Pending statuses are drained at the next frame boundary and before swapchain teardown/recreation.
- This is a scheduling change, not proof of GPU overlap or higher FPS. Source and generated work still share the graphics queue; measure WSI acquire duration, submit/present wait duration, FG GPU time, frame-latency wait and source FPS on device.
- CI for the change is queued (run [41878691981](https://github.com/Snap888/Skyrim_Android/actions/runs/41878691981)); do not treat it as compile-validated until the job completes. No physical-device validation has been performed.


## DXVK CPU present timing instrumentation — 2026-11-04

- Commit `aa5a5e08ebeb675a5bd75bb98240e0d4c012ff2a` extends the existing 120-frame `dxvk_perf.log` record with source/generated WSI acquire, submit enqueue, generated command-recording, previous-present retirement wait, and frame-latency wait averages.
- All new measurements are CPU wall-clock durations. They do not insert queue/device idle or GPU completion waits. Submit enqueue and generated record values must not be interpreted as GPU execution time.
- The log still uses the existing DXVK performance log path selection (`DXVK_PERF_LOG` or `DXVK_LOG_PATH/dxvk_perf.log`); no new runtime environment variable is required if the app already configures its DXVK log directory.
- Workflow run [41880070147](https://github.com/Snap888/Skyrim_Android/actions/runs/41880070147) for this instrumentation is queued. Earlier run [41877312213](https://github.com/Snap888/Skyrim_Android/actions/runs/41877312213) passed Android Java, portable ARM64, API tests, and DXVK host ARM64 compile, but predates the latest present-slot and timing commits. Full APK remains skipped in those validation workflows.
- No device A/B results exist yet. Collect at least 3 matched 120-frame windows with SnapFrame OFF and ON in the same scene, resolution, refresh/VSync and camera path. Compare median/average source FPS and the new CPU timing columns; correlate SnapFrame GPU timings separately by frame ID.


## WSI acquire-ticket safety correction — 2026-11-04

- Code commit: `8702981ed6bdb41390f44f236652d8f733161056`.
- Review found that the current Vulkan Presenter exposes a single mutable acquire token (`m_acquireStatus`, `m_imageIndex`, `m_frameIndex`). Since source `presentImage()` advances this state asynchronously on the DXVK CS thread, the generated acquire could race and reuse the source token/image.
- Restored `SynchronizePresent()` immediately before generated WSI acquisition. This is a temporary correctness barrier; it deliberately gives up the previous attempted CPU-side overlap until explicit per-acquire tickets are implemented.
- Independent source/generated submission status objects remain in place, but must not be confused with independent WSI acquire ownership.
- The current status checks endpoint returned no checks for the documentation checkpoint; the safety correction is not yet compile-validated or device-validated. Do not claim an FPS improvement.
- Next engineering task: redesign Presenter around an explicit acquire ticket containing swapchain generation, image index, acquire semaphore and present semaphore. Ensure the ticket is consumed exactly once by the matching queued present operation; reset/recreate must invalidate outstanding tickets only after submission retirement. Then test WSI semaphore reuse and queue ordering before removing the temporary barrier.


## Bounded WSI acquire recovery — 2026-11-04

- Code commit `241d00525003de96d4c2ba4f2eadd83519d8e910` replaces unbounded source/generated acquire retry loops with at most three attempts, logging each VkResult and recreating the swapchain between attempts.
- Persistent source acquire failure exits PresentImage without submitting work against an unacquired image. Persistent generated acquire failure reports FG rejection and skips the generated submission.
- This is a WSI failure-path reliability improvement only; no FPS gain is claimed.
- The temporary source-to-generated synchronization barrier remains required with the current single mutable Presenter acquire state. Ticket-based acquire/present ownership is still future work.
- CI and physical-device validation are pending for this code.


## WSI acquire-ticket redesign specification — 2026-11-04

- Added `docs/WSI_ACQUIRE_TICKET_DESIGN.md` (commit `4349770334131698ec9e8960e9519d95acc33fad`).
- Re-audited the current Presenter and D3D9 call path. The acquire state is mutable and singular (`m_frameIndex`, `m_imageIndex`, `m_acquireStatus`); `presentImage()` runs on the DXVK CS thread, queues WSI present, advances the frame slot and pre-acquires the next image.
- The source/generated status slots track queued submission retirement, but do not independently own acquired WSI images. The current source-to-generated `SynchronizePresent()` barrier therefore remains mandatory.
- The design document specifies a move-only generation-tagged acquire ticket, exact ticket propagation through the CS-thread closure, single-consume semantics, image-owned present semaphore retirement, acquire-slot retirement, safe cancellation, and swapchain teardown/recreate rules.
- **No ticket API or overlap optimization is claimed as implemented.** This change is a correctness specification to prevent an unsafe partial refactor. Keep the current barrier until the staged implementation and Android validation gates pass.
- Next code stage: inventory all Presenter callers and queue/thread ownership, then implement ticket plumbing while retaining the barrier. Only after ticket/semaphore retirement is proven should a controlled overlap experiment be enabled.


## Presenter acquire-ticket design review — 2026-11-04

- Re-read the active Presenter and D3D9 present implementation. `acquireNextImage()` and `presentImage()` share mutable frame/image/acquire state; `presentImage()` also pre-acquires the next image on the DXVK CS thread.
- Confirmed that waiting for the CS submission status only waits for the queued `vkQueuePresentKHR` call to return. It does not establish that the presentation engine has consumed the binary present-wait semaphore.
- A ticket that merely copies image/semaphore handles is not sufficient. Independent outstanding acquisitions, one-time ticket consumption, swapchain-generation checks, and Vulkan-guaranteed present-semaphore retirement are all required.
- Added the detailed state model, implementation sequence and acceptance criteria to `docs/DXVK_PRESENT_PIPELINE_AUDIT.md`.
- No risky Presenter API or semaphore-lifetime rewrite was committed in this step. The temporary source-to-generated `SynchronizePresent()` barrier remains enabled. Do not remove it until the ticket implementation passes compile, ownership and device validation.
- This step is a correctness/documentation checkpoint, not a runtime optimization. CI/device validation and FPS improvement are not claimed.


## Bounded WSI acquire recovery — CI result update (2026-11-04)

- Android CI run [#41881351878](https://github.com/Snap888/Skyrim_Android/actions/runs/41881351878), on descendant commit `dddc8453a7d77ac095ba07a0f57cebc17977ccb5` containing the bounded-acquire code commit `241d00525003de96d4c2ba4f2eadd83519d8e910), completed successfully.
- Confirmed successful jobs: DXVK SnapFrame host ARM64 compile, portable Snap AI/SnapFrame ARM64 build, portable API tests, and Android Java compilation.
- Full ARM64 APK job was skipped; this run validates the native host compilation and portable targets, not a distributable APK.
- The later acquire-ticket review commits are documentation-only and do not change runtime code. No physical-device WSI validation or FPS A/B result is available.


## External LSFG removal and SnapFrame HUD repair — 2026-11-04

- Removed the external Lossless Scaling UI from the Android launcher: DLL selection/import, multiplier/quality/mode controls, LSFG enable toggle, LSFG-specific GPU profiler and LSFG-labelled A/B pass.
- Replaced the old A/B test with a real SnapFrame OFF/ON DXVK benchmark. OFF sets the explicit SnapFrame bypass; ON clears it and exercises the active SnapFrame pipeline. Output files are `port/snapframe_perf_off.csv` and `port/snapframe_perf_on.csv`.
- Removed the old LSFG configuration writer and profiler setup. Before launching the game, the launcher deletes only retired generated artifacts (`lsfg.conf`, `lsfg_gpu_profile.conf`, `dxvk_lsfg_gpu.csv`, `dxvk_perf_on.csv`, `dxvk_perf_off.csv`) from the selected game's `port/` directory and unsets the old LSFG profiler environment variables. It does not delete arbitrary files or user data.
- Fixed the HUD's zero FPS counters: the old HUD counter functions had no callers after backend consolidation. Source FPS now counts only source WSI presents accepted by Vulkan; generated FPS counts only generated WSI presents accepted by Vulkan; output FPS is their sum. HUD symbols and labels now use SnapFrame terminology.
- The HUD values represent accepted Vulkan WSI present operations, not compositor scanout or physical display FPS.
- Updated Android settings and SnapFrame integration documentation. Android CI and device validation for these changes are pending. No FPS gain is claimed.


## Performance optimization and Snap AI wiring — 2026-11-04

### Implemented in this pass

- Block-matching flow refinement was reduced from a 5x5 to a 3x3 neighborhood around the coarse winner. Worst-case candidate evaluations fall from 49 to 33 per flow cell and direction (about 33% fewer candidates), while the coarse +/-8 search lattice remains. This is a measured operation-count reduction, not a measured FPS gain; sub-grid precision is lower and needs Skyrim scene validation.
- Midpoint warp now avoids the two displaced color fetches when confidence is effectively zero and writes the current real frame directly. This saves memory sampling in low-confidence/occluded regions; actual GPU gain depends on scene coverage and branch coherence.
- Snap AI is now instantiated in the Android host and linked through `SnapAI::Engine`.
- Validated frame-tagged flow metrics are staged asynchronously. On the next serialized render callback, the host combines them only with an exact-frame GPU total-chain timestamp and invokes `snap_ai_update()`.
- Sparse total-chain timestamps are collected every 30 frames even when verbose GPU profiling is disabled. Runtime timing publication no longer depends on the optional GPU CSV being open.
- Only AI `flow_scale` is currently applied, clamped to 50..75%, with 8-frame warmup, 6 ms target budget and 8 percentage-point hysteresis. Other decision fields remain advisory.
- AI changes are applied on the serialized DXVK callback; history reset is requested there. No Vulkan resource mutation or WSI wait occurs in the asynchronous metrics callback.

### Not yet implemented / safety gates

- Multi-level luma pyramid and coarse-to-fine flow propagation are not yet integrated into the live dispatch path. Current flow still operates on a single half-resolution luma level.
- Existing forward/backward consistency mask is an occlusion-confidence proxy, not a dedicated CronyFG-style disocclusion mask. Dedicated mask generation and warp compositing are not yet implemented.
- x3/x4 temporal phase generation is not implemented. The live backend still owns one midpoint image and queues one generated present per source frame.
- WSI ticket ownership and semaphore retirement are not implemented. Keep `SynchronizePresent()` enabled; do not increase generated present count or remove the barrier until ticket ownership and retirement are validated.
- No source FPS improvement or output FPS improvement is claimed. Compare matched device captures after the ARM64 build and test.

### Next implementation order

1. Verify this AI wiring compiles and that exact-frame timing + metrics pairs appear in logs.
2. Add a three-level luma pyramid with explicit per-mip image views, then coarse-to-fine vector propagation (coarse field seeds finer search).
3. Split forward/backward consistency into a validated occlusion/disocclusion mask and make warp avoid blending across invalid regions.
4. Add phase-parametric warp outputs for t=k/M (M=3,4), then implement multiple output image lifetimes and one-shot WSI tickets.
5. Convert Presenter to generation-bound acquire tickets and per-image present semaphore retirement. Only then allow multiple generated WSI presents and test source FPS/frame pacing.


### AI Flow Scale transition pacing — 2026-11-04

Adaptive Flow Scale changes force SnapFrame history/resource recreation because the flow-grid dimensions depend on the selected scale. The host now enforces a 120-source-frame cooldown between applied AI scale changes. This avoids rapid controller oscillation repeatedly destroying/reallocating the luma/flow resources and resetting temporal history. The new scale is applied at the start of the serialized frame callback; the pending reset is consumed before capture/dispatch, so the current frame does not dispatch against images allocated for the previous scale. The cooldown is a stability guard, not a measured performance gain.


### AI resource-transition pacing — 2026-11-04

A Flow Scale update changes the flow-grid extent and therefore requires history/flow resource recreation. Adaptive scale changes are now rate-limited to one per 120 source frames. The scale update is staged/applied at the beginning of the serialized host callback; its pending history reset is consumed before that callback allocates or dispatches resources, avoiding dispatch against the previous scale's image extent. This cooldown is a stability measure; it has not yet been shown to improve average FPS.

The V2 shader sources are available in the pinned Snap AI Engine dependency and are compiled/validated by the shader generation script. They include L0→L1 downsample, coarse/fine search, refinement, forward/backward validation, static-region mask and time-parametric interpolation. However, source compilation is not runtime integration: Skyrim's live host still allocates and dispatches the V1 single-level luma/flow/consistency/one-midpoint path. The next implementation task is to add DXVK descriptor/push-constant dispatch wrappers and generation-safe per-level image resources before enabling the V2 kernels.


### SnapMotion V2 DXVK dispatch foundation — 2026-11-04

Added `DxvkContext::dispatchSnapFrameV2`, a bounded generic set-0 image/sampler compute dispatch entry point for the V2 shader family. Binding-array indices map directly to descriptor binding numbers; null sampler entries represent storage-image bindings. The helper validates binding count (maximum six, matching the current V2 kernels), workgroup dimensions and push-constant payload (maximum 128 bytes), records push constants before dispatch, reports dispatch acceptance, and restores the touched DXVK resource slots plus cached compute state exactly like the V1 wrappers.

This is an API foundation only. No V2 shader is dispatched by the live host yet, and no new pyramid images/views have been allocated. Runtime activation remains gated on adding per-level resources, explicit image barriers/layout validation, per-kernel push-constant structs, and device testing. The helper must pass ARM64 DXVK compilation before it is used by the host.


### DXVK V2 dispatch validation — 2026-11-04

GitHub Actions run `41902453483` for commit `255750b2b0d5868686a4a1ce5fc94e9739706f5c` completed successfully:
- Skyrim DXVK SnapFrame host ARM64 compile: success
- Snap AI and SnapFrame portable API tests: success
- Android Java compilation: success
- Snap AI + SnapFrame portable ARM64 build: success
- Full arm64 APK build: skipped by workflow

This validates compilation of the generic V2 dispatch API and push-constant state isolation, but does not validate runtime execution of V2 shaders or produce a tested APK.

## SnapMotionV2 runtime: first live pyramid passes — 2026-11-04

- Added a separate per-temporal-slot V2 luminance pyramid to the DXVK host. It allocates six independent R16_SFLOAT images and six matching views: L0, L1 and L2 for each of the two real-frame history slots. The existing V1 luma/flow resources are not aliased or repurposed.
- L0 is generated from the real captured source image at half source resolution using the upstream V2 Rec.709 luma compute shader. Two real V2 downsample dispatches then build L0→L1 and L1→L2 using the upstream 2×2 box filter. Odd dimensions use ceil division at each level.
- Each pass uses the generic DXVK V2 dispatch API, explicit sampled-image/storage-image descriptors and a 16-byte push-constant ABI. Compute barriers are recorded between dependent passes and after L2 so later consumers see the completed pyramid. Resources use VK_IMAGE_LAYOUT_GENERAL with compute read/write access tracking; image objects and views are separate per level.
- V2 pass failure is non-fatal to the existing V1 path: the legacy luma/flow/warp pipeline continues and the real game frame remains the fallback. V2 dispatch success/failure is logged as sparse events.
- This is the first V2 shader execution path wired into the DXVK runtime, but CI and device execution still need to confirm descriptor reflection, image layout transitions and output values. Do not call it device-validated yet.
- Not implemented in this commit: L2/L1/L0 coarse-to-fine motion search, inter-level vector upscaling/seeding, refinement/forward-backward occlusion confidence, V2 interpolation, and ×3/×4 Present scheduling.
- ×3/×4 phase contract for the next stage: for multiplier M, generate exactly M−1 intermediate time samples t=i/M for i=1..M−1 between two real frames. Each generated frame must consume the same frame-pair identity/history and its own time value; no generated frame may become a source-history frame. Present acceptance and pacing must remain separate from dispatch completion. ×3/×4 stays disabled until those conditions and WSI semaphore ownership are proven.


## SnapMotionV2 coarse-to-fine flow — 2026-11-04

- Added independent RGBA16F flow images/views for two motion directions and three pyramid levels.
- Registered the V2 search compute shader with a 48-byte push-constant ABI.
- After both real-frame pyramids are valid, the host dispatches bidirectional L2 → L1 → L0 search. L2 starts without a seed; L1 uses the same direction's L2 field; L0 uses its L1 field. Motion vectors stay in L0 pixel units and `pixelScale` (4, 2, 1) adjusts sampling for each level.
- Compute barriers separate dependent levels. V2 failures remain non-fatal; the V1 flow/warp path remains authoritative.
- V2 grid dimensions use ceil(level extent / 8), matching the shader's fixed 8×8 block. The V1 adaptive `flowExtent()` must not be reused because it applies Flow Scale and would leave V2 image regions unprocessed.
- Previous-frame temporal seeding, refinement, forward/backward confidence/occlusion mask, V2 interpolation, AI-controlled V2 search density and ×3/×4 presentation are not implemented yet. V2 search is experimental and may cost more than V1; measure on device before enabling broadly.
- Validation required: shader compile, ARM64 host build, Vulkan descriptor/layout validation, and device logs confirming `v2_flow_pair_ok` with correct image coverage.


## Latest WSI device run — 2026-11-05 (zero-timeout acquire)

The latest uploaded device logs use 2448x1080, flow scale 75% (112x50 V1
motion grid), motion analysis enabled, profiling disabled and diagnostic
suite disabled. V2 diagnostic work is therefore not expected to contribute
to the production GPU load.

The user reports a small increase in clean/source game FPS after the
non-blocking WSI pre-acquire change. The performance CSV is consistent with
a partial improvement, but not a complete fix:

- The presenter handoff average starts near 1 ms and later rises to roughly
  13–18 ms. The source-to-generated synchronization remains a material cost.
- Generated presents are initially accepted, but the event stream records
  repeated `midpoint_present_failed` events from around frame 287 onward.
  This indicates that the generated path frequently falls back to the source
  frame when an additional WSI present is not accepted.
- Generated queue averages decline in later windows (about 0.72–0.75 in the
  final samples). Requested count remains close to 1, so this is not a
  reduction in generation demand; it is WSI admission/back-pressure.
- GPU stage CSV contains only its header because profiling is disabled.
  No optical-flow or warp GPU cost should be inferred from this run.
- Scalar reduction telemetry remains invalid: logs report
  `valid_cells=128`, `expected_cells=5600`, but `coverage=1.000`.
  Keep the Snap AI 90% valid-coverage gate; do not use these samples for
  adaptive Flow Scale decisions.

### Instrumentation added after this upload

The SnapFrame present-result callback now carries the actual Vulkan
`VkResult` in addition to the accepted flag. Event details will include
`result=<numeric VkResult>`, distinguishing a zero-timeout acquire miss
(`VK_NOT_READY`/`VK_TIMEOUT`), a present failure, and a generation failure
(`VK_ERROR_UNKNOWN`). This is diagnostic instrumentation only; it does not
change the WSI retry or semaphore policy.

### Engineering decision

The zero-timeout policy appears to improve source-frame throughput at the
cost of skipped generated frames. Do not restore infinite-timeout generated
acquire: that would reintroduce source-thread stalls. Do not remove
`SynchronizePresent()` while Presenter still has one mutable outstanding
acquire state. Continue toward per-acquire tickets carrying swapchain
generation, frame slot, image index, acquire semaphore and present semaphore,
with exactly-once consumption and safe retirement.

The next run should confirm the numeric failure reasons and compare source
FPS, generated queued rate and presenter handoff wait. Build and device
validation of the result-code instrumentation are pending.


### Follow-up WSI timing split

The performance CSV now separates the two serialized waits:
- `avg_source_handoff_wait_ms`: wait before acquiring/submitting the next real source frame; it can include completion of the previous generated present.
- `avg_generated_handoff_wait_ms`: wait after the current source present is queued and before attempting its generated frame; this isolates the source-present-to-generated handoff.
  
This replaces the combined `avg_presenter_handoff_wait_ms` field. It is instrumentation only and preserves the existing synchronization and semaphore ownership. Use both columns with the per-event Vulkan result code to decide whether the dominant cost is prior generated completion or the current source present.


## WSI regression investigation — 2026-11-05 latest run

The latest uploaded run is on 2448x1080, V1 flow grid 112x50
(`flow_scale_percent=75`), VSync enabled, profiling disabled.

Observed performance:
- CPU Present rises from about 1 ms in the first 120-frame window to
  27–32 ms in windows 1200–2400.
- The previously added combined source handoff metric is similarly high
  (about 25.7–30.5 ms) in those windows.
- Source acquire itself remains near 0.0008 ms in the affected windows.
- Generated handoff is around 0.64–0.75 ms in the same windows.
- Generated queue acceptance later collapses in other windows (about
  0.07–0.17 average), with repeated midpoint present failures.
- The event's numeric result 1 maps to `VK_NOT_READY`. It must not be
  described as a shader/flow failure. The generated present is not being
  accepted by the current WSI path.

The old `avg_source_handoff_wait_ms` was an aggregate timer around
`SynchronizePresent()`, which calls `WaitForPresentSubmissions()` and can
wait for both source and generated submissions. Its name therefore did not
identify which queue entry caused the stall.

### New instrumentation

The performance CSV now adds:
- `avg_source_submission_wait_ms`: time spent waiting for the source
  submission status to resolve.
- `avg_generated_submission_wait_ms`: time spent waiting for the generated
  submission status to resolve.

The aggregate handoff columns remain for continuity. This is diagnostic-only;
it does not change WSI scheduling, acquire state, semaphores or present order.

### Current engineering direction

Do not remove the source/generated synchronization based on aggregate timing.
The Presenter still has one mutable acquire slot (`m_frameIndex`,
`m_imageIndex`, `m_acquireStatus`) and a shared semaphore ring. The safe
next step remains per-acquire tickets, but first use the new split wait
measurements to establish whether the 25–30 ms stall is the source submission
or generated submission. If the generated submission dominates, a ticketed
handoff can remove that wait from the source-frame critical path without
reusing the present/acquire semaphore prematurely.

The current log has no GPU stage timings because profiling is disabled.
The scalar telemetry mismatch also remains unresolved
(`valid_cells=128`, `expected_cells=5600`, `coverage=1.000`); keep it
out of AI tuning until its reduction contract is fixed.

### Source-only FPS limiter correction — 2026-11-05

Code inspection found that both source and generated SnapFrame presents called
`Presenter::presentImage()`, which also called the shared `FpsLimiter::delay()`
on the serialized DXVK submission thread. With a non-zero DXVK frame-rate cap
(and when VSync/refresh matching does not disable the limiter), the generated
present could therefore consume a second limiter interval and delay subsequent
source-frame submission. This is independent of optical-flow shader cost.

The present path now carries an explicit `applyFrameRateLimit` flag:
- D3D9 source presents pass `true`.
- SnapFrame generated presents pass `false`.
- Other existing Presenter callers retain the default `true`.

This preserves source frame pacing while preventing generated output frames
from advancing or sleeping the source FPS limiter. The flag is explicit at the
source/generated call sites; it is not inferred from a frame index.

This is a targeted pacing correction, not an acquire-ticket implementation.
The shared Presenter acquire state and semaphore ring remain serialized and
`SynchronizePresent()` remains in place. It must be tested on device with the
same scene and settings, comparing source FPS, generated output FPS,
`avg_source_submission_wait_ms`, `avg_generated_submission_wait_ms`, and
`avg_frame_latency_wait_ms`. The previous timing-only build passed CI, but
this change requires its own CI result and runtime validation.
