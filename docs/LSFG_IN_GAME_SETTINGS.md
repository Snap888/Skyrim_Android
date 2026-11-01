# In-game LSFG settings

Branch: `snap-patch-3`

The Skyrim activity now adds a small **LSFG** button at the upper-left of the
game surface. It opens an Android settings dialog without leaving the game.

## Runtime controls

- **Frame generation:** 2x, 3x or 4x (maps to one, two or three generated frames).
- **Flow quality:** 40–100 percent.
- **GPU mode:** Performance, Balanced or Low GPU load. The mode scales the
  requested flow resolution by 0.75, 1.0 or 0.5 respectively.
- **Apply:** updates the native LSFG host immediately and writes the values to
  `port/lsfg.conf` for the next launch.

The UI thread only publishes atomic settings. The DXVK render thread latches a
consistent settings snapshot at the start of each source frame. Flow-resolution
dependent resources are recreated lazily by the existing DXVK command-list
path; no independent Vulkan queue submission is introduced.

## Limitations

- The controls are available only while the LSFG host is initialized. If it is
  inactive, the button explains that `port/lsfg.conf` and `Lossless.dll`
  must be configured before starting the game.
- This change does not add an enable-from-off switch: initialization and shader
  extraction still happen during game startup.
- Android UI compilation and on-device touch/layout behaviour still require an
  APK build and device test.
