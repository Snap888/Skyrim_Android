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

## Runtime selection and fallback

The Android selector exposes a DXVK variant only when both corresponding ARM64 ELF libraries are present. DXVK Native 1.9.2b remains the default and safe fallback. A standalone 2.6.2 DSO must never cause the 2.6.2 selector entry to appear.

## SnapFrame status

The 2.6.2 DXVK source hook currently captures the source path and records luma/flow/consistency work. Generated-frame WSI presentation remains disabled until a DXVK-owned asynchronous transaction scheduler and the matching runtime bridge are implemented and tested. Successful compilation of the candidate DSO is not evidence that generated frames are displayed.
