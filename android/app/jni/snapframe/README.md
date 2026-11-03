# SnapFrame Generation

Experimental, independent frame-generation engine for Skyrim_Android.

**Status: capture prototype. No generated frames are produced yet.**
This module is isolated from the existing `lsfg` implementation. It is now
compiled as a separate host target and has an independent DXVK callback that
copies the completed game image into a two-image GPU history ring. Optical flow,
interpolation and generated-frame presentation are not implemented.

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

## Current capture prototype

- DXVK callback runs after the game image is blitted to the WSI image and before
  the HUD overlay is drawn.
- The callback records a GPU-to-GPU image copy in the active DXVK command list;
  it performs no CPU readback and submits no unrelated Vulkan queue work.
- Two private DXVK images are allocated for frame history. They are released
  from a device-destroy callback while the DXVK allocator is still alive.
- Capture is disabled by default. To enable this diagnostic prototype, create
  `<game-directory>/port/snapframe.conf` containing:

  ```ini
  enabled=1
  ```
- The prototype currently only captures frames. It does not interpolate or
  alter presentation, and enabling it adds two full-resolution GPU images and
  a copy per real frame. This is only for validating the hook and history
  lifetime; it is not a performance mode and should remain disabled during
  normal gameplay until interpolation exists.

When enabled, the diagnostic sequence is:

1. `host initialized` — runtime config was loaded.
2. `entered D3D9SwapChainEx::PresentImage` — the D3D9 present method was entered.
3. `DXVK present path reached` — the WSI image is available after the game-image
   blit; the line reports extent, format, usage flags, and callback registration.
4. `dispatching first DXVK copy callback` and `host copy callback entered` —
   the host callback actually ran.
5. `allocated two-frame history` — the private history images were allocated.
6. `two real frames captured` — at least two copies were recorded.

If the log contains only `host initialized` and no `entered D3D9SwapChainEx::PresentImage`,
the current evidence does not show that the game reached its first D3D9 present.
If the entry marker appears but `DXVK present path reached` does not, the stop
is later in PresentImage, before or during WSI acquisition/blitting. A final `VirtualAlloc(...)` line alone is not proof of a SnapFrame
failure; collect the complete log through the point where the app exits or
stops progressing. If the present-path marker appears, the following
`capture skipped` or allocation diagnostics identify the next failure point.

The module is added to the JNI CMake build as a separate static target. No
compute shaders or runtime selector have been added yet. The Android native
CMake project currently consumes generated DXVK shader headers; it does not
compile standalone GLSL sources itself. A dedicated, reproducible
GLSL-to-SPIR-V packaging step must be established before adding the first
SnapFrame compute shader.


## First preprocessing shader source

The first SnapFrame compute source is
`shaders/luma_downsample.comp`. It reads a real frame through a sampled
`sampler2D`, computes Rec.709 luminance, and writes a half-resolution
`R16_SFLOAT` image using a 2x2 box reduction. The output dimensions must be
`ceil(sourceWidth / 2)` by `ceil(sourceHeight / 2)`; edge texels are clamped
for odd source dimensions.

Generate the C++ SPIR-V header on the host before integrating the shader:

```sh
python3 tools/compile_snapframe_shaders.py
```

The tool requires `glslangValidator` and `spirv-val` (or explicit
`--glslang` / `--spirv-val` paths). It validates Vulkan 1.1 SPIR-V and writes
`android/app/jni/snapframe/generated/snapframe_shaders.h`. Android NDK builds
consume the generated header and do not need the shader compiler. The shader
is source/build infrastructure only at this stage: no compute pipeline,
descriptor binding, dispatch, or runtime output is wired yet. Runtime support
for sampled source formats and `R16_SFLOAT` storage images must be checked on
the physical device before dispatch.
