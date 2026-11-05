# SnapFrame diagnostic report

- Schema: snapframe-diagnostic-report-v1
- Source folder: port
- Event rows: 1024
- Flow rows: 0
- GPU stage rows: 265

## Present / WSI

- Generated accepted: 66
- Generated failed: 0
- VkResult counts: {}
- Modes observed: {"MAILBOX": 7}

## Event counts

- capture_copy_recorded: 67
- consistency_dispatch_ok: 66
- flow_pair_ok: 66
- frame_complete: 27
- gpu_metrics_copy_recorded: 265
- gpu_metrics_published: 265
- host_init: 1
- luma_dispatch_ok: 67
- midpoint_present_accepted: 66
- midpoint_present_requested: 66
- midpoint_resources_ready: 2
- midpoint_warp_ok: 66

## Performance (ms unless noted)

- avg_cpu_present_ms (ms): mean 2.895, median 2.524, p95 4.569, p99 4.870, max 4.936 (n=66)
- max_cpu_present_ms (ms): mean 9.717, median 6.618, p95 35.907, p99 45.867, max 46.090 (n=66)
- avg_source_acquire_ms (ms): mean 0.000, median 0.000, p95 0.001, p99 0.001, max 0.001 (n=66)
- avg_generated_acquire_ms (ms): mean 0.000, median 0.000, p95 0.001, p99 0.001, max 0.001 (n=66)
- avg_source_handoff_wait_ms (ms): mean 1.296, median 1.148, p95 3.456, p99 3.695, max 3.723 (n=66)
- avg_generated_handoff_wait_ms (ms): mean 0.966, median 1.145, p95 1.700, p99 1.885, max 1.954 (n=66)
- avg_source_submission_wait_ms (ms): mean 0.966, median 1.144, p95 1.699, p99 1.884, max 1.953 (n=66)
- avg_generated_submission_wait_ms (ms): mean 1.290, median 1.143, p95 3.449, p99 3.688, max 3.716 (n=66)
- avg_generate_record_ms (ms): mean 0.178, median 0.176, p95 0.233, p99 0.279, max 0.324 (n=66)
- avg_generated_requested (ratio): mean 1.000, median 1.000, p95 1.000, p99 1.000, max 1.000 (n=66)
- avg_generated_queued (ratio): mean 1.000, median 1.000, p95 1.000, p99 1.000, max 1.000 (n=66)

## Findings

### [MEDIUM] FLOW_CSV_EMPTY
snapframe_flow.csv has no flow samples.

Recommendation: Check flow diagnostics export gating; accepted dispatch events alone do not prove flow quality.

## Files

| File | Bytes |
|---|---:|
| snapframe_events.csv | 92740 |
| snapframe_perf_on.csv | 9528 |
| snapframe_flow.csv | 668 |
| snapframe_gpu.csv | 10338 |
| snapframe.log | 24467 |
| log.txt | 1500650 |
| snapframe.conf | 47 |

Full logs remain local. This report contains aggregates and selected reasons; it does not upload files.
