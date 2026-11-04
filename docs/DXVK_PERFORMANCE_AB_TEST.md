# DXVK / SnapFrame controlled A/B test

This test compares the normal DXVK path with the same path using SnapFrame. There is no secondary frame-generation backend.

## Pass A — SnapFrame disabled

1. Disable **Snap AI / Frame Generation** in the in-game overlay.
2. Load the fixed save and reproduce the same camera path and difficult scene.
3. Capture `dxvk_perf.log`, `log.txt`, and the runtime configuration.
4. Record displayed/source FPS separately; do not treat CPU Present duration as displayed FPS.

## Pass B — SnapFrame enabled

1. Enable SnapFrame and start with 50% flow scale.
2. Repeat the same save, scene, camera path, resolution, VSync and duration.
3. Capture `snapframe.log`, `snapframe_events.csv`, `snapframe_gpu.csv`, `snapframe_flow.csv`, and `dxvk_perf.log`.
4. Repeat at 75% only if the 50% pass is stable.

## Metrics and interpretation

Compare on a shared session/frame timeline:

- Source-frame interval: mean, median, P95, P99 and maximum.
- CPU `D3D9SwapChainEx::PresentImage`: mean, median, P95, P99 and maximum.
- SnapFrame GPU stage timings and total-chain cost.
- Generated-frame requests, dispatch failures and WSI-accepted generated presents.
- Flow confidence, consistency, low-confidence ratio, occlusion and scene-cut candidates.
- Device thermal/frequency state when available.

A successful GPU dispatch is not proof that the generated image was accepted by WSI or displayed. Only the DXVK WSI result callback may increment accepted-generated-frame counters.

## Safety rules

- Keep the normal source-frame present path available if SnapFrame is disabled or generation fails.
- Do not remove `SyncFrameLatency()` or add device-wide idle waits as a pacing workaround.
- Do not compare different scenes or different thermal starting conditions.
- Do not claim FPS uplift without matched device measurements and a displayed-frame measurement.
