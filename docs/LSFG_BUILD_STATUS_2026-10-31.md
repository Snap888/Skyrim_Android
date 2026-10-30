# LSFG build status — 2026-10-31

Branch: `snap-patch-3`  
Repository: [Snap888/Skyrim_Android](https://github.com/Snap888/Skyrim_Android)

## Current source revision

The latest source fix at the time of this note is commit
[`00c8b982828097d1072baa31dfbfd6382deb286a`](https://github.com/Snap888/Skyrim_Android/commit/00c8b982828097d1072baa31dfbfd6382deb286a).

It fixes the compile errors reported by Android Actions run
[41124957687](https://github.com/Snap888/Skyrim_Android/actions/runs/41124957687):

- `Rc<DxvkImageView>` null checks now compare explicitly with `nullptr`.
- The generation path now calls the public `DxvkContext::clearImageView()`
  API instead of the private `clearImageViewCs()`.

The failed run stopped while compiling `lsfg_step3g.cpp` and
`lsfg_dxvk_generation.cpp`; it did not reach APK packaging. The run predates
commit `00c8b98`, so it does not validate these fixes.

## LSFG implementation state

The branch contains the intended DXVK command-list compute path:

`history copy → mipmaps (255) → alpha (290–293) → beta (298–302) → gamma/delta → generate (256)`.

The history copy and compute passes are recorded through the active
`DxvkContext`; this is not a separate `hist-copy` shader pass and does not
submit an independent Vulkan queue operation.

The source implementation is present, but source presence and command recording
are not proof that all shaders create valid pipelines or produce correct pixels.
The full chain still requires an Android runtime test on the target device.

## Build and test ownership

The project owner builds the APK manually. Do not treat this document or a
successful compile as evidence that LSFG works at runtime.

After building and installing the APK, enable LSFG with the user's
`port/lsfg.conf` and test multiplier 2 first.

## Required logs

1. `<Skyrim folder>/port/lsfg.log` — required for LSFG validation.
2. `<Skyrim folder>/port/log.txt` — additionally required if the game crashes,
   hangs, fails to launch, or never reaches presentation.

For the first GPU-chain test, inspect for:

- `step3h: mipmaps DXVK resources ready`
- `step3h: mipmaps dispatch`
- successful Alpha/Beta resource initialization and dispatch messages
- `step3j: gamma/delta/generate DXVK resources ready`
- `step3j: generate command recorded`
- generated-frame presentation messages and absence of Vulkan/descriptor errors

Fence completion counters only confirm command-list retirement; they do not
confirm correct image contents. Validate the visible output for black frames,
corruption, ghosting, tearing, pacing, and generated-frame count.

## Status

- [x] Compile-error fixes committed to `snap-patch-3`.
- [ ] Rebuild Android APK from a revision containing those fixes.
- [ ] Confirm successful Android native build and APK packaging.
- [ ] Confirm all LSFG compute pipelines and dispatches on device.
- [ ] Confirm generated-frame image quality and frame pacing.

No APK is claimed to be built or runtime-validated by this note.
