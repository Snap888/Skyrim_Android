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
- [ ] Implement motion estimation and reconstruction.
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
