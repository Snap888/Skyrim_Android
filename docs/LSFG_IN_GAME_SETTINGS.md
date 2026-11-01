# In-game LSFG settings

Branch: `snap-patch-3`

The in-game top-left `☰` menu opens the LSFG settings panel without leaving
Skyrim. LSFG is a submenu item in this menu; it is no longer presented as a
standalone top-left button.

## Runtime controls

- **LSFG ON/OFF slider:** OFF selects multiplier `1x` and bypasses generated
  frames. The native host remains initialized so the user can switch LSFG back
  ON during the same game session. ON restores the selected 2x, 3x or 4x
  multiplier.
- **Frame generation:** 2x, 3x or 4x (one, two or three generated frames).
  The multiplier selector is disabled while the OFF position is selected.
- **Flow quality:** 40–100 percent.
- **GPU mode:** Performance, Balanced or Low GPU load. The mode scales the
  requested flow resolution by 0.75, 1.0 or 0.5 respectively.
- **Apply:** publishes the selected values to the native LSFG host and saves
  them to `port/lsfg.conf` for the next launch.

## Disable semantics

OFF is a runtime bypass, not an unload/reinitialization operation. The native
bootstrap, shader resources and DXVK hooks remain initialized; the active
present hook reports LSFG inactive and no generated frames are requested while
the saved multiplier is `1`. This allows immediate ON/OFF switching without
restarting Skyrim.

The persisted config keeps `enabled=1` to indicate that LSFG is configured
and available, while `multiplier=1` represents the OFF state. On startup the
host still initializes the LSFG resources and hooks, but starts with runtime
generation disabled. Do not change this to `enabled=0`: that would skip host
initialization and make the in-game controls unable to reactivate LSFG.

The UI thread publishes settings through atomics. The DXVK render thread
latches a consistent settings snapshot at the start of each source frame.
Flow-resolution-dependent resources are handled by the existing DXVK
command-list path; no independent Vulkan queue submission is introduced.

## Limitations and verification

- The controls require a valid LSFG configuration and the user's own
  `Lossless.dll` resource source at startup. If bootstrap cannot initialize,
  runtime controls cannot activate LSFG.
- OFF prevents frame generation; it does not unload shader resources or remove
  the hooks from memory.
- Android Java compilation, native arm64 compilation and on-device ON/OFF
  behaviour must be verified with an APK built from the current branch HEAD.
- A successful Java-only CI job is not evidence that the native C++/DXVK/LSFG
  sources compile. Likewise, source-level bypass logic is not device runtime
  validation.
