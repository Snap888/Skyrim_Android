# DXVK Native 2.6.2 Android port map

## Pinned upstream source

- Repository: `doitsujin/dxvk`
- Release tag: `v2.6.2`
- Commit: `9d6f54a1ade20d1d27dd421024717a636f3d8c68`
- Build target: Android ARM64, DXVK Native, SDL2 WSI, D3D9 only.

## Verified build milestone — 2026-11-06

GitHub Actions run [42231295489](https://github.com/Snap888/Skyrim_Android/actions/runs/42231295489) completed successfully. The dedicated **DXVK Native 2.6.2 Android ARM64 candidate build** job compiled the pinned upstream source with the Android NDK, applied the Android Presenter/WSI overlay, verified the ELF ABI and uploaded artifact `dxvk-native-2.6.2-android-arm64-candidate`.

The artifact was downloaded and independently inspected after CI:

- File: `libdxvk_d3d9_262.so`
- ELF: 64-bit, little-endian, shared object, AArch64, Android API 28.
- Toolchain: Android NDK r29 (14206865).
- SONAME: `libdxvk_d3d9_262.so`.
- Required exports: `Direct3DCreate9` and `Direct3DCreate9Ex` are present.
- Dynamic dependencies: `libvulkan.so`, `libandroid.so`, `liblog.so`, `libm.so`, `libdl.so`, `libc.so`.
- No direct SDL2 dependency is recorded in `DT_NEEDED`; the patched upstream SDL2 WSI dynamically loads Android's `libSDL2.so`.

Artifact: [download from GitHub Actions run 42231295489](https://github.com/Snap888/Skyrim_Android/actions/runs/42231295489).

This confirms a real Android ARM64 DXVK D3D9 library build. It does **not** yet confirm a complete Skyrim runtime pair, gameplay compatibility, or SnapFrame FG integration in this upstream version.

## Architecture constraints before runtime selection

The existing app loads a DXVK library and a matching game runtime through `DxvkRuntimeSelector`. The selector deliberately requires both `libdxvk_d3d9_262.so` and `libmain_262.so` before exposing 2.6.2. That safety gate remains unchanged.

The current game runtime target `main` links against the legacy `dxvk_d3d9` target. The current `snapframe_host` also links against the legacy DXVK headers and library. Merely renaming `main` to `main_262` or adding the new D3D9 library to the load list would be unsafe: it could leave both DXVK implementations loaded and bind SnapFrame's old DXVK-facing code to the wrong implementation.

The upstream 2.6.2 D3D9 present path is structurally different from the current Android baseline. Its per-window `D3D9WindowContext` and `Presenter` lifecycle differs from the older `m_presenter` / `m_context` path. SnapFrame source capture, generated-image presentation, independent result accounting, present-mode changes and Snap AI pressure telemetry must be deliberately ported to the new command-submission model. DXVK remains the translation/present backend; interpolation stays in SnapFrame.

## Next implementation gates

1. Add an explicit, isolated 2.6.2 runtime-pair target. Do not reuse the old DXVK source list.
2. Build a matching `libmain_262.so` only after the SnapFrame host has a compatible 2.6.2 adapter boundary; generated Skyrim C sources are not present in the repository checkout, so CI cannot produce the real game runtime from this checkout alone.
3. Port SnapFrame source capture and generated presentation to upstream 2.6.2's Presenter/command-list lifecycle. Preserve no-readback operation, queue/semaphore ownership, and independent source/generated results.
4. Add pair verification: AArch64 ELF for both objects, expected DXVK SONAME, D3D9 exports, and exact `DT_NEEDED` edge from `libmain_262.so` to `libdxvk_d3d9_262.so`.
5. Run device tests on Adreno 740/Turnip, first with FG disabled, then enabled. Compare source FPS, source submission wait, CPU Present P95, GPU stage timing, acquire failures, and long-run stability. A queued/accepted present is not proof of physical scan-out.
6. Only after all gates pass, expose `native-2.6.2` in the launcher selector. Keep native-1.9.2b as the default/fallback.

## Android WSI and native-window lifetime

The patcher `tools/dxvk/apply_native_262_android_port.py` applies the Android overlay to the exact pinned upstream checkout:

- Adds the project-owned `NativeSurfaceLease` implementation.
- Captures a lease by value in the upstream Presenter surface callback so a presenter remains associated with the exact native-window generation.
- Creates Android surfaces through `vkCreateAndroidSurfaceKHR` and adds `VK_KHR_android_surface` to the SDL2 WSI instance extension list when needed.
- Preserves the normal SDL surface path when no Android native window is registered.

The callback-held lease must outlive the presenter's swapchain and Vulkan surface. Do not look up a mutable global native-window registry again from an old presenter callback. Resize/replacement must retire the old presenter and surface before releasing its window generation.

## Reproducible build implementation

- `tools/dxvk/checkout_native_262_source.sh` fetches and verifies the exact upstream revision.
- `tools/dxvk/generate_native_262_android_manifest.py` derives the Android source inventory from upstream Meson arrays and checks the WSI/source groups.
- `android/app/jni/dxvk_native_262/CMakeLists.txt` builds the isolated D3D9 candidate from that manifest and generates upstream shaders.
- `tools/dxvk/build_native_262_android_candidate.sh` configures the Android NDK harness, builds SDL2 and DXVK, and checks architecture, SONAME and exports.
- The build initializes the pinned Vulkan-Headers, SPIRV-Headers and DirectX header submodules before compilation.
- libdisplay-info 0.1.1 is built from pinned commit `92b031749c0fe84ef5cdf895067b84a829920e25`; it is linked as PIC static code. If host `hwdata/pnp.ids` is absent, EDID parsing remains but manufacturer-name lookup is unavailable.
- Android's SDL2 shared target is named `libSDL2.so`; the overlay uses that name on Android while preserving desktop SDL2 names elsewhere.
- Root CMake option `SNAP_BUILD_DXVK_NATIVE_262` remains OFF by default. The successful CI candidate job is an isolated build and does not replace or alter the production 1.9.2b runtime.

## Present timing attribution

The current baseline instrumentation separates nested source/generated submission waits from the enclosing handoff interval. Handoff residuals are computed after subtracting those nested waits, avoiding double-counting. These timings are attribution telemetry, not a claim about physical scan-out.

## Status

- [x] Pinned upstream DXVK Native 2.6.2 source.
- [x] Android Presenter/WSI overlay applied in CI.
- [x] Android ARM64 D3D9 candidate compiled successfully.
- [x] Candidate artifact independently inspected for AArch64 ELF, SONAME, D3D9 exports and dynamic dependencies.
- [ ] Matching `libmain_262.so` built and linked against the candidate.
- [ ] SnapFrame capture/generated-present hooks ported to upstream 2.6.2.
- [ ] Device surface, gameplay and FG tests completed.
- [ ] Runtime selector exposes 2.6.2 after pair/device gates.

**Current usable runtime remains native-1.9.2b.** The 2.6.2 D3D9 library is a verified Android ARM64 build artifact, but it is not yet a complete selectable Skyrim runtime. The APK workflow was skipped in this CI run; no APK build is claimed.
