# SnapFrame hybrid Vulkan architecture

## Chosen ownership model

SnapFrame will not become a process-wide or app-wide Vulkan Layer. The selected design is hybrid:

- **DXVK integration adapter:** identifies completed game frames, owns the D3D9/DXVK present boundary, captures the correct source image, tracks frame identity/timing, resets history on resize/device loss/stalls, and coordinates the additional present with HUD composition.
- **SnapFrame Vulkan compute backend:** owns preprocessing, motion estimation, consistency/occlusion analysis, reconstruction and backend-local resource policy. It must not decide which game frame is authoritative.
- **Vulkan ICD / custom driver:** remains selected by Android's Vulkan loader. The backend queries the active device's actual formats, limits, extensions and features; it must not assume that a driver supports an optimization merely because it is Turnip or a particular version.

DXVK remains the authority for game-frame identity and presentation. A future backend boundary may be implemented as a narrow interface around device capabilities, resource creation, shader/pipeline creation, dispatch and output-image handoff. No separate layer is required.

## Current implementation status

The repository already has a separate `snapframe_host` static target and standalone GLSL compute sources. The host is called from DXVK's present/copy path and records GPU work through the active `DxvkContext`; this preserves the same command stream and avoids CPU readback in the normal path.

However, the compute host still directly uses DXVK internal device/context/image abstractions. Therefore it is **not yet a fully standalone Vulkan backend**. The separation currently exists at the module/shader level, not as a stable backend API. Refactor incrementally; do not move frame ownership or swapchain policy out of DXVK.

## Device and custom-driver policy

The active test reports Turnip Gen8 V38. The implementation should remain driver-agnostic and capability-driven:

1. Query exact image format features and image-usage combinations before allocating resources.
2. Query extent limits, storage/sampled support, timestamp support and relevant Vulkan features/extensions.
3. Select an optimization only when every required capability is present.
4. Keep a conservative fallback path for unsupported formats/features.
5. Log physical-device vendor/device IDs, driver name/version, Vulkan API version and the selected backend profile so A/B tests can be reproduced.
6. Do not hard-code driver-name checks as a substitute for Vulkan capability checks. Driver-specific shader variants should be opt-in only after a reproducible correctness/performance win.

The current host already performs runtime checks for history-image usage, R16F luma and storage/sampled outputs. A complete capability/profile report and a formal backend interface remain follow-up work.

## Recommended staged refactor

1. **DXVK frame adapter (keep in DXVK):** capture callback, frame sequence, source image lifetime, present pacing, HUD composition and reset events.
2. **Backend contract:** pass an immutable frame-pair descriptor and target timestamp to a backend; receive an output image plus completion/lifetime token. The contract must not expose DXVK present policy.
3. **Vulkan backend:** own luma resources, motion fields, compute pipelines, capability selection, scratch images and dispatch recording.
4. **Validation backend/profile:** add synthetic identical-frame and known-translation inputs; compare vector sign/magnitude, boundary behavior and consistency against ground truth.
5. **Presentation handoff:** return the generated image to DXVK with explicit synchronization/lifetime semantics. DXVK remains responsible for WSI acquire/present and UI.
6. **Driver profiles:** only after the generic backend is correct, add optional variants selected by queried capabilities and measured device results.

## Quality and performance roadmap

- The current matcher has been changed to a 5x5 coarse lattice plus a 5x5 local refinement (49 candidate positions maximum instead of 89). This is an unbenchmarked performance experiment; verify visual quality and GPU timestamps on device before keeping it.
- Next, replace single-resolution +/-10 block matching with a true multi-level luma pyramid. Search large displacement at a coarse level, upscale the vector and refine locally at finer levels. This expands motion coverage without a full-resolution exhaustive search.
- Improve the 3x3 SAD matcher with subpixel refinement and a texture/ambiguity metric; confidence must include match uniqueness, photometric residual, forward/backward closure and boundary status.
- Reconstruct from each direction at its own projected endpoint, not by sampling both directional fields at the same output UV. Add occlusion/disocclusion selection and edge-aware confidence.
- Keep uncertain regions sharp: use a nearest real-frame fallback rather than averaging two unwarped views. This can reduce local temporal smoothness, but avoids obvious double-image blur.
- Add a dynamic quality policy: lower search/pyramid work for low-motion frames, use coarse levels for large global motion, and refine only cells with ambiguous/high-residual motion. Any adaptive policy must be deterministic and logged.
- Optimize GPU cost only after stage timing is validated. Avoid unconditional 100% flow density or a brute-force larger search; the 75% grid already has 5,600 cells and the current two-direction flow is the dominant GPU cost.

## Super-resolution / upscaling — theory only

A lower internal game render resolution followed by GPU upscaling can reduce the game's rasterization and shading load. It does **not** directly reduce SnapFrame's current cost if the captured/presented image remains 2448x1080: luma, flow and midpoint reconstruction still process the full captured extent. If capture is also performed at a lower resolution, motion estimation becomes cheaper, but fine geometry, foliage and HUD composition may lose detail and motion vectors become less precise.

A practical future experiment would be a driver-independent spatial upscaler before or after generation, with explicit source/output extents and separate controls. It should be evaluated as a game-render-resolution feature, not assumed to be a frame-generation optimization. The best order is: first stabilize flow and reconstruction, then benchmark lower render resolution plus upscaling against native-resolution SnapFrame at equal output resolution. No super-resolution code or setting is added by this document.


### Latest device log note (2026-11-03)

The follow-up 75% run records a 12.961 ms median total chain (P90 13.833 ms), with forward/backward flow medians of 5.025/5.068 ms. The flow passes remain the dominant cost. This is not a controlled comparison against the previous 16.339 ms run. More importantly, the runtime log still identified the old 9x9/step-2 matcher although source had moved to the 49-candidate 5x5/step-4 plus 5x5-refine matcher. Host-side startup diagnostics and flow boundary accounting have been corrected to match the current shader; a new device build must confirm the actual shader version before any speedup is attributed to the lattice change.

The uploaded run ends partway through phase 2. Large-motion samples occur under the static_scene label and near-zero-motion samples occur near the end of slow_camera_pan, so phase labels are not reliable in this capture. The next test must follow the phase instructions exactly and complete all seven phases. The high-motion samples still frequently hit the +/-10 search boundary; this supports moving to a true multi-level luma pyramid rather than increasing the single-level search radius.


### Latest 49-candidate device run and SAD optimization (2026-11-03)

The newest upload confirms the active 49-candidate matcher in the runtime startup log. Phases 1–3 completed at 1,800 frames each with zero skips or dispatch failures; phase 4 reached 1,500/1,800 before the uploaded log ended. Phases 5–7 are not present. The supplied config has 75% flow scale and profiling/motion diagnostics enabled; LSFG is disabled.

The 59 flow snapshots show repeated large motion (roughly 7–8 half-resolution luma texels) and significant +/-10 boundary saturation in nominal phases 1–3. This is consistent with an under-sized single-level search window, but phase labels and measured movement are not fully aligned. A phase-disciplined complete run is still required before judging static-scene or camera-pan quality.

Across 238 valid GPU timing samples per stage, medians are: capture 0.504 ms, luma 0.399 ms, forward flow 5.019 ms, backward flow 5.017 ms, consistency 0.012 ms, midpoint 1.554 ms and total chain 12.706 ms. The two flow passes account for about 79% of the median chain and remain the optimization priority. This is the first confirmed 49-candidate runtime sample, not a controlled A/B against the 89-candidate matcher.

A follow-up shader change adds exact SAD early termination: a candidate's patch evaluation stops only when its partial non-negative SAD is strictly greater than the current second-best complete cost. Such a candidate cannot affect the best match, second-best match or confidence margin. Strict greater-than preserves equal-cost tie behavior. This reduces texture reads on favorable candidate distributions without changing the search result; actual savings require device measurements. Keep the 3x3 patch, 49-candidate lattice, boundary suppression and current-frame fallback unchanged for this comparison.

Next algorithmic work remains a true multi-level luma pyramid for large motion. Do not widen the single-level search radius. Pyramid resource allocation, per-level extents, dispatch barriers and vector scaling must be implemented and validated as a separate change after the SAD early-out has a measured baseline.


### Latest device run — 2026-11-03 (SAD early-out)

The latest upload (`2fbbc80e038e0589fb6b750b8b64dfd90c433572`) confirms the 49-candidate matcher, 75% flow scale, 2448x1080 capture and 112x50 flow grid. LSFG was disabled. Phases 1–5 completed 1,800 frames each without skips or dispatch failures; phase 6 reached about 840/1,800 frames and the uploaded log ends before phase 6/7 results. The 82 aggregate flow snapshots show the same phase/motion mismatch: some nominal static-scene samples contain large movement, while some slow-pan samples are nearly static.

GPU timings (329 valid samples per stage) are: capture median 0.503 ms, luma 0.400 ms, forward flow 3.496 ms, backward flow 3.484 ms, consistency 0.012 ms, midpoint 1.554 ms and total chain 9.487 ms. Relative to the previous run's 12.706 ms total-chain median and 5.019/5.017 ms flow medians, these are lower by about 25% total and 30% per flow pass. This is encouraging evidence that exact SAD early termination may help, but the runs are not controlled for scene, thermal state or workload; no guaranteed speedup is claimed.

High-motion snapshots still hit the +/-10 boundary: up to 2,034/5,600 cells (36.3%) at frame 8,521, with forward confidence 0.0407 and only 0.018% cells above 0.5 consistency. Near-static snapshots reach approximately 0.99 confidence/consistency. Keep the boundary suppression and current-frame fallback. Do not expand the single-level radius; a multi-level pyramid remains the correct direction for large displacement.

A follow-up exact-work optimization now evaluates the 3x3 SAD patch center first, then axial neighbors and corners, retaining strict partial-SAD > second-best early termination. All nine samples still contribute for candidates that can affect ranking, so this changes evaluation order rather than match semantics. Commit `0a118fd93428a1bdf534f3f8763d59a1489be85d`; device benefit remains unmeasured.


### Extended device log upload — 2026-11-04

The new upload confirms the 49-candidate +/-10 matcher, 2448x1080 capture, 75% flow scale, 1224x540 R16F luma and 112x50 flow grid. Five diagnostic phases complete 1,800 frames each with zero reported skips or dispatch failures. Phase 6 reaches approximately 1,560/1,800 frames (log ends at captured frame 10,560); phase 6 result, phase 7 and suite completion are absent.

The 355-sample GPU CSV reports median/P90 ms: capture 0.523/0.640, luma 0.400/0.540, forward flow 4.284/4.857, backward flow 4.297/4.799, consistency 0.012/0.014, midpoint 1.554/2.108, total chain 11.106/12.606. Flow remains the dominant cost (~8.58 ms combined median). This is slower than the preceding upload's 9.487 ms total median and 3.496/3.484 ms flow medians, but the runs are not controlled for scene, thermal state or workload. The first 12 samples are notably slower than the remaining 343, suggesting a changing GPU/runtime state; do not attribute this delta to a shader change.

The 88 flow snapshots show continued large-motion clipping: peak forward boundary saturation is 2,418/5,600 cells (43.2%) at frame 5,881, with confidence 0.0432 and 0.143% consistency >0.5. Frame 9,121 reaches 2,250/5,600 boundary cells (40.2%). Near-static snapshots still reach near-perfect confidence/consistency. Keep the current boundary suppression and real-frame fallback; do not increase the single-level radius.

The implementation currently has one half-resolution luma image per history slot. `luma_downsample.comp` is the initial 2x reduction, not a multi-level pyramid. The next algorithmic stage requires per-slot level resources, downsample dispatches and explicit barriers, coarse-to-fine vector propagation with scale conversion, and matching forward/backward consistency units. Preserve the existing single-level path as fallback until deterministic identical-frame and known-translation tests validate every level. The latest upload does not establish a controlled device benefit for center-first SAD evaluation order.
