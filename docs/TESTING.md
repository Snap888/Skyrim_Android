# Snap AI EngineFrame testing

## Status

The portable Snap AI/SnapFrame shared libraries, concrete DXVK adapter, JNI settings bridge and scalar timing telemetry have passed Android ARM64 compile CI in earlier runs. Portable upstream API tests have passed. A full APK build and physical-device GPU validation have not yet been completed. The latest CI run #41817208666 passed portable API tests, portable ARM64 libraries, DXVK host ARM64 compile and Java compilation; the full APK job was skipped.

The earlier 2026-11-04 baseline run reported 7,650 captured frames and 255 successful GPU timing samples. The newest uploaded run at 2448x1080 contains 206 successful GPU timing samples per stage (1,442 rows), 51 sparse flow-stat rows, and vector-dump completion messages every 600 flow frames. The corrected flow readback gate is confirmed operational with `motion_analysis=1` and `diagnostic_suite=0`. The second log upload has a separate, longer LSFG counter stream; generated WSI-present acceptance and displayed FPS remain unconfirmed.

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
- Verify motion_analysis=1 with diagnostic_suite=0 still produces sparse aggregate flow rows.
- Verify flow readback failures are logged and do not block normal presentation.

## Required Android menu scenarios

1. Toggle Frame Generation during gameplay.
2. Change Flow Scale across low/mid/high values and verify history reset/reallocation.
3. Restart the game and verify persisted settings.
4. Verify the UI reports unavailable Snap AI control until the metric/controller adapter is complete.
5. Verify LSFG ON/OFF independently from SnapFrame and confirm the selected configuration in logs.

## GPU/device test matrix

Compare identical scenes and device conditions:
- Frame Generation OFF.
- Legacy SnapFrame ON, AI OFF.
- SnapFrame ON, AI ON (only after AI telemetry is implemented and validated).

Collect real FPS, generated FPS, stage GPU timings, total GPU time, dropped/generated counters and visual artifact notes. Use median and P90 across repeatable samples. Do not treat policy estimates or mock-backend output as measured GPU savings.

## Acceptance criteria

- Android ARM64 native build succeeds with the pinned NDK/CMake versions.
- No regression in audio, controls, LSFG behavior or game startup.
- Generated-frame output is accepted by the existing DXVK present path.
- AI recommendations demonstrably alter supported dispatch parameters and remain within backend limits.
- All fallback, device recreation and hot-setting tests pass.
- APK is produced by a successful full CI job and installed/tested on a real device.

## Configuration persistence regression test
- Change Flow Scale in the in-game menu, then inspect `port/snapframe.conf`: each key must occupy its own physical line (no literal `\\n` characters).
- Restart the game and confirm the selected Flow Scale is restored rather than silently falling back to the default.
- Confirm the host log uses separate physical lines for backend status and config summary.


## Event logging performance regression test
- Run the same scene and frame count with profiling enabled before/after the event logger change.
- Confirm failure/rejection events are retained immediately and stage-success samples appear at the configured 120-frame cadence.
- Confirm `frame_complete` progress appears at frame 1 and every 300 frames thereafter.
- Compare `snapframe_events.csv` size and verify no per-frame flush regression; GPU timings and skipped-frame counts should remain comparable under the same scene/device conditions.


## Second uploaded device run — 2026-11-04

- SnapFrame capture: 2448x1080, frame checkpoints through 840.
- GPU timing: 28 successful samples per stage; total-chain mean/P95 12.389/16.106 ms. Forward/backward flow mean 4.754/4.824 ms.
- Flow diagnostics: seven aggregate samples and one 5,600-cell vector dump at frame 601. The vector dump is a difficult-motion case (mean confidence ~0.105, ~62.5% below 0.1, ~12% search-boundary saturation, mean consistency ~0.023); do not treat it as a session-wide average.
- Event logging now produces sparse progress samples; verify that failures remain immediately visible in a future run.
- LSFG log has its own longer frame counter (up to 5400) and must not be aligned to SnapFrame's 840-frame counter. LSFG begins at multiplier 2 and later switches to multiplier 1. The available logs show generate dispatches and requested/queued counters, not a confirmed count of accepted generated WSI presents or displayed FPS.
- The full APK has not been built in this CI run. Physical-device visual quality and FPS comparisons remain outstanding.

## Next V2 validation gates

1. Integrate the V2 pyramid, search, refinement, validation and interpolation resources into DXVK behind an explicit experimental mode; keep V1 as default.
2. Implement frame-tagged GPU scalar reduction and asynchronous readback without queue-idle/fence waits.
3. Validate frame IDs, timestamps, resize/reset epochs and two-frames-in-flight resource lifetime.
4. Record actual generated-image present acceptance separately from generation dispatch/request/queue counters.
5. Run same-scene V1/V2 and LSFG ON/OFF captures with the same resolution, multiplier, duration and thermal starting condition.
6. Only then expose V2 and Snap AI adaptive control to normal gameplay.
