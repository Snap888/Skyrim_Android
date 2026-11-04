# Device testing — SnapFrame / DXVK

## Controlled baseline

Run the same save, camera path, resolution, VSync mode and duration for each pass. Record branch/commit, APK version, device/GPU driver, resolution, refresh rate, graphics settings and thermal starting condition.

1. SnapFrame OFF: verify normal DXVK rendering and record source-frame interval and CPU Present timings.
2. SnapFrame ON at 50% flow scale: record generated-frame WSI acceptance, GPU stage timings and flow-quality telemetry.
3. Repeat at 75% only if the 50% pass is stable; compare quality and total GPU cost rather than assuming higher scale is better.
4. Repeat the problematic scene and note exact points where game motion slows or accelerates.
5. Run a long session to check history/resource lifecycle, audio, input and surface recreation.

## Required logs

- `snapframe.log`
- `snapframe_events.csv`
- `snapframe_gpu.csv`
- `snapframe_flow.csv`
- `dxvk_perf.log` when CPU Present/frame-interval profiling is enabled
- Android `log.txt` / logcat for crashes or Vulkan errors

## Acceptance criteria

- Source-frame presentation remains functional with SnapFrame disabled or when generation fails.
- Generated-frame counters increment only after WSI reports `VK_SUCCESS` or `VK_SUBOPTIMAL_KHR`.
- No additional synchronous GPU readback or device-wide idle wait is introduced into the steady-state frame path.
- Frame-latency synchronization and WSI submission ordering remain correct.
- No regression in audio, touch controls, gamepad input or startup.
- GPU stage timings, CPU Present timings and source-frame intervals are analyzed on matching session/frame identifiers.
- No FPS uplift or image-quality improvement is claimed without same-scene device measurements and visual review.

## AI validation

MotionV2 metrics must be frame-tagged and asynchronously read back as compact scalar records. Invalid coverage, stale frame IDs, scene cuts or low confidence must disable adaptive decisions for that sample. Keep V2 out of runtime until descriptor/resource lifecycle, dispatch ordering, synchronization and output parity pass device testing.
