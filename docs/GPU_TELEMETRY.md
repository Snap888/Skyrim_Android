# GPU telemetry

## Existing telemetry

The DXVK host records timestamp-query timings through asynchronous completion callbacks. The normal frame path does not wait for query completion. The current log samples every 30 frames and records capture/copy, luma, forward flow, backward flow, consistency, midpoint warp and total_chain stages.

The completed total_chain GPU time and CPU frame interval for the sampled frame are exposed as scalar values through `snapframe_host_get_last_telemetry`. The getter is valid only after a successful timestamp-query completion. The portable `SnapFrameFrameOutput` timing fields remain zero because completion is asynchronous.

## Flow diagnostics

The optional flow diagnostic readback copies the two low-resolution RGBA16F flow fields and R16F consistency mask to a host-visible buffer. It runs sparsely (every 120 flow dispatches) and writes aggregate statistics to `snapframe_flow.csv`; vector samples are dumped less frequently to `snapframe_flow_vectors.csv`.

This path is for diagnostics only. It is not the AI telemetry implementation and must not be converted into a per-frame CPU pixel readback. The flow readback gate must follow `motion_analysis`, not `diagnostic_suite`: the latter controls the seven-phase scripted test sequence and should be independently switchable.

A log audit on 2026-11-04 found `motion_analysis=1`, `profiling=1`, `diagnostic_suite=0`, but `snapframe_flow.csv` contained only its header. The cause was the incorrect diagnostic-suite gate in `recordFlowReadback`. The gate was corrected to use `motion_analysis`; the second uploaded device run confirms aggregate rows and periodic vector samples are produced with `diagnostic_suite=0`.

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


## Previous device measurements — 2026-11-04

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


The current diagnostic readback still copies flow images for sparse analysis. It is not suitable for Snap AI. The next implementation should prioritize the V2 coarse-to-fine pyramid and a GPU-side scalar reduction with frame ID/timestamp, followed by asynchronous readback. Do not lower the V1 search radius based on this sample and do not enable AI decisions from these metrics yet.




The sum of all recorded stage durations per frame averages approximately 8.558 ms; P95 is approximately 9.944 ms. This is a derived sum of the stage timestamp intervals, not a displayed-FPS measurement. Highest average individual stages:
- `GammaDelta_L6`: 1.446 ms
- `Generate`: 1.166 ms
- `Alpha`: 0.838 ms
- `Mipmaps`: 0.732 ms
- `Beta`: 0.594 ms
- `GammaDelta_L5`: 0.528 ms
- `GammaDelta_L4`: 0.320 ms


The Android `log.txt` has 6,133 lines and no observed `SIGSEGV`, `SIGABRT`, `VK_ERROR_DEVICE_LOST`, fatal exception or abnormal process termination. It ends with `ExitProcess(0)`. Audio telemetry repeatedly reports source-level `empty_queue` counters, but the output mixer reports `queue_low=0`, `enqueue_fail=0`, `slow_mix=0` and `max_mix` no higher than 1 ms. One 34 ms refill gap is marked `eos=1`. This log does not show an output-mixer underrun; no audio changes are justified by this run.


## Third device log audit — 2026-11-04 (runtime settings changed during capture)

This upload supersedes the preceding second-upload interpretation where the same filenames were used. The run is not one constant configuration: startup config reports SnapFrame `enabled=0`, `flow_scale=75%`; the host later logs a live enable at 75%, a live change to 25%, then a live disable at 25%. The custom Vulkan ICD is Turnip Gen8 V38 (`libvulkan_freedreno.so`). The runtime tuning file explicitly labels CPU affinity and worker spin-wait as `not_implemented`; they were not active tuning controls.

SnapFrame GPU CSV: 23 samples per stage while the flow grid was 112x50 (75%) and 15 samples per stage at 39x17 (25%). Average milliseconds, 75% -> 25%: forward flow 4.460 -> 0.592; backward flow 4.470 -> 0.638; total chain 11.508 -> 3.933. This is about 7.5x lower combined flow time and 2.9x lower measured chain time at 25%, but the samples belong to different gameplay intervals/scenes. Treat as a cost observation, not a controlled quality/performance A/B result. Luma remains ~0.40 ms; midpoint warp ~1.55–1.59 ms. Every sampled GPU stage row reports `ok`.

The aggregate flow samples confirm strong scene dependence. At 75%, frames 121/241/361 have mean confidence ~0.088–0.109 and consistency ~0.0045–0.0142; frame 481 is intermediate (~0.253 confidence / 0.252 consistency); frame 601 is largely static/high-confidence (~0.885 / 0.906). At 25%, frames 826/946/1066 have confidence ~0.100–0.137 and consistency ~0.013–0.040. The 5,600-cell vector dump for frame 601 independently gives mean forward confidence 0.88464, mean consistency 0.90590, 4.625% of cells below confidence 0.1, 6.95% exactly zero consistency, and 0.5% forward vectors at the +/-10 boundary. This corrects the prior second-upload note that attributed ~0.105 confidence and ~0.023 consistency to frame 601; those values do not describe the current uploaded vector file. Do not reduce V1 search radius based on the old mistaken interpretation.


Android config records Turnip Gen8 V38. `runtime_tuning.conf` has diagnostics enabled, while CPU affinity and worker spin-wait are explicitly not implemented. The Android log ends with `ExitProcess(0)`; audio output mixer samples retain `queue_low=0`, `enqueue_fail=0`, `slow_mix=0`, and max_mix no higher than 5 ms. Source-level `empty_queue` counters are present, but there is no output-mixer underrun signal. Preserve the current audio code.

The present path previously incremented SnapFrame's generated counter after the generated image had been recorded/submitted, not after the asynchronous WSI present result completed. The runtime now defers the result callback and HUD generated-frame count until the existing `SynchronizePresent` wait observes `VK_SUCCESS` or the Android-accepted `VK_SUBOPTIMAL_KHR`. This adds no new wait; it reuses the synchronization already required by DXVK. The event is now `midpoint_present_accepted`, which means WSI present completion succeeded, not compositor scanout or displayed-FPS proof. A failed result is reported as failure. Device validation of this new event is still required.




## Fourth upload — stutter-scene analysis (2026-11-04)

The latest SnapFrame capture is configured at 2448x1080 with `enabled=1`, `flow_scale_percent=75`, `motion_analysis=1`, and `profiling=1`. Its 130 GPU timestamp samples show mean stage costs: capture copy 0.451 ms, luma 0.404 ms, forward flow 4.087 ms, backward flow 4.134 ms, consistency 0.011 ms, midpoint warp 1.554 ms, total chain 10.646 ms. Total-chain P95 is 11.802 ms and maximum 14.243 ms. The GPU profile is diagnostic sampling; it does not itself prove that each sample aligns with a visible stutter event.

The 32 aggregate flow samples include many low-confidence moving intervals: 25/32 have forward mean confidence below 0.15. The mean count of forward vectors at the +/-10-pixel search boundary is 616/5600. Some later static intervals have confidence above 0.9, so this is scene-dependent. Do not expand the V1 search radius based on these data; coarse-to-fine pyramid search remains the intended solution for larger motion.




## Active telemetry contract — SnapFrame only

The active runtime records SnapFrame GPU stages, frame-tagged flow quality samples, DXVK CPU Present timing and WSI acceptance events. These streams must be correlated by session and source-frame identity; a GPU dispatch or queued blit is not proof of a displayed generated frame. The legacy backend-specific CSV/log stream has been removed from this branch.

For AI control, use a compact GPU-reduced scalar record tagged with source frame ID and timestamp. Do not feed the AI controller full flow-vector readbacks, do not block Present for telemetry, and reject stale or low-coverage samples. Keep adaptive decisions in bypass until those validity conditions are met.
