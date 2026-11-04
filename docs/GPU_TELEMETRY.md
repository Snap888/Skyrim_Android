# GPU telemetry

## Existing telemetry

The DXVK host records timestamp-query timings through asynchronous completion callbacks. The normal frame path does not wait for query completion. The current log samples every 30 frames and records capture/copy, luma, forward flow, backward flow, consistency, midpoint warp and total_chain stages.

The completed total_chain GPU time and CPU frame interval for the sampled frame are exposed as scalar values through `snapframe_host_get_last_telemetry`. The getter is valid only after a successful timestamp-query completion. The portable `SnapFrameFrameOutput` timing fields remain zero because completion is asynchronous.

## Flow diagnostics

The optional flow diagnostic readback copies the two low-resolution RGBA16F flow fields and R16F consistency mask to a host-visible buffer. It runs sparsely (every 120 flow dispatches) and writes aggregate statistics to `snapframe_flow.csv`; vector samples are dumped less frequently to `snapframe_flow_vectors.csv`.

This path is for diagnostics only. It is not the AI telemetry implementation and must not be converted into a per-frame CPU pixel readback. The flow readback gate must follow `motion_analysis`, not `diagnostic_suite`: the latter controls the seven-phase scripted test sequence and should be independently switchable.

A log audit on 2026-11-04 found `motion_analysis=1`, `profiling=1`, `diagnostic_suite=0`, but `snapframe_flow.csv` contained only its header. The cause was the incorrect diagnostic-suite gate in `recordFlowReadback`. The gate has been corrected to use `motion_analysis`; the next device run must confirm that aggregate rows and periodic vector samples are actually produced.

## AI-safe telemetry

Preferred AI inputs are scalar values:
- per-stage and total GPU milliseconds from completed timestamp queries;
- motion magnitude and complexity from a GPU-side reduction;
- flow confidence and consistency/artifact risk from GPU-side reduction;
- scene-cut and frame-order flags;
- generated, skipped and failed frame counters.

Do not copy full pixel or flow images to CPU for AI. Do not insert queue-idle waits or fence waits into the real-frame path. GPU reduction should write a small scalar result buffer and consume it only after asynchronous completion.

## Current status

- Asynchronous stage timing: implemented and producing successful samples.
- Scalar total GPU time and frame interval export: implemented.
- Sparse CPU flow readback for diagnostics: implemented; gating bug fixed, pending device confirmation.
- GPU-side flow magnitude/confidence/complexity reduction: not implemented.
- Scene-cut detection: not implemented.
- Snap AI metric submission and adaptive decision loop: not implemented.

The Snap AI controller must remain disabled/unavailable until valid GPU-derived metrics are connected and tested. Do not claim performance gains from the current telemetry alone.

### Configuration/logging correction — 2026-11-04
The uploaded `Logs/snapframe.conf` contained literal backslash-n separators instead of real line breaks. The settings writer in `runtime/main.c` used double-escaped newline sequences, so multiple key/value pairs could be serialized as one physical line and later keys (including Flow Scale) could be ignored by the line-based parser. This is now corrected. The same escaped-newline issue was fixed in host/bootstrap diagnostic strings. Verify the next runtime-generated config contains one key per physical line and that Flow Scale survives restart.
