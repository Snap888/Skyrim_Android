# SnapFrame diagnostic report

- Schema: snapframe-diagnostic-report-v1
- Source folder: port
- Event rows: 1274
- Flow rows: 0
- GPU stage rows: 0

## Present / WSI

- Generated accepted: 82
- Generated failed: 0
- VkResult counts: {}
- Modes observed: {"MAILBOX": 7}

## Event counts

- capture_copy_recorded: 83
- consistency_dispatch_ok: 82
- flow_pair_ok: 82
- frame_complete: 34
- gpu_metrics_copy_recorded: 331
- gpu_metrics_rejected: 331
- host_init: 1
- luma_dispatch_ok: 83
- midpoint_present_accepted: 82
- midpoint_present_requested: 82
- midpoint_resources_ready: 1
- midpoint_warp_ok: 82

## Performance (ms unless noted)

- avg_cpu_present_ms (ms): mean 3.899, median 3.445, p95 6.049, p99 7.571, max 8.078 (n=82)
- max_cpu_present_ms (ms): mean 14.049, median 9.680, p95 48.447, p99 81.440, max 91.463 (n=82)
- avg_source_acquire_ms (ms): mean 0.001, median 0.001, p95 0.001, p99 0.001, max 0.001 (n=82)
- avg_generated_acquire_ms (ms): mean 0.000, median 0.000, p95 0.000, p99 0.001, max 0.001 (n=82)
- avg_source_handoff_wait_ms (ms): mean 1.555, median 1.130, p95 4.527, p99 6.188, max 6.889 (n=82)
- avg_generated_handoff_wait_ms (ms): mean 1.541, median 1.571, p95 2.513, p99 2.941, max 2.978 (n=82)
- avg_source_submission_wait_ms (ms): mean 1.540, median 1.570, p95 2.512, p99 2.940, max 2.976 (n=82)
- avg_generated_submission_wait_ms (ms): mean 1.547, median 1.122, p95 4.515, p99 6.176, max 6.873 (n=82)
- avg_generate_record_ms (ms): mean 0.222, median 0.218, p95 0.300, p99 0.320, max 0.339 (n=82)
- avg_generated_requested (ratio): mean 1.000, median 1.000, p95 1.000, p99 1.000, max 1.000 (n=82)
- avg_generated_queued (ratio): mean 1.000, median 1.000, p95 1.000, p99 1.000, max 1.000 (n=82)

## Findings

### [HIGH] AI_TELEMETRY_REJECTED
331 GPU metric records were rejected; adaptive AI tuning must remain disabled.

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
| snapframe_events.csv | 108872 |
| snapframe_perf_on.csv | 11652 |
| snapframe_flow.csv | 668 |
| snapframe_gpu.csv | 39 |
| snapframe.log | 23861 |
| log.txt | 1564672 |
| snapframe.conf | 47 |

Full logs remain local. This report contains aggregates and selected reasons; it does not upload files.
