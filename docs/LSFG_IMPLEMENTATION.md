# LSFG implementation status (`snap-patch-2`)

## Goal

In-process **Lossless Scaling Frame Generation** on the **Android Vulkan present path**
(after DXVK), using the user's own `Lossless.dll` as a **shader data source only**.

Reference design: WinNative `docs/lsfg-frame-generation.md` / Eden LSFG (not MediaProjection).

## Done in this branch (scaffold)

| Item | Location |
|------|----------|
| Host config load + init/shutdown stubs | `android/app/jni/lsfg/lsfg_host.*` |
| Link `lsfg_host` into `libmain` | `jni/lsfg/CMakeLists.txt`, `jni/game/CMakeLists.txt` |
| Bootstrap helper | `runtime/lsfg_bootstrap.c` |
| Feasibility notes | `docs/LSFG_VK_INTEGRATION.md` |

## Wire-up required in `runtime/platform_sdl.c`

```c
// end of platform_init(), after load_key_script():
{ extern void lsfg_bootstrap(void); lsfg_bootstrap(); }

// start of platform_present_hook():
{ extern void lsfg_bootstrap_frame(void); lsfg_bootstrap_frame(); }
```

`runtime/lsfg_bootstrap.c` is picked up by `file(GLOB RT_SRC ${PORT}/runtime/*.c)`.

## Launcher (add if missing)

On Play, write `port/lsfg.conf`:

```
enabled=1
multiplier=2
flow_scale=0.70
performance=1
dll_path=<game>/port/Lossless.dll
shader_cache=<game>/port/lsfg_spirv
```

Copy `Lossless.dll` to `<game>/port/Lossless.dll`. Do not commit the DLL.

## Not done yet

1. Extract RCDATA from DLL → SPIR-V cache
2. Vulkan feature probe
3. DXVK present hook (real interpolation)
4. FPS pacing with multiplier

Until then, logcat tag `skyrim-lsfg` only reports config / DLL found.
