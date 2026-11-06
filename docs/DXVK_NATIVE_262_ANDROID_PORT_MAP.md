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

## Present-path comparison and SnapFrame port boundary

The pinned upstream 2.6.2 D3D9 presentation path differs materially from the old Android fork:

- D3D9SwapChainEx owns a map of per-window D3D9WindowContext objects; the active one is m_wctx.
- Each D3D9WindowContext owns its Presenter, frame ID and frame-latency fence.
- PresentImage obtains the source from m_backBuffers[0], acquires a WSI image through m_wctx->presenter, and schedules the blit/present through m_parent->EmitCs.
- The command-list lambda captures the device, presenter, blitter, source/destination views, WSI synchronization, frame ID and latency tracker by value.
- The lambda performs beginExternalRendering, DxvkSwapchainBlitter::present, synchronizeWsi, flushCommandList and DxvkDevice::presentImage.

The existing Skyrim SnapFrame host is not version-neutral. snapframe_host.cpp directly uses DXVK private C++ types such as DxvkContext, DxvkDevice, DxvkImage, DxvkImageView and GPU query objects. Its CMake target includes private headers from the old fork and links dxvk_d3d9. The adapter's opaque C pointers do not make the underlying C++ object layouts ABI-compatible across DXVK revisions.

Do not link the existing snapframe_host to dxvk_d3d9_262, and do not build a renamed main_262 against it. The safe boundary is a separate version-specific backend:

1. Keep portable SnapFrame and Snap AI independent of DXVK private types.
2. Build snapframe_host_262 against the pinned 2.6.2 headers and sources; it owns 2.6.2 GPU resources, command recording and query/readback implementation.
3. Keep the C ABI narrow and versioned. Never pass a DXVK C++ object to a host compiled against another DXVK revision.
4. Capture the D3D9 backbuffer in upstream 2.6.2 PresentImage before the WSI blit, using only 2.6.2 context/device/view objects.
5. Schedule generated midpoint work in the same ordered command-list and WSI synchronization domain as source present. Track source and generated completion independently; accepted present is not physical scan-out.
6. Tie history to the matching per-window D3D9WindowContext and presenter lifetime. Reset history on resize, reset, format change, surface recreation and window replacement.
7. Only build the paired game runtime after the 2.6.2 host compiles and passes queue ownership, no-readback and lifetime tests.

DXVK remains the D3D9/Vulkan backend and WSI integration. Interpolation and AI decisions remain in SnapFrame and Snap AI.

## Runtime-pair safety

The launcher exposes an upstream candidate only when both libraries exist and both pass an ARM64 ELF-header check. The current game target main links the old dxvk_d3d9 and old snapframe_host; the 2.6.2 candidate is not linked into that target.

A matching libmain_262.so cannot be built from the current repository checkout alone because generated Skyrim gen/*.c runtime sources are absent. It must also wait for the 2.6.2-specific SnapFrame host. Before selection, verify both AArch64 ELF objects, the DXVK SONAME and D3D9 exports, the exact DT_NEEDED edge from main_262 to dxvk_d3d9_262, absence of a legacy DXVK dependency, and matching pinned headers.

Keep native-1.9.2b as the default and fallback. Do not expose 2.6.2 until the complete pair and device gates pass.

## Next implementation gates

1. Add an explicit, isolated 2.6.2 runtime-pair target. Do not reuse the old DXVK source list.
2. Build a matching `libmain_262.so` only after the SnapFrame host has a compatible 2.6.2 adapter boundary; generated Skyrim C sources are not present in the repository checkout, so CI cannot produce the real game runtime from this checkout alone.
3. Port SnapFrame source capture and generated presentation to upstream 2.6.2's Presenter/command-list lifecycle. Preserve no-readback operation, queue/semaphore ownership, and independent source/generated results.
4. Add pair verification: AArch64 ELF for both objects, expected DXVK SONAME, D3D9 exports, and exact `DT_NEEDED` edge from `libmain_262.so` to `libdxvk_d3d9_262.so`.
5. Run device tests on Adreno 740/Turnip, first with FG disabled, then enabled. Compare source FPS, source submission wait, CPU Present P95, GPU stage timing, acquire failures, and long-run stability. A queued/accepted present is not proof of physical scan-out.
6. Only after all gates pass, expose `native-2.6.2` in the launcher selector. Keep native-1.9.2b as the default/fallback.

## Versioned 2.6.2 source-capture seam

The patcher now copies `snapframe_dxvk262_api.h` into the pinned upstream `src/d3d9` tree and inserts an optional weak C-ABI call in `D3D9SwapChainEx::PresentImage`:

- The hook is invoked on the DXVK command-stream worker after color-space compatibility is prepared and before the source backbuffer is consumed by the WSI blitter.
- It receives the exact 2.6.2 `DxvkContext`, `DxvkDevice`, and source `DxvkImageView` as opaque pointers, plus a size/version-tagged frame descriptor (frame ID, extent, Vulkan format, and the captured `Presenter` identity as a per-window history token).
- Opaque DXVK pointers are valid only for a host compiled against the pinned 2.6.2 headers. They must never be passed to the legacy 1.9.2b host.
- The weak hook is optional. If the matching host is not linked, DXVK follows the normal source-present path without changing behavior.
- A future implementation must record work into the supplied context, without flushing/submitting it, waiting on fences, or reading pixels/flow back to CPU. This preserves ordering with the source blit.

This is a source-capture/compute-recording seam only. It does **not** yet generate or present a midpoint, does not establish completion/scan-out, and does not mean a `snapframe_host_262` implementation exists. The next stage is to implement the hook in a separate host compiled against the pinned upstream headers, then add per-window history/resource lifetime and generated-frame scheduling inside the same command stream. The runtime selector remains gated on a fully built matching pair.

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
