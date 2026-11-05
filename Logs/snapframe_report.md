# SnapFrame diagnostic report

- Schema: snapframe-diagnostic-report-v1
- Source folder: port
- Event rows: 2815
- Flow rows: 0
- GPU stage rows: 0

## Present / WSI

- Generated accepted: 34
- Generated failed: 2093
- VkResult counts: {"1": 2093}
- Modes observed: {"AUTO": 5, "FIFO": 2}

## Event counts

- capture_copy_recorded: 49
- consistency_dispatch_ok: 48
- flow_pair_ok: 48
- frame_complete: 20
- gpu_metrics_copy_recorded: 195
- gpu_metrics_rejected: 195
- host_init: 1
- luma_dispatch_ok: 49
- midpoint_present_accepted: 34
- midpoint_present_failed: 2093
- midpoint_present_requested: 34
- midpoint_resources_ready: 1
- midpoint_warp_ok: 48

## Performance (ms unless noted)

- avg_cpu_present_ms (ms): mean 5.458, median 4.388, p95 12.241, p99 13.889, max 14.002 (n=49)
- max_cpu_present_ms (ms): mean 16.663, median 13.568, p95 29.879, p99 68.841, max 84.465 (n=49)
- avg_source_acquire_ms (ms): mean 1.569, median 0.026, p95 11.092, p99 12.362, max 12.575 (n=49)
- avg_generated_acquire_ms (ms): mean 0.014, median 0.009, p95 0.052, p99 0.074, max 0.084 (n=49)
- avg_source_handoff_wait_ms (ms): mean 0.802, median 0.185, p95 2.582, p99 3.863, max 4.899 (n=49)
- avg_generated_handoff_wait_ms (ms): mean 2.280, median 2.505, p95 3.795, p99 4.254, max 4.321 (n=49)
- avg_source_submission_wait_ms (ms): mean 2.279, median 2.504, p95 3.794, p99 4.253, max 4.320 (n=49)
- avg_generated_submission_wait_ms (ms): mean 0.797, median 0.179, p95 2.576, p99 3.857, max 4.892 (n=49)
- avg_generate_record_ms (ms): mean 0.163, median 0.157, p95 0.300, p99 0.384, max 0.415 (n=49)
- avg_generated_requested (ratio): mean 0.997, median 1.000, p95 1.000, p99 1.000, max 1.000 (n=49)
- avg_generated_queued (ratio): mean 0.643, median 0.650, p95 0.992, p99 1.000, max 1.000 (n=49)

## Findings

### [HIGH] GENERATED_WSI_BACKPRESSURE
2093 generated-frame failures report VK_NOT_READY.

Recommendation: Keep game presentation non-blocking. Inspect per-mode acquire availability and presenter pre-acquire/ticket ownership before increasing timeout.

### [HIGH] AI_TELEMETRY_REJECTED
195 GPU metric records were rejected; adaptive AI tuning must remain disabled.

Recommendation: Validate shader valid/expected cell counts against CPU grid dimensions and derive coverage from the same counters.

### [MEDIUM] GPU_STAGE_CSV_EMPTY
snapframe_gpu.csv has no stage samples.

Recommendation: Enable stage profiling briefly; keep diagnostic V2 workloads disabled during normal gameplay.

### [MEDIUM] FLOW_CSV_EMPTY
snapframe_flow.csv has no flow samples.

Recommendation: Check flow diagnostics export gating; accepted dispatch events alone do not prove flow quality.

## Files

| File | Bytes |
|---|---:|
| snapframe_events.csv | 240698 |
| snapframe_perf_on.csv | 7202 |
| snapframe_flow.csv | 668 |
| snapframe_gpu.csv | 39 |
| snapframe.log | 13945 |
| log.txt | 1474275 |
| snapframe.conf | 47 |

Full logs remain local. This report contains aggregates and selected reasons; it does not upload files.
