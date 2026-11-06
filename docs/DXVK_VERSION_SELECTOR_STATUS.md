# DXVK version selector — implementation status

Date: 2026-11-06  
Branch: `Snap-AI-EngineFrame`

## Current state

The Android runtime uses an in-tree DXVK-Native-derived D3D9 implementation labelled `native-1.9.2b`. It is not a drop-in binary for desktop DXVK releases.

The Android load order is:

1. `SkyrimActivity.getLibraries()` loads `SDL2`, `dxvk_d3d9`, SnapFrame/Snap AI shared libraries, then `main`.
2. `main` is the game runtime and has a direct ELF dependency on `libdxvk_d3d9.so`.
3. SDL's `getMainSharedObject()` resolves the main runtime path before `SDL_main` starts.

This provides a valid selection point before game startup, but changing only a config value or loading another DXVK library alongside the existing one is insufficient. The game runtime's linked symbols and ELF `DT_NEEDED` dependency must resolve to the selected implementation.

## Candidate matrix

| Variant | Source | Status | Decision |
|---|---|---|---|
| Android baseline | In-tree native-1.9.2b + Skyrim patches | Existing fallback | Keep unchanged |
| DXVK Native 2.6.2 | Official `doitsujin/dxvk` tag `v2.6.2`, commit `9d6f54a1ade20d1d27dd421024717a636f3d8c68`, built with `DXVK_NATIVE` + SDL2 WSI | Candidate; not yet ported or built for Android | First upgrade target |
| DXVK 3.1.1 | Current Snap888/dxvk master lineage | Not a native Android drop-in; higher compatibility risk | Do not expose yet |

Important distinction: DXVK Native is built from the official `doitsujin/dxvk` source tree with its native build configuration and a selected WSI backend; it is not a separate downloadable Android binary. The candidate must be built from source at the pinned tag with `DXVK_NATIVE` and `DXVK_WSI_SDL2`, not taken from the Windows/Wine release assets. The upstream README documents SDL2, SDL3, and GLFW native WSI backends. Its release notes describe 2.6.2 as a maintenance release without new Vulkan feature requirements and mention improved dxvk-native WSI backend selection. This makes it a more defensible first candidate than jumping directly to 3.1.1.

## Required architecture for a real selector

Each selectable implementation must be a complete, independently linkable runtime pair:

- `libdxvk_d3d9_<variant>.so` — version-specific D3D9/Vulkan implementation.
- `libmain_<variant>.so` — same Skyrim runtime, linked against that exact versioned DXVK SONAME.
- SnapFrame/Snap AI libraries remain common only where their ABI and integration contracts are unchanged.

The launcher stores a stable variant ID. `SkyrimActivity.getLibraries()` loads the selected versioned DXVK dependency and matching SnapFrame/Snap AI dependencies; `getMainSharedObject()` returns the matching versioned main library. Unknown or missing selections must fall back to baseline. Selection applies on the next game process launch; a loaded Vulkan/DXVK instance must never be hot-swapped.

## DXVK Native 2.6.2 source-level port map

The detailed comparison is in [`DXVK_NATIVE_262_ANDROID_PORT_MAP.md`](DXVK_NATIVE_262_ANDROID_PORT_MAP.md). It records the pinned upstream commit, the per-window WSI/presenter differences, the current SnapFrame two-submit present contract, and the required migration gates. The key safety decision is to retain the current baseline present path until the candidate can preserve source capture before HUD, separate generated-frame submission/results, Snap AI pressure feedback, Android-native surface lifetime, and serialized swapchain recreation.

## Porting and acceptance gates for 2.6.2

The 2.6.2 native D3D9 swapchain is not source-compatible with the current adapter. Upstream uses per-window presenter contexts, acquires a WSI image with `Presenter::acquireNextImage`, then records the blit/present work through `m_parent->EmitCs` and `DxvkDevice::presentImage`. The current port instead has a custom `PresentImage` flow with SnapFrame copy/generation callbacks, source/generated pressure reporting, present-mode hooks, and explicit synchronization diagnostics around two present synchronization points. The new asynchronous command-stream path changes where source capture, generated-frame submission, and completion reporting must be inserted. This is a real adapter port, not a CMake target rename. Preserve the current baseline until each hook has an equivalent implementation and tests.

1. Pin the exact upstream native tag/commit and build only D3D9 + SDL2 WSI for Android ARM64.
2. Port the Android integration currently carried by the in-tree version: Vulkan loader/adrenotools integration, native surface handling, JNI/main hook integration, and Android logging.
3. Port the SnapFrame capture/present adapter and Snap AI source/generated submission-pressure telemetry. Do not duplicate interpolation in DXVK.
4. Preserve present-mode handling, bounded WSI timing diagnostics, queue synchronization, and semaphore ownership rules.
5. Build both baseline and 2.6.2 runtime pairs in CI; verify ELF dependencies and exported D3D9 entry points.
6. Only then expose the selector in Launcher settings, defaulting to baseline. Record selected and actually loaded versions in `port/log.txt`.
7. Test on Adreno 740 / Turnip with SnapFrame OFF first, then ON. Compare original game FPS, source submission wait, CPU Present P95, GPU stage timings, and stability. A higher generated-submit count is not proof of better scanout or source FPS.

## Explicit non-goals

- No runtime unloading/reloading of DXVK while Skyrim is running.
- No fake selector that changes a label/config but still loads baseline.
- No renaming the same source build as a second version.
- No dropping SnapFrame/Snap AI hooks or weakening synchronization to make a port compile.
- No signing-key requirement for APK builds.

## CI build coverage update

The workflow concurrency policy no longer cancels an in-progress branch run when another commit is pushed. This prevents rapid follow-up documentation or code commits from terminating the expensive native compile before its DXVK result is available.\n\nThe Android CI native job builds `dxvk_d3d9`, a dedicated `dxvk_linkage_check` shared-library probe, the SnapFrame host, and the JNI compile-check target. It verifies that DXVK and the probe are AArch64 ELF objects, that DXVK has the expected SONAME and exports `Direct3DCreate9`/`Direct3DCreate9Ex`, and that the probe has an exact `DT_NEEDED` dependency on `libdxvk_d3d9.so`.

A direct `main` target build was attempted, but the CI checkout does not contain the generated Skyrim module tables. The placeholder source correctly failed to link because it cannot define `g_fn_table_*` and `g_fn_count_*`. The workflow no longer pretends that a dummy source can produce a valid Skyrim runtime. The linkage probe tests the D3D9 ABI and ELF dependency independently; a real `libmain.so` / `libmain_262.so` build and full APK verification still require the actual generated `gen/*.c` game sources.

## Selector framework committed

The launcher/runtime selection scaffold is now implemented:

- `DxvkRuntimeSelector` defines complete runtime pairs and checks that both shared objects are present in the APK's native library directory.
- The launcher displays only variants that are actually packaged. It stores the selected stable variant ID in the existing `launcher` preferences; the old index preference is migrated once for compatibility.
- `SkyrimActivity.getLibraries()` resolves the selected pair before SDL loads libraries, loads the matching DXVK library before the matching game runtime, and logs the selected pair.
- The current Android Native 1.9.2b pair remains the mandatory fallback.
- The 2.6.2 pair is reserved as `libdxvk_d3d9_262.so` + `libmain_262.so`; it is not shown until both are included in the APK.

This prevents a misleading version selector: the UI cannot select a version whose DXVK library or matching game runtime is absent. Selection applies only at process startup; there is no hot swap.

## Current readiness

**Not ready for multi-version device testing.** The selector framework's Java compilation passed in Android CI run `42178305096`. Only the baseline pair is currently packaged, so the launcher correctly exposes no alternative yet. DXVK-Native 2.6.2 still needs the Android ARM64 port, SnapFrame/Snap AI adapter migration, the matching `main_262` runtime build, and packaging. The expanded native CI gate that also builds `main` and checks its exact DXVK `DT_NEEDED` dependency has been added; its validation is pending in the current branch CI run.