# LSFG source status — 2026-11-01

Branch: `snap-patch-3`  
Repository: [Snap888/Skyrim_Android](https://github.com/Snap888/Skyrim_Android)  
Reviewed HEAD: [`98ac2957fede8c805375a86ecbcba91c79074091`](https://github.com/Snap888/Skyrim_Android/commit/98ac2957fede8c805375a86ecbcba91c79074091)

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
