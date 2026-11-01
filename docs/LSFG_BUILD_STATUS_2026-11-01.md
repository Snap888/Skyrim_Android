# LSFG source status — 2026-11-01

Branch: `snap-patch-3`  
Repository: [Snap888/Skyrim_Android](https://github.com/Snap888/Skyrim_Android)  
Baseline HEAD before checked-dispatch patch: [`5832467751ec3f25214bad8d79414088b9791749`](https://github.com/Snap888/Skyrim_Android/commit/5832467751ec3f25214bad8d79414088b9791749)

## Scope of this update

This is a source review and documentation refresh. No Android NDK build, APK
packaging, device installation, or GPU runtime test was performed during this
review. It must not be read as a build-success or LSFG-working claim.

## Current source path

`source Present → DXVK history copy → mipmaps (255) → Alpha (290–293) →
Beta (298–302) → Gamma/Delta → Generate (256) → offscreen target → DXVK
presentation`

The source audit of `android/app/jni/lsfg/lsfg_step3g.cpp` confirms:

- Two alternating history slots are selected with `g_pp & 1`.
- The source image is copied through `DxvkContext::copyImage()`.
- Mipmaps dispatch uses shader 255 and seven flow-pyramid views.
- Shared Alpha/Beta preparation is called through
  `dispatchLsfgSharedPasses()`.
- Generation is gated by warm-up, both history views, and shared-pass readiness.
- Generation uses an offscreen target returned by `getLsfgGenerationTarget()`
  and records `dispatchLsfgGeneration()` into the supplied context.
- Completion callbacks report command-list fence retirement, not pixel values.

All GPU passes must remain on the active DXVK command list. No separate
`vkQueueSubmit` path and no additional LSFG shader hist-copy pass are intended.

## Current issue

The latest user observation is: static scenes are sharp, while rapid motion
produces blur/distortion, including at multiplier x4. This is not yet reproduced
or diagnosed from a fresh log.

## Next investigation order

1. Audit descriptor bindings against the host-side resource-slot mapping.
2. Verify image formats/usages/extents and all mip-level dimensions.
3. Verify Alpha history slot recurrence and Beta flow producer/consumer mapping.
4. Verify Gamma/Delta resource mapping, UBO layouts/values, and per-output
   timestamps.
5. Verify output target reuse/lifetime and actual producer-consumer barriers
   using DXVK's existing hazard tracking before adding any new barrier.
6. Compare pass order and resource assignments against WinNative only where
   source evidence is available; do not guess shader variants or constants.

## Required next test artifact

Build and install an APK from the reviewed HEAD, then test multiplier x2 first
with a short rapid camera pan. If x2 is stable, repeat at x4.

Required:
- `port/lsfg.log` — full file, including initialization, resource creation,
  dispatch, and generated-frame presentation.
- `port/log.txt` and Android logcat — if startup fails, the game crashes,
  hangs, or Vulkan reports an error.

A `command recorded` message or successful completion callback alone does not
prove correct GPU output. Visual inspection must check ghosting, blur, warped
edges, black/corrupt frames, pacing, and generated-frame count.

## Status checklist

- [x] Source path and DXVK command-list integration reviewed.
- [x] Documentation synchronized with current source state.
- [ ] Android native build confirmed for reviewed HEAD.
- [ ] APK packaging confirmed.
- [ ] GPU pipeline/descriptor/dispatch runtime confirmed on device.
- [ ] Motion quality validated at x2 and x4.

## Runtime evidence update — 2026-11-01 (user test, 05:22)

The latest device log reports:

- 300/300 compute modules reflected successfully.
- History images allocated at 2448×1080.
- The real WSI image hook is active.
- DXVK command-list history copies continue to retire successfully; the
  supplied excerpt reaches copy/completion #5400.
- Mipmaps dispatches are requested at 1713×756 flow resolution (27×12 groups).
- Alpha/Beta resources initialize, and Gamma/Delta/Generate resources initialize.
- Generate requests are recorded at source-frame checkpoints through 5400.

Important limitation: the existing DxvkContext::dispatch() returns void.
Internally it silently skips vkCmdDispatch() when commitComputeState()
fails to resolve/commit a compute pipeline or pipeline state. LSFG then logs
dispatch / command recorded unconditionally. These log lines therefore prove
that the host reached the dispatch call sites, but do not prove that DXVK
actually emitted every vkCmdDispatch.

The source now adds dispatchChecked() and uses it for all LSFG compute passes
(mipmaps, Alpha, Beta, Gamma, Delta, Generate). A failed pipeline/state commit
propagates as a failed LSFG pass, emits a pass/stage/shader diagnostic, and
prevents the offscreen target from being counted as a generated frame. The
existing dispatch() API remains unchanged for other DXVK callers.

The D3D9 presentation source was also checked: generated targets are passed
to DxvkSwapchainBlitter::presentImage() and then submitted through SubmitPresent().
The output presentation hook is therefore wired in source; the remaining
question is whether compute passes really commit and whether output pixels
are correct.

This change has not yet been built with Android NDK or tested on-device.

## WinNative cross-check — 2026-11-01

Compared the Android implementation against the current
`Snap888/WinNative` LSFG implementation in
`app/src/main/cpp/winlator/vk/lsfg/`:

- Shader IDs match the reference arrays:
  - Mipmaps: 255
  - Alpha: 290–293
  - Beta: 298–302
  - Gamma: 280, 282–285
  - Delta: 280, 286–289, 281, 294–297
  - Generate: 256
- Alpha uses three temporal output slots. Beta reads them in the same
  `(history+1), (history+2), history` order and uses the same 8x8 / 32x32
  dispatch split.
- Gamma and Delta use the same per-stage descriptor roles, temporal indices,
  intermediate image ordering, paired stage order, and motion/detail dependencies.
- Generation slot calculation and slot timestamps match
  `LsfgGenerationSlot` / `LsfgSlotTimestamp` in WinNative.
- Internal image formats match the reference: RGBA8 intermediates, R8 flow,
  and RGBA16F motion outputs. The UBO structure is 48 bytes in both sources.

This is a source-level correspondence check, not proof of pixel-equivalent
execution. The Android path still differs in synchronization implementation:
it relies on DXVK command-list resource tracking instead of WinNative's
explicit Vulkan barrier builder. That must be validated on device.

## CI result for checked-dispatch commit

GitHub Actions run
[41266055424](https://github.com/Snap888/Skyrim_Android/actions/runs/41266055424)
completed successfully. The Java compilation job passed. The full arm64 APK
job was **skipped** because it only runs for an explicitly requested
`workflow_dispatch` with `build_apk=true`; this run therefore did not compile
the Android native C++/DXVK/LSFG sources.

## Remaining blockers before a device test

The source-level comparison did not identify a confirmed pass-order or
descriptor-role mismatch against WinNative. Do not treat this as LSFG-ready:
the native Android build has not run, compute pipeline creation has not been
verified on device, and output pixels have not been read back or visually
validated. Next work must focus on native compilation and runtime pipeline
creation/failure details, followed by image-format/descriptor validation and
a visual x2 motion test. No speculative shader variant substitutions were made.
