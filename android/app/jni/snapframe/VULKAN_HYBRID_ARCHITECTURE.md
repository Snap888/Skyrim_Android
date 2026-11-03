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

- Replace single-resolution +/-9 block matching with a true multi-level luma pyramid. Search large displacement at a coarse level, upscale the vector and refine locally at finer levels. This expands motion coverage without a full-resolution exhaustive search.
- Improve the 3x3 SAD matcher with subpixel refinement and a texture/ambiguity metric; confidence must include match uniqueness, photometric residual, forward/backward closure and boundary status.
- Reconstruct from each direction at its own projected endpoint, not by sampling both directional fields at the same output UV. Add occlusion/disocclusion selection and edge-aware confidence.
- Keep uncertain regions sharp: use a nearest real-frame fallback rather than averaging two unwarped views. This can reduce local temporal smoothness, but avoids obvious double-image blur.
- Add a dynamic quality policy: lower search/pyramid work for low-motion frames, use coarse levels for large global motion, and refine only cells with ambiguous/high-residual motion. Any adaptive policy must be deterministic and logged.
- Optimize GPU cost only after stage timing is validated. Avoid unconditional 100% flow density or a brute-force larger search; the 75% grid already has 5,600 cells and the current two-direction flow is the dominant GPU cost.

## Super-resolution / upscaling — theory only

A lower internal game render resolution followed by GPU upscaling can reduce the game's rasterization and shading load. It does **not** directly reduce SnapFrame's current cost if the captured/presented image remains 2448x1080: luma, flow and midpoint reconstruction still process the full captured extent. If capture is also performed at a lower resolution, motion estimation becomes cheaper, but fine geometry, foliage and HUD composition may lose detail and motion vectors become less precise.

A practical future experiment would be a driver-independent spatial upscaler before or after generation, with explicit source/output extents and separate controls. It should be evaluated as a game-render-resolution feature, not assumed to be a frame-generation optimization. The best order is: first stabilize flow and reconstruction, then benchmark lower render resolution plus upscaling against native-resolution SnapFrame at equal output resolution. No super-resolution code or setting is added by this document.
