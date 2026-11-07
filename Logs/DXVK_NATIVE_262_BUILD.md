# DXVK Native 2.6.2 Android candidate and runtime-pair gate

## Pinned inputs

- Skyrim Android branch: `Snap-AI-EngineFrame`
- DXVK Native source: `doitsujin/dxvk`
- DXVK commit: `9d6f54a1ade20d1d27dd421024717a636f3d8c68`
- Generated Skyrim source archive: `https://github.com/Snap888/fexdroid-1/releases/download/gen/gen.tar.xz`
- Archive SHA-256: `5c03d4d21737ff50514433f775a3e4b0a6f2c68b282bc29a4fbcbb344683623c`
- Android ABI: `arm64-v8a`, minimum API 28.

## Current build boundary

CI builds and validates the standalone candidate `libdxvk_d3d9_262.so`. The APK may carry this candidate for inspection, but it is **not selectable as a runtime** yet.

The matching `libmain_262.so` is gated by `SNAP_BUILD_DXVK_NATIVE_262_RUNTIME_PAIR=OFF`. Run 42301795388 exposed why this gate is necessary: `main_262` was linked against neither a DXVK-2.6.2-specific Android surface bridge nor a matching SnapFrame host API. It failed with unresolved `dxvk_native_surface` and `snapframe_host_*` symbols. Linking the legacy `snapframe_host` is not an acceptable workaround because that target depends on DXVK Native 1.9.2b.

Do not turn the gate on until a dedicated 2.6.2 runtime bridge is implemented and the final APK contains both:

- `libdxvk_d3d9_262.so`
- `libmain_262.so`

The CI package check verifies the candidate's AArch64 ELF class and SONAME and explicitly rejects an accidentally packaged `libmain_262.so` while the gate is off.

## Verified candidate artifact (2026-11-07)

- DXVK candidate workflow: [run 42345480727](https://github.com/Snap888/Skyrim_Android/actions/runs/42345480727) — success.
- Android CI: [run 42345480750](https://github.com/Snap888/Skyrim_Android/actions/runs/42345480750) — success for all executed jobs. The full ARM64 APK job was **skipped**, so this run does not provide a new installable APK.
- Artifact: `dxvk-native-2.6.2-android-arm64.zip` (artifact ID `13563447448`).
- Archive SHA-256: `72ecf9c700a1cd11108b23445981888f022a160e745bee4dc2117c9d3c3d2e11`.
- Contents: only `libdxvk_d3d9_262.so` (35,520,976 bytes); no `libmain_262.so`.
- ELF: 64-bit AArch64, Android API 28, NDK r27c; SONAME `libdxvk_d3d9_262.so`.
- DSO SHA-256: `fb622f8deb7d14d1fad445d8873fbc9e6c4a2ba9bdbbe307f6949d52f7c0568f`.
- Dynamic exports verified with `nm -D --defined-only`: `dxvk_native_surface`, `snapframe_dxvk262_record_source`, `snapframe_dxvk262_release_window`, and `snapframe_dxvk262_release_device`.

This artifact is a verified standalone candidate, not a runtime pair. It must not be exposed in the runtime selector. The generated-frame presentation hook remains disabled.

## Dedicated main_262 bridge inventory

The standalone candidate is now linkable at the DXVK D3D9 export boundary, but the game-side runtime bridge is still absent. The Android runtime currently calls these SnapFrame host entry points from `runtime/main.c` and `runtime/snapframe_bootstrap.c`:

- `snapframe_config_defaults`
- `snapframe_host_set_log_path`, `snapframe_host_set_gpu_log_path`, `snapframe_host_set_flow_log_path`, `snapframe_host_set_flow_vectors_log_path`, `snapframe_host_set_event_log_path`
- `snapframe_host_log`, `snapframe_host_init`
- `snapframe_host_get_settings`, `snapframe_host_get_present_mode`
- `snapframe_host_update_settings`, `snapframe_host_update_present_mode`

The current 2.6.2 DSO exports the four DXVK/SnapFrame hook symbols listed above, but it does not yet implement this runtime host API. The legacy `snapframe_host.cpp` cannot be reused because its C++ implementation depends on DXVK 1.9.2b internals. A dedicated bridge must define the runtime API against an explicit 2.6.2 C ABI, and settings must actually reach the 2.6.2 capture/compute host; returning success from placeholder functions is not acceptable.

Before enabling the pair, verify that disabled-by-default configuration prevents compute dispatch, enabled state is applied safely at a frame boundary, unsupported flow-scale/present-mode settings are reported honestly, and telemetry distinguishes recorded compute from generated WSI presents. The existing generated-present gate remains closed.

## Runtime selection and fallback

The Android selector exposes a DXVK variant only when both corresponding ARM64 ELF libraries are present. DXVK Native 1.9.2b remains the default and safe fallback. A standalone 2.6.2 DSO must never cause the 2.6.2 selector entry to appear.

## SnapFrame status

The 2.6.2 DXVK source hook currently captures the source path and records luma/flow/consistency work. Generated-frame WSI presentation remains disabled until a DXVK-owned asynchronous transaction scheduler and the matching runtime bridge are implemented and tested. Successful compilation of the candidate DSO is not evidence that generated frames are displayed.


## Device launch crash analysis (2026-11-07)

The updated device log in `Logs/log.txt` records a launch failure after Skyrim calls
`Direct3DCreate9(32)` and loads `DXVK: v2.6.2+`:

- `libc++abi: terminating due to uncaught exception of type dxvk::DxvkError`
- signal 6 (`SIGABRT`)
- stack enters `libdxvk_d3d9_262.so` and returns through `Direct3DCreate9` in `libmain_262.so`
- no D3D9 device, swapchain, or SnapFrame frame-capture initialization is reached.

This proves the current APK's 2.6.2 runtime pair is packaged and loaded, but it does
**not** prove runtime initialization works. The failure is in DXVK startup, before
SnapFrame's frame-generation path. Missing voice `.fuz` files are unrelated.

A startup exception barrier is now applied to the pinned upstream
`src/d3d9/d3d9_main.cpp`: it catches `DxvkError`, `std::exception`, and unknown
exceptions at the exported D3D9 creation boundary, logs the original error to
Android logcat under tag `DXVK262`, and returns `D3DERR_NOTAVAILABLE` instead of
allowing a C++ exception to escape the exported ABI and abort the process.

This is a containment/diagnostic change, **not yet the underlying Vulkan startup
fix**. The next device run must capture the `DXVK262` logcat message. It will
identify whether DXVK 2.6.2 rejected a required Vulkan instance extension, failed
to create a Vulkan 1.3 instance, failed to enumerate an adapter, or encountered
another initialization error. Do not mark 2.6.2 stable or make it the default
until that cause is fixed and the game reaches the D3D9 device/swapchain stage.
