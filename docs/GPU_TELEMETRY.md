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


## Event log overhead control — 2026-11-04

The event CSV previously flushed after every event, while the render callback emitted several successful stage events plus `frame_complete` for every captured frame. That caused synchronous stdio flushes on the frame path and unnecessarily large event logs. The logger now uses a 64 KiB stdio buffer, flushes critical failures/rejections and lifecycle/test-phase events immediately, and otherwise flushes every 64 accepted event records. Routine successful stage/present events are sampled every 120 frames; `frame_complete` progress is sampled every 300 frames (the first event is retained). Failures are never rate-limited. This reduces diagnostic I/O pressure without changing the GPU pipeline or its dispatch cadence. Validate event ordering, failure retention and log size on the next device run.


## Latest device measurements — 2026-11-04

The latest uploaded run contains 206 successful timestamp samples per stage at 2448x1080. Average/P95 milliseconds: capture/copy 0.568/0.739; luma 0.404/0.403; forward flow 4.456/5.722; backward flow 4.450/5.485; consistency 0.012/0.014; midpoint warp 1.612/1.565; total chain 11.508/14.270. The small P95 values below averages for luma/warp reflect highly clustered samples with a few outliers; use the full CSV when comparing builds. Total chain remains the SnapFrame GPU chain only.

The sparse flow CSV contains 51 samples. Mean per-sample confidence is approximately 0.230 forward and 0.230 backward. Across the 285,600 vector observations represented by those 51 grids, about 53.7% of forward and 53.8% of backward vectors have confidence below 0.1; about 9.9%/9.8% are pinned to the search boundary. Confidence and consistency vary substantially by scene. Do not reduce search coverage until the vectors are validated against repeatable scene captures and image quality.


## Latest device log audit — 2026-11-04 (second upload)

The new upload contains a shorter SnapFrame capture at 2448x1080: 840 captured-frame checkpoints in `snapframe.log`, 28 successful timestamp samples per stage (196 rows total), seven sparse flow-stat rows (frames 121, 241, 361, 481, 601, 721, 841), and one 5,600-cell vector dump for frame 601. Every GPU CSV row reports `status=ok`.

Average / P95 GPU milliseconds across the 28 samples:
- capture/copy: 0.734 / 0.798
- luma: 0.420 / 0.728
- forward flow: 4.754 / 5.901
- backward flow: 4.824 / 5.908
- consistency: 0.012 / 0.018
- midpoint warp: 1.638 / 2.829
- total chain: 12.389 / 16.106

The two flow directions consume about 9.578 ms, approximately 77% of the measured SnapFrame chain. The flow search remains the primary GPU cost. From approximately sample frame 720 onward, luma and midpoint-warp times rise from their earlier ~0.34 ms / ~1.31 ms cluster to ~0.73 ms / ~2.83 ms. Resolution remains unchanged. This suggests a GPU clock/load/thermal state change, but the logs contain no temperature or frequency telemetry, so the cause is unconfirmed.

The seven aggregate flow samples are highly scene-dependent. Their unweighted sample means are ~0.452 forward confidence, ~0.452 backward confidence, ~4.58 / ~4.62 mean vector magnitude and ~0.409 consistency. These averages hide large per-scene differences: at frames 121/241/361/601 confidence is roughly 0.08–0.11 and consistency is 0.004–0.023; at frames 481/721 confidence is 0.84–0.93 with consistency ~0.89–0.93; frame 841 is effectively static with confidence ~0.999 and consistency ~0.999.

The single vector dump at frame 601 has mean confidence ~0.105 in both directions; ~62.5% of cells in each direction have confidence below 0.1, ~98.1% below 0.5, ~11.8–12.1% are pinned at the +/-10 search boundary, and mean consistency is ~0.023 with ~91.7% of cells exactly zero. This one dump is a difficult-motion sample, not a session-wide quality average. Confidence is a best-vs-second-best SAD ambiguity score, not a literal accuracy percentage.

The event log is now sparse and contains sampled progress events rather than per-frame successes. This is consistent with the new event sampling policy; however, compare file sizes only against runs with similar duration and event configuration.

The LSFG log is a separate, longer counter stream than the 840-frame SnapFrame capture: LSFG reports present progress up to frame 5400 while SnapFrame stops at frame 840. Do not correlate their frame IDs or assume they cover identical intervals. LSFG starts with `enabled=1 mult=2`, records real WSI images, history copies and generate dispatch commands, then logs a live settings change to `multiplier=1`; the final `lsfg.conf` also contains multiplier=1. `dxvk_perf_on.csv` reports 31 CPU-present samples through frame 3720, with mean `avg_cpu_present_ms` ~30.32 ms and mean requested/queued generated counts ~0.978. These counters indicate generation requests were queued, but the log set does not provide a confirmed generated-image WSI-present/acceptance counter or a measured displayed-FPS result. Therefore, real LSFG output cadence and FPS uplift remain unverified.

The current diagnostic readback still copies flow images for sparse analysis. It is not suitable for Snap AI. The next implementation should prioritize the V2 coarse-to-fine pyramid and a GPU-side scalar reduction with frame ID/timestamp, followed by asynchronous readback. Do not lower the V1 search radius based on this sample and do not enable AI decisions from these metrics yet.
