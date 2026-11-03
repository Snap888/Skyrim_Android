# Snap AI EngineFrame testing

## Status

The portable Snap AI/SnapFrame shared libraries, concrete DXVK adapter and JNI main bridge have passed Android ARM64 compile CI (run #41738044737). Both upstream portable unit tests (`snap_ai_tests`, `snap_frame_tests`) also pass in CI (run #41738584797). Java compilation has passed. The Snap AI bootstrap API-version probe also compiled successfully in run #41738386096. The latest scalar timing telemetry changes are in a newer native compile run. No full APK or physical-device GPU validation has been completed. The full APK job is skipped unless explicitly dispatched with generated `gen/` sources.

## Portable API tests

Run upstream Snap AI and SnapFrame C API tests in the Snap_AI_Engine repository or a host build. These cover portable lifecycle and policy behavior only; they do not exercise DXVK or Vulkan.

## Required native tests

- Snap AI create/destroy, disabled state, profile changes, invalid metrics, scene cut, non-monotonic frame ids and bounded output.
- SnapFrame create/destroy, disabled/bypass, frame ordering, backend failure, update_config, reset_history, resize and device recreation.
- DXVK adapter fallback when portable engine creation fails; backend-error bypass; accepted-present counting only after DXVK reports success.
- Verify SnapFrame remains functional when Snap AI is absent, disabled or returns an error.
- Verify Frame Generation OFF prevents backend dispatch.
- Verify original-frame present fallback on every backend failure.
- Verify GPU resources are released before DXVK device teardown and rebuilt after device recreation.

## Required Android menu scenarios

The Frame Generation and Flow Scale controls are now present in the in-game menu and route through SnapFrameBackendV1; Snap AI is not yet live.

1. Toggle Frame Generation during gameplay.
2. Change Flow Scale across low/mid/high values and verify history reset/reallocation.
3. Restart the game and verify persisted settings.
4. Verify the UI reports unavailable Snap AI control until the adapter is complete.
5. Toggle Snap AI during gameplay (pending implementation).
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
