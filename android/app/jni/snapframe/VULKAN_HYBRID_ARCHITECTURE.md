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
