# SnapFrame Generation

Experimental, independent frame-generation engine for Skyrim_Android.

**Status: GPU capture, temporal luminance and coarse block-matching motion estimation prototype. No interpolated/generated frames are presented yet.**
This module is isolated from the existing `lsfg` implementation. It is now
compiled as a separate host target and has an independent DXVK callback that
copies the completed game image into a two-image GPU history ring. A first coarse bidirectional block-matching motion pair is estimated from a two-slot luma history; flow refinement, consistency validation, interpolation and generated-frame presentation are not implemented.

## Goals

- Implement an original hybrid motion-estimation and frame-warping pipeline.
- Run image processing on the Vulkan GPU; avoid GPU-to-CPU readback.
- Keep the existing LSFG implementation unchanged and available for A/B tests.
- Make the engine independently switchable: Off / LSFG / SnapFrame.
- Add per-stage GPU timing and diagnostic counters before performance tuning.

## Initial algorithm direction

1. Build reduced-resolution luminance pyramids for two consecutive real frames.
2. Estimate forward and backward motion using coarse-to-fine block matching. The initial coarse bidirectional pass is implemented; pyramid refinement is pending.
3. Refine motion locally and calculate forward/backward consistency confidence.
4. Detect likely disocclusion/occlusion regions and low-confidence vectors.
5. Warp both source frames toward the requested intermediate timestamp.
6. Blend valid samples; use conservative spatial fallback where motion is uncertain.
7. Add temporal stabilization only after the basic pipeline is validated.

This is the target algorithm. Capture, luma preprocessing and a first coarse bidirectional block-matching pass are implemented; the remaining stages are not yet measured or implemented. See [DESIGN.md](DESIGN.md) for the staged integration plan.

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
- This stage captures each real frame, downsamples to R16F luma in a two-slot history ring, and—after two valid frames—runs a coarse block-matching pass into a 1/16-resolution RGBA16F motion field. The pass searches a 9x9 displacement window over 3x3 luma patches and stores motion, best/second-best confidence and matching cost. It does not interpolate or alter presentation. Enabling it adds two full-resolution history images, two half-resolution luma images, two low-resolution motion images, a GPU copy and two motion compute dispatches per real frame. Treat it as a diagnostic test, not a performance mode.

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
8. `luma dispatch accepted` — DXVK committed the luma compute pipeline and recorded the dispatch. This confirms command recording, not numeric correctness.
9. `bidirectional block-matching flow ready` and `bidirectional flow pair accepted` — both RGBA16F motion outputs and both dispatches are available. This still does not validate motion-vector values.

If the log contains only `host initialized` and no `entered D3D9SwapChainEx::PresentImage`,
the current evidence does not show that the game reached its first D3D9 present.
If the entry marker appears but `DXVK present path reached` does not, the stop
is later in PresentImage, before or during WSI acquisition/blitting. A final `VirtualAlloc(...)` line alone is not proof of a SnapFrame
failure; collect the complete log through the point where the app exits or
stops progressing. If the present-path marker appears, the following
`capture skipped` or allocation diagnostics identify the next failure point.

The module is added to the JNI CMake build as a separate static target. The
luma and block-matching GLSL compute shaders have DXVK compute pipeline/resource
paths and are dispatched on the same active command list. Luma writes a
half-resolution R16F image; block matching writes a 1/16-resolution RGBA16F
motion field after two valid luma frames. CMake attempts to compile and
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

## Runtime diagnostics and failure handling

The luma setup now registers the compute shader with DXVK's pipeline manager
only after the output image, image view, sampler, and shader object are all
available. If setup is incomplete or throws, partially created luma image,
view, and sampler references are released and frame capture remains enabled.
The dedicated log reports the individual luma resource states before falling
back to capture-only operation. Invalid callback arguments are also reported
once as a capture skip reason.

These checks improve failure visibility; they do not replace an NDK build or
a device run. A recorded dispatch still needs GPU-side validation before the
luma values can be considered numerically verified.


## First motion-estimation pass (experimental)

The `shaders/block_match_flow.comp` kernel runs after the current frame's luma pass once both temporal luma slots are valid. Each motion texel represents a 16x16 source-image region. The kernel searches +/-4 half-resolution luma texels using a 3x3 absolute-difference patch and writes RGBA16F: X/Y displacement in luma texels, best/second-best separation confidence, and normalized matching cost.

The motion field is diagnostic/preparatory data only. It is not yet consumed by a warp, occlusion resolver, interpolation pass, or presentation path. The search is intentionally conservative and may fail on repeated textures, disocclusion, thin geometry, particles, and HUD elements. Its GPU cost and numerical output have not yet been measured on device. The output format is queried at runtime; unsupported devices keep capture/luma active and skip motion estimation.

The DXVK helper preserves resource slots 0..2 and the previous compute state around the three-binding motion dispatch. This does not constitute a full synchronization/presentation solution; subsequent stages must explicitly consume the flow image in the same command stream or add a lifetime-safe handoff.


### Bidirectional coarse motion (current implementation)

The host allocates two RGBA16F fields for each adjacent-frame pair. Output 0 is current-to-previous (the current-frame sample searches the previous frame); output 1 is previous-to-current (the inputs are reversed). A pair is counted as accepted only when both dispatches are recorded. Both fields are overwritten on the next real-frame pair and are not yet consumed by an interpolation stage. This is a directional pair, not yet a forward/backward consistency test.


The block matcher rejects candidate patches that leave the previous-frame bounds instead of clamping samples to the edge. The current patch center is kept inside the valid image region. This avoids artificial edge matches; edge vectors can still be less constrained and remain unvalidated until the consistency/readback stage.
