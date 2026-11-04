# Snap AI GPU metrics integration plan

Last reviewed: 2026-11-04
Target branch: `Snap-AI-EngineFrame`

## Current verified baseline

- The active Skyrim frame-generation path is SnapFrame V1 through `SnapFrameBackendV1`.
- The V1 host currently owns real DXVK luma, bidirectional block-flow, forward/backward consistency and midpoint-warp resources.
- Sparse CPU flow readback is diagnostic-only. It must not be used as an input to Snap AI.
- `SnapMotionV2` and `SnapAI::Engine` are linked portable libraries, but the Skyrim runtime does not yet publish GPU-reduced motion metrics or call the AI controller.
- The V2 shader assets are compiled/validated by the Android host build, but their V2-specific inputs (refined flow, validation mask, static mask and luma pyramid) are not dispatched by the V1 runtime. In particular, the V1 consistency mask is not a substitute for either the V2 validation image or static mask.
- CI run [41861458011](https://github.com/Snap888/Skyrim_Android/actions/runs/41861458011), head `bd28e9d5977443f9adf0b97443ff5396c92cae68`: Java compilation, portable ARM64 libraries, portable API tests and the DXVK/SnapFrame host compile all reached successful build steps. The DXVK job's final job/run conclusion was not yet published at this review; the full APK job was skipped.

## Phase A — V1-compatible GPU scalar reduction

The first runtime metrics pass must work with the currently dispatched V1 pipeline. It must not bind V1 resources to V2 shader slots merely because the generated shader header contains V2 SPIR-V.

### Inputs

- Forward flow image: `RGBA16F`, where `xy` is motion, `z` is SAD separation confidence and `w` is normalized best-match cost.
- Backward flow image: same format and dimensions.
- Forward/backward consistency image: `R16F`, where values above the V1 validity threshold indicate a consistent match.
- The frame's current and previous half-resolution luma images, when a photometric scene-change candidate is required.

The V1 consistency image is only a forward/backward consistency signal. It is not a semantic/static-region mask.

### Reduction output

Use a two-pass compute reduction:

1. **Partial pass:** one 8x8 workgroup processes one flow-grid tile and writes a compact partial record. Reduce in workgroup shared memory; do not use floating-point atomics. Suggested fields: valid-cell count, mean motion magnitude sum, local flow-gradient/complexity sum, confidence sum, low-confidence count, consistency-valid count, search-boundary count and luma-delta sum.
2. **Final pass:** one workgroup reduces the small partial array into one fixed-size scalar record. Include source frame ID, monotonic timestamp, source extent, flow extent, valid-cell coverage, mean magnitude, normalized complexity, mean confidence, low-confidence ratio, inconsistency ratio, boundary ratio and photometric delta. Keep `scene_cut` false until a separately validated detector is available; a heuristic candidate must not silently reset history.

The first pass should use the existing V1 images only. It must not pretend that a static mask, refined flow or V2 validation pass exists.

### Readback and lifetime

- Copy only the final scalar record (target: no more than 64 bytes) to a host-visible coherent staging buffer.
- Use a small readback ring. A slot may be reused only after its completion callback has fired; do not overwrite a buffer still referenced by an in-flight command list.
- Consume the mapped record only in the command-list completion callback. Reject failed completion, non-finite values, invalid dimensions, impossible counts, coverage below 90%, duplicate/out-of-order frame IDs and records older than the allowed frame lag.
- The callback publishes a frame-tagged `SnapMotionV2Metrics` record. It must not call `waitIdle`, `vkDeviceWaitIdle`, queue-idle, or block the DXVK Present thread.
- Resource allocation is resolution/device scoped and happens alongside the existing history resources. Allocation/dispatch failure disables metrics only; it must not disable real-frame presentation or midpoint fallback.
- Keep diagnostic full-vector readback behind the existing explicit diagnostics setting. The AI path must never use it.

## Phase B — SnapMotionV2 and SnapAI mapping

Only after Phase A is live and frame identity is verified:

1. Publish each valid GPU record through `snap_motion_v2_publish_metrics`.
2. Retrieve only a record with `snap_motion_v2_get_metrics(current_frame_id, max_frame_lag, ...)`; discard stale records.
3. Build `SnapAIFrameMetrics` using the same source-frame identity. Map motion magnitude, complexity, confidence and artifact risk from the reduced record. GPU time must be associated with that same frame; if no matching completed timestamp sample exists, do not fabricate one or pair a different frame's timing.
4. Call `snap_ai_update`. Invalid metrics, stale IDs, scene transitions, controller errors or disabled state must yield bypass.
5. Apply decisions on the next serialized DXVK/SnapFrame callback, never from the completion thread. Clamp every decision to the capabilities actually implemented by V1.
6. Initially apply only the existing flow-scale control, and only at a safe history-reset boundary. Do not apply search-radius, refinement-pass, reuse-flow or bidirectional-flow decisions until the V1 host genuinely supports those controls. Do not recreate GPU resources every frame.

## Phase C — validation gates

Before enabling adaptive decisions by default:

- Unit-test partial/final reduction math against a CPU reference using synthetic static, translational, ambiguous, occluded and non-finite fields.
- Test frame IDs, out-of-order callbacks, stale records, ring-buffer exhaustion, device destruction, resolution changes and reset epochs.
- Confirm the completion callback never blocks Present and that a missed metrics sample leaves the original SnapFrame output unchanged.
- Add log fields for metrics validity/rejection reason, source frame ID, record age and AI decision reason mask.
- Run a fixed-scene device test at each supported Flow Scale. Compare SnapFrame OFF/ON and AI bypass/adaptive with the same resolution, scene, duration and device state.
- Keep AI disabled by default until image quality, frame pacing, sound and controls pass device validation.

## Explicit non-goals for the first pass

- No neural-network inference is present in `SnapAI::Engine`; it is a bounded heuristic adaptive controller.
- No V2 motion estimator migration in this phase.
- No use of V2 static-mask/validation shaders with V1 images.
- No full-resolution or full-flow CPU readback for AI.
- No GPU/queue/device waits, per-frame resource allocation, or unmeasured performance claims.
- No change to the source-frame Present path, audio, touch/gamepad controls or generated-present acceptance semantics.


## Performance-first runtime baseline update — 2026-11-04

The user's controlled A/B report shows SnapFrame ON at 10–12 source FPS + 10–12 generated FPS, while OFF sustains 20–24 source FPS. Output cadence is therefore not improved; the current compute/present path consumes nearly the same frame budget as the game itself.

A performance-first baseline is now selected for fresh configurations:
- `flow_scale_percent=50` (77x34 flow cells at 2448x1080, versus 112x50 at 75%). This reduces the number of motion cells to about 47% of the 75% grid. It is a workload reduction estimate, not a measured FPS gain; visual quality must be checked for thin foliage and fast motion.
- `profiling_enabled=0` by default. Timestamp queries and diagnostic readbacks are for controlled tests, not normal gameplay.
- Existing per-game `port/snapframe.conf` values override compiled defaults. Existing installs must change these two values to receive the performance-first profile.

### AI controller status correction

GPU scalar reduction and MotionV2 telemetry publication exist in the host, but the active Snap AI Engine controller is not yet instantiated/called by the runtime. The portable SnapFrame adapter also currently rejects config changes outside its supported fields. Thus there is no active AI policy loop yet and no AI-controlled optimization claim.

Next implementation gate:
1. Instantiate `SnapAI::Engine` in a per-device runtime owner.
2. Consume only frame-matched, asynchronously completed GPU scalar metrics and timestamp samples.
3. Start with bounded flow-scale decisions (25–75%), applied only at history-reset boundaries; keep 50% as the fallback.
4. Add hysteresis/cooldown so decisions do not oscillate each frame.
5. Log telemetry frame ID, validity, decision/profile, old/new scale and reason.
6. Keep AI bypass on stale/missing metrics, low coverage, scene transition, or controller error.
7. Validate A/B with identical scenes and thermal state before enabling adaptation by default.

### Experimental x3/x4 multiplier gate

The current renderer's generated-present loop is hard-coded to one generated frame per source frame. The portable adapter validates multiplier 1–2, and the current midpoint shader only writes one fixed t=0.5 output image. Merely changing the multiplier integer would submit duplicate midpoint images, not correctly interpolated temporal phases.

A real x3/x4 implementation requires:
- parameterized interpolation phase (t=k/M) in the warp shader;
- distinct output images (or a proven sequential output lifetime) for each phase;
- DXVK generation callback carrying phase index and multiplier;
- multiple WSI acquire/present tickets with one-time ownership and present-semaphore retirement;
- correct source/generated pacing and per-phase accepted/drop counters;
- x3/x4 visual tests on motion boundaries and HUD composition.

Do not expose x3/x4 as selectable runtime modes until this pipeline exists. The current shared Presenter acquire state and temporary source/generated synchronization barrier are still a hard safety gate. x3/x4 is an experimental engineering target, not yet implemented.


## Live Snap AI controller connection (2026-11-04)

The Android DXVK host now links `SnapAI::Engine` and creates a per-runtime
`SnapAIHandle`. Only a frame-matched asynchronous GPU timestamp and the matching
validated scalar flow record are submitted to `snap_ai_update()`. If the timestamp
frame ID does not exactly match, AI bypasses that sample. The current implementation
consumes only `decision.flow_scale`; search radius, refinement pass count, flow reuse,
bidirectional-flow selection and artifact-check decisions remain advisory.

Validated scalar metrics are staged by the completion callback. The next serialized
DXVK copy/present callback pairs them with the exact-frame total-chain GPU timestamp,
calls the AI controller, and applies any Flow Scale update there. The completion
callback never releases history, mutates Vulkan resources, blocks on GPU completion,
or waits for WSI. Sparse total-chain timestamps are collected every 30 frames even
when verbose GPU profiling and CSV logging are disabled. Runtime scale
is clamped to 50..75%, warmup is 8 valid frames, target budget is 6 ms, and scale
hysteresis is 8 percentage points. These are conservative initial values, not
device-calibrated optima.

AI bypasses when GPU timestamp telemetry is missing/stale, scalar validation fails,
or the Snap AI API returns an error. The host logs frame, mode, scale, GPU time,
pressure, confidence, complexity and artifact risk.



## Runtime A/B investigation and adaptive V2 budget — 2026-11-05

### Build comparison

- Run [41915429838](https://github.com/Snap888/Skyrim_Android/actions/runs/41915429838) completed successfully on `2b30fcd6ec30b0bd26add8e617bc70e31c635f9c` (older V2 integration state).
- Run [41923694837](https://github.com/Snap888/Skyrim_Android/actions/runs/41923694837) completed successfully on `b4d22fdb96594d13a07b6409c5497193cc10a75a` (newer V2 pyramid/search/refinement/validation and timing instrumentation).
- Both are successful APK builds, but they are different source revisions. GitHub build success does not establish runtime FPS or visual quality.

### Uploaded runtime log findings

The current uploaded `snapframe_perf_on.csv` contains 32 measurement windows. It reports `avg_generated_queued` close to 1.0, but `avg_cpu_present_ms` rises from 2.7 ms to 94.9 ms, and `avg_retire_wait_ms` accounts for nearly all of that delay (2.3–94.2 ms in the shown windows). This points to a severe WSI/frame-retirement wait or pacing bottleneck, not simply CPU cost in the optical-flow shader. A queued generated present is not proof of scan-out.

The same upload has `profiling_enabled=0` and an empty `snapframe_gpu.csv` (header only). The V2 dispatch counters show pyramid/search/refinement/validation work was accepted, but no per-stage GPU cost can be inferred from dispatch acceptance. The event log repeatedly reports `gpu_metrics_rejected: low_valid_flow_coverage`. Therefore the scalar motion telemetry is not currently accepted and the SnapAI flow-scale decision loop receives no valid motion sample in this test. This also means the observed FPS difference between the two APKs cannot yet be attributed to a specific V2 kernel.

### Runtime corrections committed

1. Sparse `total_chain` timestamp queries now run every 30 captured frames even when verbose profiling is disabled. Previously `beginGpuTimer` returned early whenever `profiling_enabled=0`, so the supposedly always-on AI timing sample was never created.
2. The total-chain completion callback now updates the frame-matched timing atomics independently of whether the CSV file is open. CSV output remains optional; AI timing no longer depends on a logging file handle.
3. A bounded GPU-budget guard now adjusts the V2 motion-search cadence using completed total-chain GPU timing:
   - above 6 ms for two consecutive samples: run V2 search/refine/validation once per four source frames;
   - below 4 ms for three consecutive samples: restore every-frame V2 analysis;
   - between thresholds: retain the current interval.
4. This decimation is safe for the current architecture because V2 flow/refinement/validation remain diagnostic-only and do not feed the V1 midpoint warp. The V1 source-frame and midpoint output path is unchanged. It reduces experimental V2 work, not the V1 flow or WSI wait.
5. The guard is a conservative host-side budget governor, not yet the full `SnapAI::Engine` policy loop. The full AI loop still bypasses while GPU scalar coverage is rejected. Do not describe Flow Scale as AI-adaptive until the metrics validity problem is fixed and an accepted AI decision is logged.

### Next required validation

- Rebuild the exact new HEAD and run the same scene with the same settings.
- Keep `profiling_enabled=0` for normal gameplay; confirm `snapframe_gpu.csv` now receives sparse `total_chain` rows. For one controlled diagnostic run, set profiling on to collect `v2_pyramid`, `v2_search`, `v2_refine` and `v2_validation` stage timings.
- Confirm logs show interval transitions 1 → 4 under sustained pressure and 4 → 1 after sustained recovery.
- Compare `avg_retire_wait_ms` separately from `avg_generate_record_ms` and the V2 GPU stage times. If retire wait remains dominant, optimize WSI retirement/semaphore pacing before changing optical-flow quality.
- Fix the scalar metrics coverage rejection with a CPU-reference test of the reduction shader and a dump of per-field valid counts. Do not lower the 90% validity threshold merely to force AI activation.
- Only after coverage is accepted and timing is frame-matched should the full SnapAI policy make adaptive Flow Scale/search decisions.

### Deferred

Confidence-mask consumption by the warp remains gated on proving vector direction, mask format/extent and measured GPU cost. The current V2 validation mask is not yet wired into midpoint reconstruction. x3/x4 temporal phases remain deferred until x2 pacing and Presenter acquire/present ownership are stable.
