# SnapFrame Generation

Experimental, independent frame-generation engine for Skyrim_Android.

**Status: GPU capture, temporal luminance, bidirectional coarse motion estimation, forward/backward consistency, midpoint color-warp compute output, sampled flow readback and opt-in GPU timestamp diagnostics. The midpoint image is generated into an internal RGBA16F image and routed through DXVK's additional WSI-present loop. The code path is wired but still requires a full native build and on-device validation; visual quality and device performance remain unverified.**
This module is isolated from the existing `lsfg` implementation. It is now
compiled as a separate host target and has an independent DXVK callback that
copies the completed game image into a two-image GPU history ring. A first coarse bidirectional block-matching motion pair is estimated from a two-slot luma history; pyramid refinement and robust occlusion handling are not implemented. A midpoint compute pass writes an internal RGBA16F image, which is now routed to an additional WSI present when ready.

## Goals

- Implement an original hybrid motion-estimation and frame-warping pipeline.
- Run image processing on the Vulkan GPU; keep CPU readback out of the normal path and use sparse readback only for diagnostics.
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

This is the target algorithm. Capture, luma preprocessing, bidirectional block matching, forward/backward consistency and a first confidence-weighted midpoint color-warp pass are implemented as GPU dispatches. Flow diagnostics are read back sparsely; the midpoint output is not yet read back or visually validated. Pyramid refinement and robust occlusion handling remain unimplemented; the additional WSI-present path is wired but not yet device-validated. See [DESIGN.md](DESIGN.md) for the staged integration plan.

## Current GPU reconstruction and presentation prototype

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
- This stage captures each real frame, downsamples to R16F luma in a two-slot history ring, and—after two valid frames—runs bidirectional coarse-to-fine block matching into a configurable RGBA16F motion grid, calculates forward/backward consistency, and dispatches a confidence-weighted midpoint warp into an internal RGBA16F output image. The matcher evaluates 9x9 coarse offsets at two-luma-texel spacing and a 3x3 full-resolution refinement over 3x3 luma patches. It stores displacement, best/second-best confidence and matching cost. The midpoint image is submitted as one additional WSI present after each real frame for which a valid warp was dispatched. The runtime falls back to the real image if the midpoint view is unavailable. Enabling it adds two full-resolution history images, two half-resolution luma images, two low-resolution motion images, an RGBA16F midpoint image, a GPU copy and multiple compute dispatches per real frame. Treat it as a diagnostic test, not a performance mode.

When the game starts, SnapFrame creates separate diagnostic outputs under
`<game-directory>/port/`: `snapframe.log`, `snapframe_gpu.csv`,
`snapframe_flow.csv`, `snapframe_flow_vectors.csv`, and `snapframe_events.csv`
(each truncated for a new launch). The event CSV records capture-copy
submission, luma dispatch success/failure, both flow directions, consistency
dispatch status, completed frames, and early skips (invalid callback arguments,
source validation, history allocation, midpoint dispatch, and additional WSI-present request/result). It is intended to make it clear
which stage stopped progressing even when the text log is incomplete. The text log records
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
luma, block-matching, consistency and midpoint-warp GLSL compute shaders have
DXVK compute pipeline/resource paths. Luma writes a half-resolution R16F
image; block matching writes a low-resolution RGBA16F motion field; the warp
writes a full-resolution RGBA16F midpoint. The DXVK D3D9 present path then
acquires an additional WSI image and presents that midpoint after the real
frame. CMake compiles and validates all SnapFrame GLSL on the build host when
Python 3, glslangValidator and spirv-val are installed. If they are missing,
the APK still builds with capture-only diagnostics and `snapframe.log`
explicitly reports that SnapFrame compute dispatches were disabled. The native
C++ integration and device runtime still require a complete APK build and
on-device verification.


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

The `shaders/block_match_flow.comp` kernel runs after the current frame's luma pass once both temporal luma slots are valid. Each motion texel represents a configurable region: at 50% flow scale it is approximately 32x32 source pixels. The matcher searches a +/-8 coarse lattice with two-luma-texel spacing, then refines +/-1 around the coarse winner (effective +/-9 range), using a 3x3 absolute-difference patch. It writes RGBA16F: X/Y displacement in luma texels, best/second-best separation confidence, and normalized matching cost.

The motion field is diagnostic/preparatory data only. It is not yet consumed by a warp, occlusion resolver, interpolation pass, or presentation path. The search is intentionally conservative and may fail on repeated textures, disocclusion, thin geometry, particles, and HUD elements. Its GPU cost and numerical output have not yet been measured on device. The output format is queried at runtime; unsupported devices keep capture/luma active and skip motion estimation.

The DXVK helper preserves resource slots 0..2 and the previous compute state around the three-binding motion dispatch. This does not constitute a full synchronization/presentation solution; subsequent stages must explicitly consume the flow image in the same command stream or add a lifetime-safe handoff.


### Bidirectional coarse motion (current implementation)

The host allocates two RGBA16F fields for each adjacent-frame pair. Output 0 is current-to-previous (the current-frame sample searches the previous frame); output 1 is previous-to-current (the inputs are reversed). A pair is counted as accepted only when both dispatches are recorded. Both fields are overwritten on the next real-frame pair and are not yet consumed by an interpolation stage. This is a directional pair, not yet a forward/backward consistency test.


The block matcher rejects candidate patches that leave the previous-frame bounds instead of clamping samples to the edge. The current patch center is kept inside the valid image region. This avoids artificial edge matches; edge vectors can still be less constrained and remain unvalidated until the consistency/readback stage.


### Forward/backward consistency pass (new, unvalidated)

After both directional block-matching dispatches have been recorded, SnapFrame
runs `shaders/flow_consistency.comp`. It projects each current-to-previous
vector into the reverse field, evaluates the forward/backward cycle error,
and combines that error with the two local match-confidence values. The result
is a reduced-resolution R16F soft validity mask. Values near 1 indicate a
mutually consistent pair of local matches; values near 0 are rejected.

The pass is intentionally conservative and is not a complete occlusion
classifier. It cannot reliably distinguish all disocclusions from repeated
textures, low-texture regions, animated foliage, particles, or HUD motion.
The R16F mask is currently diagnostic/preparatory data only: no interpolation
shader consumes it and no intermediate frame is presented. A dispatch being
accepted proves only that DXVK recorded the compute work, not that the mask
contains correct values.

The host allocates the mask only when the luma and bidirectional flow stages
are available. It is generated on the same active DXVK command list after
both flow dispatches. The DXVK helper preserves compute state and resource
slots 0..2. The shader compiler/validator list includes this third kernel.


## One-build multi-scene diagnostic campaign

Set `diagnostic_suite=1` and `diagnostic_phase_frames=1800` in
`<game-directory>/port/snapframe.conf`. The host marks seven sequential
phases in the dedicated log, each lasting the configured number of captured
frames:
1. `static_scene`: hold the camera and scene still.
2. `slow_camera_pan`: slowly pan horizontally and vertically.
3. `fast_camera_rotation`: rotate quickly in multiple directions.
4. `moving_npc_combat`: observe moving NPCs/combat with limited camera motion.
5. `foliage_particles_water`: capture foliage, water, fire, smoke and particles.
6. `hud_inventory_map`: open HUD, inventory and map; move cursor/selection.
7. `long_run_stress`: continue normal gameplay for sustained stability.

The log emits `TEST_PHASE_BEGIN`, a manual action prompt, and
`TEST_PHASE_RESULT` with per-phase dispatch counts and skips. Phase labels
do not detect scene content; follow the action prompt manually. Duration is
based on successfully captured frames, not wall time.

All currently implemented GPU stages remain active in every phase. This is
not a numerical shader self-test: dispatch acceptance does not prove vector
accuracy. Sampled GPU timestamp rows are written every 30th captured frame to
`snapframe_gpu.csv` (capture copy, luma, forward flow, backward flow,
consistency and total chain). During the active seven-phase suite, every 120th accepted flow pair, the two flow
fields and consistency mask are copied into a host-visible staging buffer;
summary statistics go to `snapframe_flow.csv`. A spatial vector dump is
written every 600th accepted pair during the suite to `snapframe_flow_vectors.csv`. Readback is
performed only after the active command list completes and is not consumed by
the runtime rendering path. These summaries are diagnostics, not ground-truth
accuracy tests; controlled synthetic translations and visual inspection are
still required. No rebuild is needed between phases.


## Current engineering gate — 2026-11-03

The latest uploaded run completed `static_scene`, `slow_camera_pan`, and
`fast_camera_rotation` (1,800 captured frames each). `moving_npc_combat`
started but the log ends at approximately frame 5,640; phases 5–7 and the
`TEST_SUITE_COMPLETE` marker are absent. This is an incomplete run, not
evidence of a crash.

The sampled flow CSV is structurally readable and reports no non-finite
samples in the available rows. It nevertheless shows frequent vectors
saturated at the +/-4 luma-texel search boundary, especially during the
first two phases. A high boundary count means the matcher may be reporting
motion outside its search range; it is not a valid large-motion estimate.
Some near-zero-motion samples have high confidence and consistency, but this
does not establish accuracy across the scene.

The GPU timing CSV reports very different times for the forward and backward
dispatches even though both use the same kernel and workload. Timestamp
results are therefore provisional until the DXVK/Vulkan query stages, query
availability and command-list completion ordering are verified. The spatial vector CSV exists in the uploaded Git tree (7,866,563 bytes), so it is not empty. The connector did not expose its full contents because of the large blob; numerical inspection of the vector rows remains outstanding.

### Implementation order from this gate

1. Make vector readback completion and file output observable; ensure the
   600-pair dump produces rows or an explicit failure reason.
2. Audit timestamp query lifecycle and Vulkan timestamp stage semantics.
   Do not optimize based on the current per-stage asymmetry.
3. Add deterministic GPU validation scenes: identical frames (expected zero
   displacement) and known translated patterns (known sign and magnitude).
   Compare every output cell against ground truth, including image borders.
4. Correct search-range/boundary behavior and confidence calibration based
   on those tests; do not treat dispatch acceptance as correctness.
5. Only after the numerical gate passes, implement two-sided warp,
   confidence-aware reconstruction and a generated-frame target.
6. Keep generated-frame presentation disabled until swapchain image lifetime,
   acquire/present semaphore ordering and pacing are proven.

The runtime currently has capture, luma, bidirectional coarse block matching,
forward/backward consistency and diagnostic readback only. No warp, blend,
intermediate-frame reconstruction, or generated-frame presentation is
implemented. Do not describe the current module as usable frame generation.

**Build/test status:** no APK was built and no GitHub Actions workflow was
triggered during this work. Local Android build and device validation remain
pending.


### Variable flow-grid density — source implementation added

The `flow_scale_percent` setting now controls the motion-grid density. The
base stride is 8 half-resolution luma texels at 100%; lower percentages
increase the stride. Both the block matcher and forward/backward consistency
derive the same stride from the luma and flow extents. The consistency
dispatch now binds the current luma view as a fourth resource so projected
coordinates use the correct grid spacing.

For 2448x1080 input, the intended grids are 153x68 at 100%, 112x50 at 75%,
77x34 at 50%, and 39x17 at 25%. The default 50% setting therefore reduces
flow-cell count by approximately four times relative to 100%, but it also
reduces spatial detail. These are calculated dimensions, not measured GPU
performance. The shader pipeline and four-binding dispatch have not yet been
compiled or validated on device; verify them with zero-motion and known-offset
tests before using the resulting flow for reconstruction.


### Latest diagnostic findings and matcher revision — 2026-11-03

The latest `frame_gen/Logs` upload was captured with 50% flow scale
(77x34 grid). Two phases completed; fast camera rotation reached about
1,500/1,800 frames before the process exited normally. There is no
`TEST_SUITE_COMPLETE` marker and phases 4–7 were not run.

Spatial dumps are now available and contain eight complete 77x34 fields.
During pan and rotation, many vectors reached the former +/-4 search limit
and forward/backward consistency was poor. A near-static dump at frame 4801
showed mean confidence 0.987 and mean consistency 0.988. These results
confirm that the old fixed-radius search is not yet reliable enough for
reconstruction.

The matcher has been revised to a coarse-to-fine search: 9x9 candidates
spaced two luma texels apart over +/-8, then 3x3 refinement around the best
coarse match. This gives a +/-9 effective range using 89 candidate positions.
The host boundary-saturation diagnostic now uses the matching +/-9 threshold.

This change has not been GLSL-compiled, built into an APK, or tested on the
device. Do not enable reconstruction/presentation based on this source change.
Next device run should repeat static, pan and rotation tests at 50% scale and
compare boundary counts, confidence and consistency against this upload.
A deterministic known-offset validation remains mandatory.

### Midpoint reconstruction compute pass (2026-11-03)

The new `midpoint_warp.comp` pass samples the two retained color frames,
current-to-previous and previous-to-current RGBA16F motion fields, and the
R16F forward/backward consistency mask. It offsets each color lookup by half
of the estimated inter-frame displacement, weights the two samples by local
motion confidence, and falls back to a conservative 50/50 blend when motion
evidence is weak. Output is a device-local RGBA16F image.

The pass is registered and dispatched through a state-preserving DXVK compute
helper. It is timed as `midpoint_warp` in `snapframe_gpu.csv`; the event log
records `midpoint_resources_ready/unavailable` and
`midpoint_warp_ok/failed`. This confirms only that a midpoint image was
written by an accepted GPU dispatch. It does not establish visual correctness,
occlusion quality, or successful presentation.

**Presentation integration:** after the real frame is submitted, DXVK now
acquires another WSI image, blits the SnapFrame midpoint into it, renders the
HUD, and queues the additional present. LSFG retains priority: SnapFrame does
not run a second generation loop while LSFG is actively generating. The
midpoint image is one-shot per real frame and is rejected on device/extent/
format mismatch. `generated_frames` increments only after the additional
present has been queued; this is not a measurement of scan-out completion.

The new path still needs a complete native APK build and device validation.
Check `midpoint_present_requested`, `midpoint_present_queued` and
`midpoint_present_failed` in `snapframe_events.csv`, plus the generated
count in the performance log. Visual quality, pacing, semaphore behavior
under stress, and GPU cost are not yet validated. LSFG remains unchanged.
