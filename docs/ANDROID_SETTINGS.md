# Android settings integration

## Current status

The in-game overlay menu now exposes a separate **Snap AI / Frame Generation** section. The currently supported controls are Frame Generation on/off and Flow Scale (25–100%). These call the legacy DXVK host's native API, queue changes for the next serialized frame callback, and persist them to `port/snapframe.conf`. Snap AI adaptive control is shown as unavailable until scalar telemetry and the new DXVK adapter are connected.

## Existing behavior to preserve

- The Snap AI / Frame Generation panel is opened from the in-game overlay button.
- Its current JNI methods are implemented in `runtime/main.c`.
- Existing touch/gamepad controls and editor settings must remain unchanged.
- The separate SnapFrame bootstrap currently reads `port/snapframe.conf` and writes dedicated SnapFrame logs.

## UI rules for the new section

Only expose settings that the active backend can actually apply. The legacy host currently exposes live enable/disable and flow scale (25–100%). The portable SnapFrame API v1 additionally defines Performance/Balanced/Quality, motion-analysis/profiling/diagnostic flags and multiplier 1/2, but these are not yet wired to the active legacy host and must not be presented as active controls. Additional search-radius, refinement-pass, HUD handling and artifact-reduction controls remain hidden until the adapter implements them.

Snap AI API v1 supports enable/disable and Performance/Balanced/Quality profile selection. Do not expose Custom or independent AI switches that have no native effect.

## Persistence and application

The current Frame Generation and Flow Scale controls persist validated values in `port/snapframe.conf`. Native state is authoritative. JNI validates flow scale and returns failure if the host is not initialized. GPU-affecting changes are queued to the render callback; the UI thread never accesses Vulkan objects. The toast confirms that settings were queued, not that GPU resource recreation has already completed.

A setting requiring resource recreation must show a pending/restart-safe status until the backend confirms application. Preserve settings across app/game restarts without changing the existing SnapFrame configuration.
