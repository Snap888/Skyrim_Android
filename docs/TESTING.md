# Snap AI EngineFrame testing

## Status

The branch has not yet completed an Android NDK build, a concrete DXVK adapter build, or physical-device GPU validation. The Android CI workflow's Java compilation job is currently running; its full APK job is skipped unless explicitly dispatched with generated `gen/` sources.

## Portable API tests

Run upstream Snap AI and SnapFrame C API tests in the Snap_AI_Engine repository or a host build. These cover portable lifecycle and policy behavior only; they do not exercise DXVK or Vulkan.

## Required native tests

- Snap AI create/destroy, disabled state, profile changes, invalid metrics, scene cut, non-monotonic frame ids and bounded output.
- SnapFrame create/destroy, disabled/bypass, frame ordering, backend failure, update_config, reset_history, resize and device recreation.
- Verify SnapFrame remains functional when Snap AI is absent, disabled or returns an error.
- Verify Frame Generation OFF prevents backend dispatch.
- Verify original-frame present fallback on every backend failure.
- Verify GPU resources are released before DXVK device teardown and rebuilt after device recreation.

## Required Android menu scenarios

1. Toggle Snap AI during gameplay.
2. Toggle Frame Generation during gameplay.
3. Change Performance/Balanced/Quality.
4. Change flow scale and supported motion settings.
5. Toggle GPU telemetry and diagnostics.
6. Resize/switch display resolution.
7. Return from overlay menu to gameplay.
8. Restart the game and verify persisted settings.

## GPU/device test matrix

Compare identical scenes and device conditions:
- Frame Generation OFF.
- Legacy SnapFrame ON, AI OFF.
- SnapFrame ON, AI ON.
- Performance, Balanced and Quality profiles.

Collect real FPS, generated FPS, stage GPU timings, total GPU time, dropped/generated counters and visual artifact notes. Use median and P90 across repeatable samples. Do not treat policy estimates or mock-backend output as measured GPU savings.

## Acceptance criteria

- Android ARM64 native build succeeds with the pinned NDK/CMake versions.
- No regression in audio, controls, LSFG behavior or game startup.
- Generated-frame output is accepted by the existing DXVK present path.
- AI recommendations demonstrably alter supported dispatch parameters and remain within backend limits.
- All fallback, device recreation and hot-setting tests pass.
- APK is produced by a successful full CI job and installed/tested on a real device.
