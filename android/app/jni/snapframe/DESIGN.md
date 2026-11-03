# SnapFrame Generation — design and integration plan

## 1. Scope and constraints

SnapFrame is a new, original frame-generation implementation for this project.
It must not depend on Lossless.dll, extracted LSFG SPIR-V, or LSFG shader IDs.
The existing `android/app/jni/lsfg` implementation remains untouched.

Primary target is Android/Vulkan on Adreno. All full-resolution image processing
must remain GPU-resident. CPU work is limited to scheduling, resource lifetime,
configuration, and diagnostics. No frame may be read back to the CPU in the
normal path.

The current repository already has a DXVK-native LSFG host and callbacks. The
game target links `lsfg_host`; the top-level JNI CMake adds `lsfg`; the LSFG
present path receives dimensions/device information and its generation path
uses DXVK command-list callbacks. These are useful integration precedents, but
SnapFrame must own its resources, shaders, state, and callbacks.

## 2. Proposed hybrid algorithm

### Stage A — frame history and preprocessing

- Capture two consecutive *real* rendered frames, A and B, at the point where
  their DXVK images are valid for sampling.
- Keep images on the same Vulkan device and avoid CPU readback.
- Convert/prepare a compact luminance representation; retain color source
  images for final reconstruction.
- Build a small Gaussian/luminance pyramid. Start with 4 levels and tune from
  measurements rather than copying LSFG's seven-level pyramid.
- Track frame dimensions, format, device generation, and monotonically
  increasing frame sequence. Reset history on resize, swapchain recreation,
  device loss, or discontinuity.

### Stage B — bidirectional motion estimation

- Estimate A→B and B→A motion independently at the coarsest level.
- Use hierarchical block matching with a bounded search window, initially
  8x8 or 16x16 blocks; exact block size/search range are tunables.
- Refine the vector field at finer pyramid levels using a small local search
  and image-gradient refinement.
- Store vectors in a compact RG16F/RG16 format where supported; validate
  storage-image and format support and provide a supported fallback.
- Keep search bounds and dispatch dimensions safe for odd image sizes.

### Stage C — confidence and occlusion handling

- Forward/backward consistency: compare a vector with the reverse vector at
  its projected endpoint.
- Combine consistency, local luminance residual, and gradient strength into
  a confidence value.
- Mark out-of-bounds projections and inconsistent vectors as unreliable.
- Dilate/feather occlusion boundaries at reduced resolution; avoid expensive
  full-resolution morphology in the first prototype.
- Do not blindly blend both warped frames where confidence is low.

### Stage D — intermediate-frame reconstruction

- For interpolation fraction t, warp A forward by t and B backward by (1-t).
- Use confidence-weighted blending where both projections are valid.
- Prefer the more reliable source near disocclusions; use conservative
  neighborhood reconstruction only as a fallback.
- Preserve alpha/format behavior and handle HDR only after SDR is validated.
- Initial target is one intermediate frame (2x output cadence). Multipliers
  above 2 are deferred until the basic path is stable.

### Stage E — temporal stabilization

- Add only after A/B reconstruction works: reject sudden vector changes,
  damp unstable low-confidence regions, and reset on scene cuts.
- Avoid temporal trails by never allowing stale history across a detected cut
  or a large frame-time gap.

## 3. Module layout

Planned ownership under `android/app/jni/snapframe/`:

- `snapframe_host.*`: lifecycle, configuration, runtime enable/disable.
- `snapframe_types.h`: API-neutral configuration and frame/stat structures.
- `snapframe_dxvk.*`: DXVK resource wrappers and command-list dispatch.
- `shaders/*.comp`: Vulkan compute shaders, compiled to SPIR-V by the
  project's verified shader build path.
- `snapframe_profiler.*`: per-stage GPU timestamps and CSV output.
- `DESIGN.md`: algorithm decisions, measured limits, and integration status.

The host API and DXVK capture backend exist. The first GLSL preprocessing
kernel now has an R16F output allocation, sampler, DXVK compute shader/pipeline
registration, and a state-preserving dispatch helper on the active command
list. This validates the first GPU preprocessing stage only; the profiler,
motion estimation, interpolation, and generated-frame presentation remain
unimplemented.

## 4. Integration plan

### Phase 0 — preserve baseline

- Keep this work on branch `frame_gen`, based on `snap-patch-5`.
- Do not modify LSFG files or change the default frame-generation mode.
- Establish a reproducible test scene and capture baseline real FPS, frame
  pacing, GPU time, memory use, and device temperature.

### Phase 1 — verify DXVK capture/presentation contracts

- Trace the existing LSFG registration hooks and the exact DXVK locations
  where source images, device, queue, and command list are available.
- Determine image layout/access transitions, queue ownership, synchronization,
  swapchain lifetime, and when a source image can safely be retained.
- Identify whether the Android SDL/Vulkan presentation path can schedule an
  additional image between real presents without breaking acquire/present
  semaphores or frame pacing.
- Document the chosen hook and lifetime rules before writing the GPU backend.

### Phase 2 — standalone GPU prototype

- Add a SnapFrame-owned DXVK backend without registering it in the runtime.
- Implement image copy/format conversion and luminance pyramid first.
- Add GPU timestamp queries and a CSV with capture, pyramid, flow, confidence,
  warp, and composite timings.
- Validate output against captured frame pairs using a debug/offline path.
- Confirm resource cleanup on resize and device recreation.

### Phase 3 — first usable interpolation

- Implement bidirectional coarse-to-fine block matching.
- Add forward/backward confidence and occlusion masks.
- Implement two-sided warp and confidence-weighted blend for t=0.5.
- Add conservative fallback for invalid vectors.
- Validate visual quality on camera pans, foliage, HUD, particles, animation,
  scene cuts, and fast motion.

### Phase 4 — runtime integration behind a separate switch

- Add an explicit engine selector: Off / LSFG / SnapFrame.
- Register SnapFrame callbacks only when selected; never run LSFG and
  SnapFrame generation simultaneously.
- Reuse DXVK's active command-list scheduling only after synchronization
  behavior is proven. Do not submit ad-hoc work to an unrelated queue.
- Integrate generated-image presentation only after Phase 1 proves a safe
  Android swapchain/pacing design.
- Preserve a reliable fallback to real frames if generation fails.

### Phase 5 — tuning and release gate

- Add Performance / Balanced / Quality presets controlling flow resolution,
  pyramid levels, search radius, and refinement passes.
- Compare against LSFG and generation-off in the same scene, resolution,
  multiplier, and thermal state.
- Record real FPS separately from displayed FPS; report generated-frame
  count, dropped frames, per-stage GPU time, memory, and latency.
- Do not enable by default unless it improves perceived motion without
  unacceptable input latency, artifacts, or sustained GPU overload.

## 5. Initial performance targets (hypotheses, not guarantees)

- No CPU readback in the normal path.
- One generated frame between two real frames.
- Reduced-resolution motion estimation; full-resolution work limited to
  reconstruction/composite.
- GPU stage timings reported independently; never infer displayed FPS from
  compute time alone.
- Graceful bypass when GPU budget, history validity, or synchronization is
  insufficient.

Numeric budgets will be set only after the first measurements on the target
device. Existing LSFG CSV timings are useful reference data but are not a
direct performance prediction for this different algorithm.

## 6. Risks and open questions

- DXVK image access at present may not provide a stable image lifetime for
  asynchronous processing.
- Android swapchain pacing may require a presentation-path change beyond a
  compute shader. This is a gating architectural question.
- Optical-flow quality may degrade on disocclusion, foliage, particles, HUD,
  motion blur, and low-texture regions.
- Adreno support for chosen storage formats and subgroup operations must be
  queried at runtime; do not assume a format is supported.
- Additional compute can reduce real FPS or increase input-to-photon latency.
- The LSFG GPU CSV contains measurements of a different shader pipeline and
  cannot be used as a direct estimate of SnapFrame cost.

## 7. DXVK integration findings

The repository's `D3D9SwapChainEx::PresentImage` path already records the
source-to-WSI blit into `m_context`, renders the DXVK HUD, and then submits the
same command list. LSFG has an independent copy callback in this path. SnapFrame
now has its own callback, invoked after the blit but before HUD composition.
The callback receives the active `DxvkContext`, the WSI `DxvkImageView`, and
`DxvkDevice`. It checks transfer-source usage, dimensions and format, then
records a copy into one of two SnapFrame-owned DXVK images in the same command
list. No CPU readback or separate queue submission is used.

A device-destroy callback releases the two history images after device idle and
before the DXVK device allocator is destroyed. History is reset on size/format/
device changes and when capture is toggled. Capture is disabled by default and
can currently be enabled with `<game-directory>/port/snapframe.conf` containing
`enabled=1`. Bootstrap opens `<game-directory>/port/snapframe.log` as a separate
per-launch diagnostic log. DXVK present-path markers are forwarded through an
atomic callback into this file, alongside host-side capability, allocation,
skip and capture counters. The file is truncated at game startup and mirrored
to stderr for compatibility with existing log collection.

Important limitation: the captured source is the WSI image after blitting the
game backbuffer but before the HUD overlay. This avoids baking the DXVK HUD into
motion estimation, but it does not yet separate the game's own HUD/UI from the
3D scene. The host checks the source image's `VK_IMAGE_USAGE_TRANSFER_SRC_BIT` before
recording the copy. The DXVK present path now emits one-time diagnostics for
first PresentImage entry, WSI image usage/format, callback dispatch, and host
callback entry. The copy-hook pointer is atomically published/read to avoid a
setter-versus-present data race. These markers distinguish failure to reach
PresentImage from an unsupported source usage or a history allocation/copy problem. The two history images are allocated only after checking sampled-image support
and querying the exact `TRANSFER_DST | SAMPLED` image-usage combination and
requested extent for their WSI format. When an enable/disable or history-reset
request is pending, the next DXVK present callback consumes it before the
inactive-state return, releasing the cached history images and sampler even
when SnapFrame has just been disabled. The host now also reports `R16_SFLOAT` luminance-output capability: optimal-tiling
storage and sampled feature bits, the concrete 2D image configuration, the
`shaderStorageImageExtendedFormats` device feature, and the half-resolution
extent limits. This is diagnostic only; capture continues even when the
compute output is unsupported.

The Android NDK build does not compile standalone GLSL. SnapFrame's CMake
configure step now invokes the host-side
`tools/compile_snapframe_shaders.py` when Python 3, `glslangValidator`, and
`spirv-val` are available, producing a C++ SPIR-V header before compiling the
host target. The NDK consumes that header and does not need shader tools. If
the host tools are absent, CMake warns and leaves capture enabled while
disabling the luma dispatch; the dedicated runtime log reports this condition.

## 8. Current status

- [x] Create isolated branch from `snap-patch-5`.
- [x] Create isolated module directory and initial host/API scaffold.
- [x] Record algorithm and staged integration plan.
- [x] Trace the DXVK source-image and active command-list hook contract.
- [x] Add an independent SnapFrame copy callback before HUD composition.
- [x] Allocate a two-image GPU-resident history ring and device-lifetime cleanup.
- [x] Add opt-in configuration bootstrap; capture remains disabled by default.
- [ ] Build APK and verify the new present-path/callback diagnostics on device.
- [ ] Confirm two real frames are captured before adding compute work.
- [x] Check sampled-image support before allocating the WSI-format history images.
- [x] Add host-side `R16_SFLOAT` storage/sampled-image capability and extent checks (device runtime result still pending).
- [x] Add a dedicated `port/snapframe.log` and route DXVK present/callback diagnostics into it.
- [x] Add first luminance downsample GLSL source and a host-side GLSL-to-SPIR-V header generator.
- [x] Add CMake host-side GLSL-to-SPIR-V generation and validation (requires Python 3, glslangValidator and spirv-val on the build host).
- [x] Implement R16F output allocation, sampler/descriptor setup and first checked DXVK dispatch.
- [ ] Verify shader compilation, pipeline creation and successful luma dispatch on the target GPU.
- [x] Add first coarse GPU block-matching motion-estimation pass (device validation pending).
- [ ] Validate motion vectors/confidence numerically on device and measure GPU cost.
- [ ] Implement flow refinement, consistency checks and reconstruction.
- [ ] Add runtime selector and safe presentation integration.
- [ ] Run device A/B tests and tune.


### Phase 2a — first preprocessing kernel

The first source kernel is `shaders/luma_downsample.comp`. It computes
Rec.709 luminance from a sampled source frame and reduces 2x2 texel blocks
into a half-resolution `R16_SFLOAT` output. The host-side
`tools/compile_snapframe_shaders.py` compiles it for Vulkan 1.1, validates the
SPIR-V with `spirv-val`, and emits a C++ word-array header. This keeps shader
compilation out of the Android NDK build.

The kernel is dispatched after each captured frame on the same active DXVK
command list. The host checks sampled-image support for the WSI history format
and storage+sampled support for `VK_FORMAT_R16_SFLOAT`, including the concrete
2D image configuration, `shaderStorageImageExtendedFormats`, and ceil-half
extent limits. It creates the R16F output, sampled source sampler, compute
shader and descriptors, then uses `dispatchChecked()` through a state-preserving
DXVK helper. The helper restores shared resource slots and the previous compute
shader so the internal pass does not poison later game draws. A successful
`luma dispatch accepted` log means DXVK recorded the dispatch; it does not yet
prove numerical output correctness. The shader is compiled and SPIR-V validated
on the build host during CMake configure when the required tools are available.

## Preprocessing resource failure path

The luma compute shader is registered with the DXVK pipeline manager only
after all required luma resources have been created. An incomplete setup or
exception releases partial output-image, image-view, and sampler references;
the capture history remains available. The dedicated diagnostic log reports
the individual luma resource states and invalid callback arguments so a device
test can distinguish capture-path failures from compute-resource failures.

This is host-side failure handling only. Successful command recording is not
evidence that the GPU produced numerically correct luminance values.


### Phase 2b — coarse temporal motion field

A two-slot R16F luma history now feeds `shaders/block_match_flow.comp`. The flow field is stored at 1/16 source resolution in RGBA16F and contains displacement in half-resolution luma texels, a best/second-best SAD confidence estimate, and normalized matching cost. The 9x9 search over 3x3 patches is intentionally a first baseline, not production optical flow. Runtime checks gate the RGBA16F storage image; if unsupported, capture and luma preprocessing remain active.

The DXVK flow dispatch binds previous luma, current luma and one output image in slots 0..2, preserving all three prior resource slots and compute state. The host records two dispatches for each adjacent-frame pair: current-to-previous and previous-to-current, into separate RGBA16F images. A pair is counted only when both dispatches are accepted. The host dispatches only when both temporal luma slots are valid and the current luma dispatch was accepted. No generated frame is reconstructed or presented yet. The device test must check for `bidirectional flow pair accepted`; that confirms command recording for both directions, not numerical motion-field correctness. Sparse GPU readback now records aggregate flow/mask statistics and periodic per-cell vectors; inspect those outputs before trusting vector values.


The two directional fields are preparatory inputs for the next consistency pass. They are not yet checked against one another, confidence is only a local best-versus-second-best SAD separation, and no occlusion/disocclusion classification is performed. The motion images are reused for each subsequent adjacent-frame pair; any later interpolation consumer must read them before the next pair overwrites them or introduce an explicit per-frame lifetime/ring.


## 8. Implemented forward/backward consistency milestone

The current branch now contains a GPU consistency kernel in
`shaders/flow_consistency.comp`, a dedicated DXVK dispatch helper, runtime
R16F output allocation, shader registration, and host-side dispatch/failure
counters. The pass samples the current-to-previous and previous-to-current
RGBA16F fields, follows the forward vector to the reverse field, computes the
cycle-closure error, and attenuates the result using local match confidence.

This is only a first confidence/validity estimate. It is not a full occlusion
or disocclusion classifier, and it has not been numerically validated against
ground truth. No warping, interpolation, HUD composition, or generated-frame
presentation is included. Device shader compilation and APK build have not
been run as part of this change.

Next validation gate:
1. Build the generated SPIR-V header with glslangValidator and spirv-val.
2. Build the Android APK locally and run it on the target Adreno device.
3. Confirm `forward/backward consistency accepted` increments with no failures.
4. Inspect `snapframe_gpu.csv`, `snapframe_flow.csv`, and
   `snapframe_flow_vectors.csv` for valid timings, non-finite values, vector
   direction, confidence and consistency-mask behavior.
5. Add controlled synthetic zero-motion and known-translation GPU tests to
   establish vector direction and scale against ground truth.
6. Only after numeric validation, use the mask in a two-sided warp prototype.


## 9. Single-build diagnostic campaign

The runtime config supports `diagnostic_suite=1` and
`diagnostic_phase_frames`. It labels seven gameplay phases: static scene,
slow pan, fast rotation, moving NPC/combat, foliage/particles/water,
HUD/inventory/map, and sustained stress. Each phase logs a manual action
prompt and counts captured frames, luma dispatches, bidirectional flow pairs,
consistency dispatches and skips. Phase labels are based on captured-frame
count, not scene recognition. The suite runs only the current capture/luma/
flow/consistency chain; it does not validate numerical shader output or
present generated frames.


## 10. Diagnostic completion, GPU timing and flow readback

The seven-phase campaign now closes the seventh phase on its actual final
captured frame (12,600 frames with the default 1,800 frames per phase).
Completion is latched, so subsequent gameplay continues through capture and
compute without an eighth phase, a synthetic `complete` phase result, or
repeated suite-completion markers.

When `profiling_enabled=1`, timestamp queries are recorded in the active DXVK
command list every 30th captured frame. Timing rows cover capture copy, luma,
forward flow, backward flow, consistency and the complete capture-to-
consistency chain. Results are written asynchronously from command-list
completion callbacks to `port/snapframe_gpu.csv`; SnapFrame does not submit a
separate queue or synchronously wait for the GPU.

While the seven-phase diagnostic suite is active, every 120th accepted bidirectional flow pair, both RGBA16F motion fields and
the R16F consistency mask are copied into a host-visible, host-coherent
staging buffer on the same command list. CPU inspection happens only after its
completion callback. `port/snapframe_flow.csv` records grid dimensions, mean
displacement, mean vector magnitude, mean local confidence, mean matching
cost, mean consistency, percentage of mask values above 0.5, non-finite
sample count and completion status. This sampled readback is diagnostic only
and is not used by the normal rendering path.

Mean statistics can expose zero/invalid output, non-finite values, broad
directional bias and confidence-mask behavior, but cannot establish
ground-truth optical-flow accuracy by themselves. Controlled synthetic
translations and visual vector inspection remain required before the fields
can be trusted for interpolation. Unavailable timestamp samples are marked
with an explicit status instead of being reported as zero-time passes.
Profiling and readback use the existing `profiling_enabled` setting.


A second sparse spatial dump is written to `port/snapframe_flow_vectors.csv`
every 600th accepted flow pair during the active suite (every fifth aggregate readback). Each row
contains the grid coordinate, forward and backward displacement/confidence/
matching-cost values, and the consistency mask value. This preserves the
spatial field for external vector visualization while limiting file growth.


## 11. Validation-first implementation status — 2026-11-03

The latest uploaded diagnostic run has completed three full 1,800-frame
phases and entered the fourth phase, but the log ends around frame 5,640.
There is no seventh-phase result or `TEST_SUITE_COMPLETE` marker. The
available flow samples contain no non-finite values, but many vectors reach
the +/-4 luma-texel search boundary. Boundary saturation is evidence that the
search range may be insufficient or that matches are ambiguous; it must not
be interpreted as validated large motion.

The timing CSV reports a large forward/backward asymmetry for the same
compute kernel and dimensions. Until the Vulkan timestamp query lifecycle
and timestamp stage placement are audited, per-stage GPU timings are
diagnostic observations only and must not drive performance tuning. The `snapframe_flow_vectors.csv` blob exists in the uploaded Git tree (7,866,563 bytes), so it is not empty. The connector did not expose its full contents because of the large blob; numerical inspection of the rows remains outstanding.

### Required numerical gate before reconstruction

- Verify the asynchronous readback callback is registered at the intended
  600-pair cadence, reaches its success path, and writes a complete header and
  rows. Log distinct statuses for staging allocation, command recording,
  callback completion, map/invalidate, and file write failures.
- Audit `DxvkGpuQuery`, timestamp writes, query-pool reset, completion
  callbacks, queue-family timestamp support, and Vulkan timestamp stage
  semantics. Confirm that both directional dispatches are measured within
  comparable command-stream boundaries.
- Add a deterministic validation mode that supplies an identical image pair
  and translated image pairs with known offsets. Validate zero-motion output,
  vector sign, scale, confidence and consistency for interior and boundary
  blocks. Record expected and observed values.
- Fix the matcher based on ground-truth failures: search-range saturation,
  low-texture ambiguity, patch boundaries and tie-breaking. Confidence must
  reflect ambiguity, not just the best/second-best numerical gap.
- Re-run the same validation set before using flow in a reconstruction pass.

### Reconstruction milestones (not yet implemented)

After the numerical gate passes, implement a separate GPU warp/reconstruction
prototype for a single midpoint (t=0.5): warp A forward and B backward, sample
the confidence/validity mask, choose the more reliable source near invalid
projections, and blend only mutually supported samples. Add a debug output
that can be read back for offline comparison, but keep normal rendering and
presentation untouched.

Only after the midpoint output is visually and numerically checked should
SnapFrame acquire a generated-frame target and integrate presentation. The
presentation design must explicitly account for WSI image lifetime, queue
ownership, acquire/present semaphores, frame pacing, resize and device loss.
The engine selector must remain separate from LSFG, with a safe real-frame
fallback. LSFG sources and runtime behavior are out of scope for this branch's
SnapFrame work.

**Current state:** capture, luma preprocessing, bidirectional coarse flow,
forward/backward consistency and sparse diagnostics are implemented at source
level. Motion accuracy, shader runtime behavior, reliable per-stage GPU
timings, midpoint reconstruction and generated-frame presentation remain
unverified or unimplemented. No APK build or device test was performed for
this milestone.


### Flow-grid scale implementation — 2026-11-03

`flow_scale_percent` is now applied to the coarse motion-grid allocation and
dispatch geometry. The base stride is 8 half-resolution luma texels at 100%;
the configured percentage increases that stride and reduces the number of
flow cells. The block matcher derives its luma-space cell stride from the
actual input/output extents. The consistency shader receives the current luma
view and uses the same stride and clamped block centers when projecting into
the reverse field. Allocation, dispatch, consistency output and sparse
readback therefore share one flow-grid extent.

For the target 2448x1080 image (half-resolution luma 1224x540), expected
grid dimensions are approximately:

| flow scale | luma stride | flow grid |
|---|---:|---:|
| 100% | 8 | 153x68 |
| 75% | 11 | 112x50 |
| 50% | 16 | 77x34 |
| 25% | 32 | 39x17 |

These are source-derived geometry expectations, not measured performance
results. The larger stride reduces spatial density and may miss small or
thin moving objects; displacement values remain expressed in half-resolution
luma texels. The shader changes and four-binding consistency pipeline have
not yet been compiled or exercised on the Adreno device. Verify dimensions,
zero-motion behavior, known translations, borders and consistency projections
before using the scaled field for reconstruction.
