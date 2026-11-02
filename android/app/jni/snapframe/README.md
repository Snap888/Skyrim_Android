# SnapFrame Generation

Experimental, independent frame-generation engine for Skyrim_Android.

**Status: architecture/scaffold only. No generated frames are produced yet.**
This module is intentionally isolated from the existing `lsfg` target and is
not included in the application build. This commit creates the module boundary
and records the implementation/integration plan; it does not claim a working
frame-generation implementation.

## Goals

- Implement an original hybrid motion-estimation and frame-warping pipeline.
- Run image processing on the Vulkan GPU; avoid GPU-to-CPU readback.
- Keep the existing LSFG implementation unchanged and available for A/B tests.
- Make the engine independently switchable: Off / LSFG / SnapFrame.
- Add per-stage GPU timing and diagnostic counters before performance tuning.

## Initial algorithm direction

1. Build reduced-resolution luminance pyramids for two consecutive real frames.
2. Estimate forward and backward motion using coarse-to-fine block matching.
3. Refine motion locally and calculate forward/backward consistency confidence.
4. Detect likely disocclusion/occlusion regions and low-confidence vectors.
5. Warp both source frames toward the requested intermediate timestamp.
6. Blend valid samples; use conservative spatial fallback where motion is uncertain.
7. Add temporal stabilization only after the basic pipeline is validated.

This is a proposed algorithm, not an implemented or measured result. See
[DESIGN.md](DESIGN.md) for the architecture and staged integration plan.

## Build isolation

The local `CMakeLists.txt` defines a small host library, but the parent
`android/app/jni/CMakeLists.txt` does not add this directory yet. The host API
is scaffolding only. Vulkan/DXVK resources, compute shaders, frame capture,
and presentation are future implementation stages.
