# SnapFrame Generation

Experimental, independent frame-generation engine for Skyrim_Android.

**Status: first real GPU preprocessing test. No interpolated/generated frames are presented yet.**
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

## Current capture + preprocessing test

- DXVK callback runs after the game image is blitted to the WSI image and before
  the HUD overlay is drawn.
- The callback records a GPU-to-GPU image copy in the active DXVK command list;
  it performs no CPU readback and submits no unrelated Vulkan queue work.
- Two private DXVK images are allocated for frame history. They are released
  from a device-destroy callback while the DXVK allocator is still alive.
- Capture is disabled by default. To enable this diagnostic prototype, copy
  `snapframe.conf.example` to `<game-directory>/port/snapframe.conf` or create
  that file containing:

  ```ini
  enabled=1
  ```
- This stage captures each real frame and runs one half-resolution R16F luma
  downsample compute pass. It does not interpolate or alter presentation.
  Enabling it adds two full-resolution history images, one half-resolution
  output image, a GPU copy and a compute dispatch per real frame. Treat it as a
  diagnostic test, not a performance mode.

When the game starts, SnapFrame creates a separate diagnostic file at
`<game-directory>/port/snapframe.log` (truncated for each launch). It records
bootstrap/configuration, DXVK PresentImage entry, WSI image details, callback
entry, format capability checks, history allocation, capture skips and frame
counters. History allocation validates the exact `TRANSFER_DST | SAMPLED`
image-usage combination and requested extent before creating the two images.
Pending history resets are consumed before the enabled-state early return, so
disabling SnapFrame releases cached GPU images and the sampler on the next
DXVK present callback. The same messages are also mirrored to the normal stderr log. This
makes it possible to send `snapframe.log` without the large general game log.

When enabled, the diagnostic sequence is:

1. `host initialized` — runtime config was loaded.
2. `entered D3D9SwapChainEx::PresentImage` — the D3D9 present method was entered.
3. `DXVK present path reached` — the WSI image is available after the game-image
   blit; the line reports extent, format, usage flags, and callback registration.
4. `dispatching first DXVK copy callback` and `host copy callback entered` —
   the host callback actually ran.
5. `allocated two-frame history` — the private history images were allocated.
6. `two real frames captured` — at least two copies were recorded.
7. `luma compute resources ready` — the R16F output, sampler and shader were created.
8. `luma dispatch accepted` — DXVK committed the compute pipeline and recorded the dispatch. This confirms command recording, not yet the numeric correctness of the output.

If the log contains only `host initialized` and no `entered D3D9SwapChainEx::PresentImage`,
the current evidence does not show that the game reached its first D3D9 present.
If the entry marker appears but `DXVK present path reached` does not, the stop
is later in PresentImage, before or during WSI acquisition/blitting. A final `VirtualAlloc(...)` line alone is not proof of a SnapFrame
failure; collect the complete log through the point where the app exits or
stops progressing. If the present-path marker appears, the following
`capture skipped` or allocation diagnostics identify the next failure point.

The module is added to the JNI CMake build as a separate static target. The
first GLSL compute shader now has a DXVK compute pipeline/resource path and is
dispatched on the same active command list immediately after each captured
frame. It writes half-resolution R16F luminance. CMake attempts to compile and
validate the GLSL automatically on the build host when Python 3, glslangValidator
and spirv-val are installed. If they are missing, the APK still builds with
capture-only diagnostics and `snapframe.log` explicitly reports that luma
compute was disabled. No optical flow, interpolation, or generated-frame
presentation is implemented yet.


## First preprocessing shader source

The first SnapFrame compute source is
`shaders/luma_downsample.comp`. It reads a real frame through a sampled
`sampler2D`, computes Rec.709 luminance, and writes a half-resolution
`R16_SFLOAT` image using a 2x2 box reduction. The output dimensions must be
`ceil(sourceWidth / 2)` by `ceil(sourceHeight / 2)`; edge texels are clamped
for odd source dimensions.

To build the compute test manually, make sure the build host has Python 3,
`glslangValidator`, and `spirv-val`.
On Ubuntu/Debian, install the tools with `sudo apt-get install glslang-tools spirv-tools`. CMake invokes
`tools/compile_snapframe_shaders.py` automatically during configure and emits
`generated/snapframe_shaders.h`; the NDK build then consumes this header and
does not need the shader tools. The standalone command is also available:

```sh
python3 tools/compile_snapframe_shaders.py
```

The tool validates Vulkan 1.1 SPIR-V before generating the C++ word array.
