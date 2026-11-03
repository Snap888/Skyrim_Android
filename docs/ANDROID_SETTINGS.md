# Android settings integration

## Current status

The existing in-game overlay menu exposes LSFG settings and controls. A dedicated Snap AI / Frame Generation settings section has not yet been added. The portable libraries are linked, but their runtime settings are not yet connected to JNI or the Android UI.

## Existing behavior to preserve

- The LSFG menu is already opened from the in-game overlay button.
- Its current JNI methods are implemented in `runtime/main.c`.
- Existing touch/gamepad controls and editor settings must remain unchanged.
- The separate SnapFrame bootstrap currently reads `port/snapframe.conf` and writes dedicated SnapFrame logs.

## UI rules for the new section

Only expose settings that the active backend can actually apply. API v1 currently supports SnapFrame Performance/Balanced/Quality, flow scale 25..100%, motion-analysis/profiling/diagnostic flags, and multiplier 1/2. Additional search-radius, refinement-pass, HUD handling and artifact-reduction controls must remain hidden until the adapter implements them.

Snap AI API v1 supports enable/disable and Performance/Balanced/Quality profile selection. Do not expose Custom or independent AI switches that have no native effect.

## Persistence and application

Persist validated settings in the per-game configuration under `port/`. Native state is authoritative. JNI calls must validate inputs, return an explicit failure when a setting cannot be applied, and enqueue GPU-affecting changes for the render thread rather than mutating resources from the UI thread.

A setting requiring resource recreation must show a pending/restart-safe status until the backend confirms application. Preserve settings across app/game restarts without changing existing LSFG configuration.
