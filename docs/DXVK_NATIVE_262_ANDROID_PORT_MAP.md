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

## Current status

- Baseline Android DXVK runtime remains the only packaged runtime.
- The launcher selector intentionally hides 2.6.2 until both candidate libraries exist.
- Upstream source has been inspected against the current Android baseline; the present path and WSI are not drop-in compatible.
- No 2.6.2 binary, Android port, or device test is claimed by this document.
- CI's native Android job must build both `dxvk_d3d9` and `main`, then verify the DXVK SONAME and the main runtime's exact `DT_NEEDED` dependency.
