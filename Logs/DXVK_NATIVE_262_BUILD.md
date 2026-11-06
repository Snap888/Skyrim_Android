# DXVK Native 2.6.2 Android runtime-pair build

## Inputs pinned for this build

- Skyrim Android branch: `Snap-AI-EngineFrame`
- DXVK Native source: `doitsujin/dxvk`
- DXVK commit: `9d6f54a1ade20d1d27dd421024717a636f3d8c68`
- Generated Skyrim source archive: `https://github.com/Snap888/fexdroid-1/releases/download/gen/gen.tar.xz`
- Archive SHA-256: `5c03d4d21737ff50514433f775a3e4b0a6f2c68b282bc29a4fbcbb344683623c`
- Android ABI: `arm64-v8a`, minimum API 28.

## Required runtime pair

The APK build opts into both matching targets:

- `libdxvk_d3d9_262.so`
- `libmain_262.so`

The build must verify AArch64 ELF headers, the DXVK SONAME and D3D9 exports, and the exact `DT_NEEDED` dependency from `libmain_262.so` to `libdxvk_d3d9_262.so`. It then extracts both libraries from the APK and repeats the pair check against the packaged files.

The existing DXVK Native 1.9.2b pair remains the default fallback. The 2.6.2 option is exposed by the Android selector only when both matching libraries are present.

## Scope and limitations

This build validates compilation and APK packaging. It does not claim that SnapFrame generated-frame presentation is enabled or visually validated on-device; the current 2.6.2 host still records the source path and keeps generated present disabled until the WSI present transaction is implemented and tested.
