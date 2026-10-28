# LSFG implementation status (`snap-patch-2`)

## Goal

In-process **Lossless Scaling Frame Generation** on the **Android Vulkan present path**
(after DXVK), using the user's own `Lossless.dll` as a **shader data source only**.

Reference design: WinNative `docs/lsfg-frame-generation.md` / Eden LSFG (not MediaProjection).

## Done in this branch (scaffold)

| Item | Location |
|------|----------|
| Launcher toggles + write `port/lsfg.conf` | `LauncherActivity.java` |
| Host config load + init/shutdown stubs | `android/app/jni/lsfg/lsfg_host.*` |
| Link `lsfg_host` into `libmain` | `jni/lsfg/CMakeLists.txt`, `jni/game/CMakeLists.txt` |
| Call `lsfg_host_init` after `platform_init` | `runtime/main.c` |
| Feasibility notes | `docs/LSFG_VK_INTEGRATION.md` |

## Not done yet (next commits)

1. **Extract** RCDATA from `Lossless.dll` → DXBC → SPIR-V cache under `port/lsfg_spirv/`
2. **Probe** device for required Vulkan features (`VK_EXT_robustness2`, etc.).
3. **Present hook** in dxvk-native: intermediate presents then real frame.
4. **Pacing** with existing FPS cap / vsync options.

## User setup (once FG is complete)

1. Copy `Lossless.dll` to `<Skyrim folder>/port/Lossless.dll`
2. Launcher → enable LSFG, multiplier 2×
3. logcat tag `skyrim-lsfg`

Until extract + present hook land, enabling LSFG only logs that the DLL was found.

## License

Do **not** commit or ship `Lossless.dll` or extracted SPIR-V in the repo/APK.
