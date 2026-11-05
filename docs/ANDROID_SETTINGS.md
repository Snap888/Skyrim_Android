# Android settings integration

## Current status

The in-game overlay exposes a **Snap AI / Frame Generation** section. Supported controls are SnapFrame enable/disable and Flow Scale (25–100%). These call the native SnapFrame API and persist to `port/snapframe.conf`. Snap AI adaptive control remains unavailable until telemetry is calibrated and controller decisions are safely connected.

The launcher contains no external LSFG controls, DLL import, LSFG A/B test, or LSFG GPU profiler. On game launch it removes the retired external integration's generated files (`lsfg.conf`, `lsfg_gpu_profile.conf`, and `dxvk_lsfg_gpu.csv`) and unsets the old profiler environment variables.

## HUD and benchmark

- HUD controls configure DXVK's `fps` item and its Game FPS, FG submit FPS and Output submit FPS fields.
- Game FPS increments only after a source WSI present is accepted.
- FG submit FPS increments only after the generated WSI present result is accepted; enqueue/dispatch alone is not counted.
- Output submit FPS is the sum of accepted source and generated presents.
- The DXVK benchmark pass is now SnapFrame OFF/ON. Pass A bypasses SnapFrame; pass B runs the active SnapFrame pipeline. CSVs are `port/snapframe_perf_off.csv` and `port/snapframe_perf_on.csv`.

## Persistence and application

SnapFrame and Flow Scale settings persist in `port/snapframe.conf`. Native state is authoritative. JNI validates flow scale and returns failure if the host is not initialized. GPU-affecting changes are queued to the render callback; the UI thread never accesses Vulkan objects. The toast confirms queueing, not completed GPU resource recreation.

Only expose settings that the active backend can actually apply. Performance/quality modes, custom search radius, AI adaptive controls, CPU affinity and worker spin-wait remain hidden until implemented and validated.

## SnapFrame presentation mode

The launcher exposes `Auto`, `FIFO`, `MAILBOX`, and `IMMEDIATE` under
SnapFrame Frame Generation. The preference is written as `present_mode` in
`port/snapframe.conf` and read by the native bootstrap before DXVK creates
the swapchain.

The selected mode applies to the **shared DXVK swapchain**, so it affects both
source game frames and generated frames; it is not a separate FG-only queue.
`Auto` preserves the existing game VSync/tear-free selection. Explicit
`FIFO`, `MAILBOX`, and `IMMEDIATE` requests are checked against the surface's
supported modes. MAILBOX and IMMEDIATE fall back to FIFO when unavailable.
The Presenter log records the requested mode and the actual selected Vulkan
present mode.

The selection is applied at game startup / swapchain creation. Change it in
the launcher and restart the game to run a controlled A/B test. MAILBOX may
discard stale queued frames; IMMEDIATE can tear and does not guarantee scanout
at the submit rate. Output submit FPS remains a submission metric, not a
measurement of displayed refresh.
