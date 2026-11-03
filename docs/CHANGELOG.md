# Changelog

## 2026-11-04 — Snap AI EngineFrame branch

- Created `Snap-AI-EngineFrame` from `frame_gen`; verified the initial commit matched.
- Added a backup branch before the next integration phase.
- Pinned Snap AI Engine / SnapFrame portable core in Android CMake.
- Linked `SnapAI::Engine` and `SnapFrame::Engine` into the Android game runtime.
- Enabled Android CI push/PR filtering for the working branch.
- Added integration status and architecture documentation.
- Preserved the existing DXVK-coupled SnapFrame GPU host as the active implementation pending adapter parity.

Not yet completed: concrete DXVK `SnapFrameBackendV1` adapter, live AI telemetry/control loop, JNI settings, in-game UI, full APK build and real-device validation.
