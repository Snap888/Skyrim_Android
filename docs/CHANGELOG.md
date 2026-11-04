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
- Added a compile-only Android ARM64 CI target for the JNI settings bridge and real DXVK host; adapter + JNI compile passed in CI run #41738044737.
- Explicitly load `snap_frame_engine` and `snap_ai_engine` shared libraries before the game runtime.
- Added asynchronous scalar total-GPU-time and frame-interval export for the upcoming AI telemetry path.
- Added host CI for portable API unit tests; both upstream test suites pass.
- Verified the Snap AI bootstrap API-version probe, JNI settings bridge and scalar timing telemetry in the Android ARM64 native compile job (run #41738939416).

## 2026-11-04 — device log audit and telemetry fix
- Audited the latest SnapFrame logs: 7,650 captured frames, zero skips, and 255 successful stage-timing samples.
- Identified that sparse flow diagnostics were incorrectly gated by the optional diagnostic suite rather than the motion-analysis setting.
- Corrected the flow-readback gate so `motion_analysis=1` can collect aggregate flow diagnostics even when the seven-phase diagnostic suite is off.
- Updated GPU telemetry and testing documentation with measured timing distributions and the required device retest.

## 2026-11-04 — configuration persistence and diagnostics
- Fixed double-escaped newline literals in the SnapFrame settings writer so config keys are written as separate lines and can be parsed after restart.
- Corrected host/bootstrap diagnostic strings that emitted literal backslash-n text.
- Added config persistence and sparse flow-readback regression checks to the Android test plan.


### Event telemetry I/O optimization — 2026-11-04
- Buffered the SnapFrame event CSV with a 64 KiB stdio buffer.
- Removed per-event `fflush` from the render path; critical failures/rejections and lifecycle/test-phase events flush immediately, other records flush every 64 writes.
- Rate-limited repetitive successful stage/present events to one sample per 120 frames and frame-complete progress to one per 300 frames; first-frame samples and all failures are retained.
- No GPU algorithm, generated-frame decision or audio/input code changed. Device validation is still required.
