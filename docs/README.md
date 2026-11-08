# Skyrim Android — project documentation

This index describes the current SnapFrame-based development line. Historical backend-specific documentation is intentionally not part of the active project documentation.

## Core development

- [SnapFrame integration](SNAPFRAME_INTEGRATION.md) — DXVK callbacks, frame capture, generation, WSI acceptance and backend lifecycle.
- [Snap AI integration](SNAP_AI_INTEGRATION.md) — portable AI core and adaptive-control integration.
- [Snap AI EngineFrame status](SNAP_AI_ENGINEFRAME_STATUS.md) — current implementation, device logs, known limitations and next steps.
- [Android settings](ANDROID_SETTINGS.md) — runtime controls and persistence.
- [DXVK performance A/B testing](DXVK_PERFORMANCE_AB_TEST.md) — only when the test is configured for SnapFrame ON/OFF and does not rely on a removed backend.
- [GPU telemetry](GPU_TELEMETRY.md) — stage timing and device-log interpretation.
- [Testing](TESTING.md) — repeatable device validation and regression checks.

## Active branch

`Snap-AI-EngineFrame` is the active Skyrim integration branch. Keep `main` and `frame_gen` unchanged. SnapFrame is the only frame-generation backend in this branch.

- [Android mouse control](ANDROID_MOUSE_CONTROL.md) — in-game touchpad toggle, pointer ownership and simultaneous virtual gamepad input.
