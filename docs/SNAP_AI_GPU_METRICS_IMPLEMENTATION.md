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
