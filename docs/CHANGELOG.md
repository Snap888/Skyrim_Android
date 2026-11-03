# Changelog

## 2026-11-04 — Snap AI EngineFrame branch

- Created `Snap-AI-EngineFrame` from `frame_gen`; verified the initial commit matched.
- Added a backup branch before the next integration phase.
- Pinned Snap AI Engine / SnapFrame portable core in Android CMake.
- Linked `SnapAI::Engine` and `SnapFrame::Engine` into the Android game runtime.
- Enabled Android CI push/PR filtering for the working branch.
- Added a successful Android NDK ARM64 build job for both portable shared libraries.
- Added an in-game Snap AI / Frame Generation section with live Frame Generation and Flow Scale controls, native validation and config persistence.
- Added a render-callback-safe flow-scale update path that resets temporal history before resource recreation.
- Added integration status and architecture documentation.
- Preserved the existing DXVK-coupled SnapFrame GPU host as the active implementation pending adapter parity.

Added after the initial audit: JNI and in-game Frame Generation/Flow Scale controls for the existing DXVK host, with config persistence and render-callback-safe history reset.

Not yet completed: concrete DXVK `SnapFrameBackendV1` adapter, live AI telemetry/control loop, Snap AI UI controls, full APK build and real-device validation.

- Added a concrete `SnapFrameBackendV1` DXVK adapter that routes the portable SnapFrame core into the existing GPU pipeline without replacing its shaders or present hooks.
- Added a compile-only Android ARM64 CI target for the JNI settings bridge and real DXVK host.
- Explicitly load `snap_frame_engine` and `snap_ai_engine` shared libraries before the game runtime.
