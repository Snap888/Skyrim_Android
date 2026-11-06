# DXVK Native 2.6.2 Android port map

## Pinned upstream source

- Repository: `doitsujin/dxvk`
- Release tag: `v2.6.2`
- Commit: `9d6f54a1ade20d1d27dd421024717a636f3d8c68`
- Annotated tag object: `e2373f9421f2c70583aea75c54219fbdcd67959c`
- Tag signature: GitHub reports `verified=true`.
- Build target: Android ARM64, DXVK Native, SDL2 WSI, D3D9 only for this game.

This document records a source-level compatibility review. It does not claim that 2.6.2 has been ported or built in this repository.

## Confirmed architectural differences

### 1. D3D9 present path

Upstream 2.6.2 `src/d3d9/d3d9_swapchain.cpp` uses a per-window context (`m_wctx`) and its presenter. `PresentImage`:
1. Ends the D3D frame and flushes.
2. Acquires a presenter image and `PresenterSync`.
3. Records the backbuffer blit on the DXVK CS thread via `EmitCs`.
4. Calls `ctx->synchronizeWsi(sync)`, flushes the command list, then `DxvkDevice::presentImage`.
5. Synchronizes frame latency and rotates D3D9 backbuffers.

The current Android baseline has a substantially different serialized present path:
- It uses `m_presenter`, `m_context`, `SynchronizePresent`, and `SubmitPresent`.
- It captures the source frame before HUD composition through `SnapFrameCopyHook`.
- It performs SnapFrame generation as a separate second WSI acquire/submit, with opportunistic acquire timeout 0.
- It tracks source and generated present results separately and reports generated acceptance only after the asynchronous present result is observed.
- It feeds source/generated submission pressure to Snap AI and applies present-mode changes at a serialized present boundary.

**Port requirement:** do not replace the current path with upstream `PresentImage` wholesale. The SnapFrame source capture point, generated-image submission, independent result tracking, present-mode changes, and AI pressure feedback must be reintroduced deliberately into the 2.6.2 command submission model. Preserve queue order and semaphore ownership; do not infer scan-out from `vkQueuePresentKHR` acceptance.

### 2. WSI and Android surface

Upstream 2.6.2 has a generic WSI driver abstraction and an SDL2 driver:
- `src/wsi/sdl2/wsi_platform_sdl2.cpp` dynamically loads SDL2 and queries SDL Vulkan instance extensions.
- `src/wsi/sdl2/wsi_platform_sdl2.h` implements platform, monitor, window and surface operations.
- `src/wsi/meson.build` includes SDL2 WSI as a normal backend.

The current Android baseline has a local SDL2 WSI adaptation and an Android-native surface path in `wsi_presenter_sdl2.cpp`, using `NativeSurface` and `vkCreateAndroidSurfaceKHR` rather than assuming SDL's desktop window/surface implementation.

**Port requirement:** add an explicit Android surface implementation to the 2.6.2 WSI layer, retain the current native-window lifetime rules, and ensure required instance extensions are enabled before instance creation. Do not silently fall back to a desktop SDL surface path on Android.

### 3. SnapFrame integration boundaries

DXVK remains the D3D9/Vulkan translation and present adapter. It must not own interpolation algorithms. The only interpolation backend remains SnapFrame Engine; Snap AI consumes timing/backpressure telemetry and chooses whether/how aggressively to generate.

Required integration points in a 2.6.2 port:
- Source image callback after game rendering and before HUD composition.
- A command-list-safe way to hand the source image/device/format/extent to SnapFrame without CPU readback.
- Generated image acquire/record/blit/submit as a distinct optional presentation.
- Distinct source and generated `VkResult` accounting.
- Serialized present-mode recreation and safe drain of outstanding source/generated work.
- Source and generated submission-wait telemetry to Snap AI.
- Device/swapchain destruction hooks that cannot call into destroyed SnapFrame state.

## Port sequence and gates

1. **Source baseline:** vendor or pin the exact upstream commit; retain the current baseline as the fallback pair.
2. **Build-only Android port:** add Android toolchain, SDL2 linkage and native-surface support; build upstream D3D9 ARM64 without SnapFrame hooks.
3. **Game ABI:** give the candidate a unique SONAME (for example `libdxvk_d3d9_262.so`), build a matching `libmain_262.so`, and verify both ELF architecture and exact `DT_NEEDED` linkage.
4. **SnapFrame source capture:** add capture only; validate source image lifetime/layout and no regression with FG disabled.
5. **Generated present:** add one generated image per source frame behind a runtime feature gate; preserve separate present-result slots and non-blocking generated acquire.
6. **Snap AI feedback:** port pressure/timing hooks only after source and generated presentation pass correctness tests.
7. **Device validation:** test FG off first, then FG on, on Adreno 740/Turnip. Compare source FPS, source submission wait, CPU Present P95, GPU stage timing, generated acquire failures, and long-run stability. Accepted presents are not a physical scan-out counter.
8. **Expose selector entry:** only package and show 2.6.2 after both matching libraries and all build/device gates pass.

## Present timing attribution correction (2026-11-06)

The baseline DXVK present instrumentation now reports handoff residuals separately from nested source/generated submission waits. Previously, `source_handoff` and `generated_handoff` included time already counted by `source_submission_wait` / `generated_submission_wait`. Summing those fields therefore double-counted the same wall-clock interval and made `untracked` artificially small or negative-clamped.

The updated measurements subtract both nested submission-wait deltas from the enclosing synchronization interval. The remaining handoff fields represent synchronization/dispatch overhead only; the submission wait fields remain independent. This makes the thresholded spike components disjoint for attribution. It does not change queue ordering, semaphore ownership, wait behavior, or AI throttle thresholds.

## Current status

- Baseline Android DXVK runtime remains the only packaged runtime.
- The launcher selector intentionally hides 2.6.2 until both candidate libraries exist.
- Upstream source has been inspected against the current Android baseline; the present path and WSI are not drop-in compatible.
- No 2.6.2 binary, Android port, or device test is claimed by this document.
- CI's native Android job must build both `dxvk_d3d9` and `main`, then verify the DXVK SONAME and the main runtime's exact `DT_NEEDED` dependency.

## Reusable runtime-pair ABI verifier

The Android CI now uses `tools/ci/verify_dxvk_runtime_pair.sh` for the candidate pair acceptance checks. It accepts a DXVK shared object, its matching game runtime, the NDK `llvm-readelf` and `llvm-nm` tools, and the expected DXVK SONAME. It checks both files are AArch64 ELF, verifies the DXVK SONAME, requires the D3D9 creation exports, and verifies that the runtime's dynamic section has an exact `DT_NEEDED` entry for that SONAME.

The current CI feeds the existing baseline DXVK and the small ABI-linkage probe into this verifier because generated Skyrim C sources are not present in the repository checkout. That proves the baseline DXVK ABI and the link edge in isolation; it does **not** claim the probe is a real `libmain.so`. When the 2.6.2 port produces both candidate objects, run the same verifier with `libdxvk_d3d9_262.so` and the actual `libmain_262.so` before allowing the launcher selector to expose the variant.


## Reproducible upstream source audit (2026-11-06)

The exact 2.6.2 upstream source is now materialized by
`tools/dxvk/checkout_native_262_source.sh`. The script fetches the signed
`v2.6.2` tag, rejects any resolved commit other than
`9d6f54a1ade20d1d27dd421024717a636f3d8c68`, checks the reviewed D3D9,
presenter, SDL2 WSI and Meson source files, and verifies the key
`acquireNextImage` / `EmitCs` / `synchronizeWsi` / `presentImage`
integration anchors. Android CI runs this audit on every branch update.

This makes the reviewed upstream source reproducible and catches accidental
tag drift or changes in the presenter contract before port work proceeds.
It deliberately reports `NOT_PORTED` / `SOURCE_ONLY`: no upstream DXVK
binary is built or packaged by this audit. The next engineering stage is the
Android-native WSI and SnapFrame present-adapter port, followed by a matching
Skyrim runtime link.


## Android WSI adapter seam audit (2026-11-06)

The current Android implementation points are now identified by their actual
source paths (the SDL2 WSI files live under `src/wsi/sdl2/`):

| Current Android file | Responsibility that must survive the 2.6.2 port |
|---|---|
| `src/wsi/sdl2/wsi_native_surface.h/.cpp` | Registers the game-provided `ANativeWindow*` behind the synthetic HWND; serializes registration/lookups with a mutex |
| `src/wsi/sdl2/wsi_presenter_sdl2.cpp` | Resolves the registered native surface and calls `vkCreateAndroidSurfaceKHR`; otherwise retains SDL Vulkan surface creation |
| `src/d3d9/d3d9_swapchain.cpp` | Owns source capture, generated-image second present, present-mode hook, source/generated completion and pressure telemetry |
| `CMakeLists.txt` | Explicitly compiles the Android WSI adapter and defines `DXVK_NATIVE`, `DXVK_WSI_SDL2`, Android linkage and Vulkan/SDL dependencies |

### Port boundary decision

Do not copy the current `wsi_presenter_sdl2.cpp` directly over upstream
2.6.2. First port the native-window registry as a small platform adapter,
then connect it to the 2.6.2 SDL2 WSI surface-creation path. The registry API
is deliberately narrow: synthetic HWND in, native window + extent out.
The 2.6.2 WSI port must additionally verify that
`VK_KHR_android_surface` is enabled at instance creation, that the window
remains alive until surface destruction, and that resize/replacement cannot
race a presenter using the previous native window.

The registry now owns one `ANativeWindow` reference for the active
registration, and `nativeSurface()` acquires a temporary lease for the
surface-creation call. This closes the lookup-versus-replacement race while
`vkCreateAndroidSurfaceKHR` is executing. It does **not** pin the window for
the lifetime of an already-created `VkSurfaceKHR`: the caller must still
retire the presenter/swapchain and destroy that surface before unregistering
or replacing the old registration. The 2.6.2 port must preserve this ordering
and add an explicit surface-lifetime test.

### Current source vs upstream source

- Current Android source list is a manually curated CMake target; upstream
  2.6.2 is Meson-based. Reusing the old CMake source list would silently omit
  new/renamed upstream translation units and is not an acceptable port.
- The source audit script pins upstream and checks the reviewed D3D9 and
  presenter anchors. Android-specific source adaptation remains a separate
  patch series so upstream files can be compared without losing provenance.
- The first buildable milestone is **2.6.2 D3D9 + SDL2 WSI on Android with
  SnapFrame disabled**, using the existing NativeSurface lifetime contract.
  Only after that passes should the SnapFrame capture hook and separate
  generated present be introduced.

### Acceptance tests to add with the port

1. Register native window, create Vulkan surface, unregister only after
   presenter/swapchain teardown; verify no stale surface use.
2. Replace/resize the native window while the game is paused; verify the old
   swapchain is retired before the old window reference is released.
3. Run baseline game presentation with SnapFrame disabled; compare surface
   creation, source FPS and stability against native-1.9.2b.
4. Add source capture only and confirm the source image is captured before HUD.
5. Add generated presentation and verify source/generated result accounting
   independently, with no synchronous acquire or queue wait added to the
   generated path.
6. Enable Snap AI pressure feedback only after the preceding stages pass.

No Android 2.6.2 source files or binary are claimed as ported by this audit.


### Native-window reference ownership hardening (2026-11-06)

The current Android WSI registry now owns an `ANativeWindow` reference for its active registration. `nativeSurface()` acquires a temporary reference while holding the registry mutex and the caller releases that lease immediately after `vkCreateAndroidSurfaceKHR` returns. Replacing/unregistering a registration acquires the new window before publishing it and releases the old registry reference after dropping the mutex, so native-window callbacks are not invoked under the registry lock.

This specifically closes the lookup-versus-replacement race during Vulkan surface creation. It does **not** by itself prove that an already-created `VkSurfaceKHR` no longer uses the old native window. The Android caller must still retire the presenter/swapchain and destroy the Vulkan surface before unregistering/replacing the old registration. A follow-up 2.6.2 WSI port must preserve that ordering and add an explicit surface-lifetime test; no claim is made here that the full surface lifetime is reference-counted by this registry.


## Upstream 2.6.2 architecture delta (reviewed against pinned commit)

The pinned 2.6.2 source confirms that the current Android adapter files are
not drop-in replacements for upstream:

- Upstream has no `src/wsi/sdl2/wsi_presenter_sdl2.cpp`; SDL2 WSI surface
  creation lives in `src/wsi/sdl2/wsi_platform_sdl2.cpp`, through the
  `Sdl2WsiDriver::createSurface` implementation.
- Upstream `WsiDriver::getInstanceExtensions()` is the instance-extension
  provider. The SDL2 implementation currently returns SDL's extension list;
  Android must add `VK_KHR_android_surface` only for the native-window path
  and only when the loader/device supports it.
- Upstream D3D9 owns a map of per-window `D3D9WindowContext` presenters.
  The presenter accepts a `PresenterSurfaceProc` callback; it is not the
  older single `m_presenter` + `wsi::createSurface` implementation in this
  Android fork. Native-window lifetime must therefore be tied to the
  presenter/window-context generation, not just to the duration of the
  surface-create callback.
- The upstream D3D9 target includes additional translation units absent from
  the 1.9.2b CMake target, including `d3d9_constant_buffer.cpp`,
  `d3d9_shader_validator.cpp`, `d3d9_annotation.cpp`, `d3d9_mem.cpp`,
  `d3d9_window.cpp`, `d3d9_interop.cpp`, `d3d9_on_12.cpp`, and
  `d3d9_bridge.cpp`. The DXVK core also has new/renamed units such as
  `dxvk_allocator.cpp`, `dxvk_constant_state.cpp`,
  `dxvk_fence.cpp`, `dxvk_implicit_resolve.cpp`,
  `dxvk_latency_builtin.cpp`, `dxvk_latency_reflex.cpp`,
  `dxvk_platform_exts.cpp`, and `dxvk_sparse.cpp`.
- Upstream `src/wsi/meson.build` compiles SDL2, SDL3 and GLFW backends
  conditionally. Android's curated CMake target should include only the SDL2
  WSI sources and common WSI sources, not desktop GLFW/SDL3/Win32 files.

### Required 2.6.2 port sequence

1. Build a pinned-source-derived CMake source manifest from the upstream
   Meson arrays. Every listed source must exist at the pinned commit; the
   manifest must fail if an upstream source is missing or a new source is
   silently omitted.
2. Add Android native-window handling to upstream
   `Sdl2WsiDriver::getInstanceExtensions()` and
   `Sdl2WsiDriver::createSurface()`. Keep SDL's regular path unchanged.
3. Extend the native-window registration from a temporary surface-create
   lease to a presenter-generation lease. Retire the presenter (which destroys
   its swapchain and surface) before releasing that generation's
   `ANativeWindow` reference. Handle replacement and resize by creating a
   new generation and retiring the old one only after presenter teardown.
4. Build and package the upstream D3D9 library with SnapFrame disabled.
   Validate AArch64 ELF, SONAME, D3D9 exports and matching runtime dependency.
5. Run a device baseline test for surface creation, resize/replacement,
   source presentation and stability. Only then port source capture and the
   generated-present path to the new `D3D9WindowContext` / `Presenter`
   lifecycle. Preserve source/generated accounting and avoid introducing
   synchronous GPU readback or queue waits.

**Current implementation boundary:** this review identifies the actual 2.6.2
integration points and source-list delta. It does not yet add upstream 2.6.2
translation units to the Android build, and does not claim a 2.6.2 binary.


### Presenter-generation ownership refinement

Inspection of the pinned upstream `src/d3d9/d3d9_swapchain.cpp` shows
`D3D9SwapChainEx::CreatePresenter(HWND, Signal)` constructs a
`PresenterSurfaceProc` lambda that captures the device and HWND. The
`Presenter` stores that callback for its full lifetime and its destructor
destroys the swapchain and surface before member destruction. This gives the
2.6.2 port a concrete ownership boundary:

- The Android integration must bind a native-window lease to the presenter
  callback/window-context generation, not to the temporary
  `vkCreateAndroidSurfaceKHR` call.
- The lease must be captured by value in the callback (or an equivalent
  presenter-owned RAII object). When a window is replaced, the old callback
  retains the old native window until its Presenter has destroyed its
  swapchain and VkSurfaceKHR.
- The callback must create the surface from its captured window generation;
  looking up the mutable global registry again at callback time can silently
  bind an old presenter to a newly registered window.
- `Sdl2WsiDriver::getInstanceExtensions()` is the upstream instance-extension
  seam. Add `VK_KHR_android_surface` there for the Android native-window
  path, while preserving SDL2's existing extension list and checking the
  extension is available before instance creation.

This replaces the earlier broad assumption that the current registry's
active-registration reference alone could cover surface lifetime. The
presenter-owned lease is the required 2.6.2 lifetime mechanism.
