# SnapFrame diagnostic report

- Schema: snapframe-diagnostic-report-v1
- Source folder: port
- Event rows: 682
- Flow rows: 0
- GPU stage rows: 147

## Present / WSI

- Generated accepted: 36
- Generated failed: 0
- VkResult counts: {}
- Modes observed: {"MAILBOX": 7}

## Event counts

- ai_scale_change_deferred: 116
- capture_copy_recorded: 37
- consistency_dispatch_ok: 36
- flow_pair_ok: 36
- frame_complete: 15
- gpu_metrics_copy_recorded: 147
- gpu_metrics_published: 147
- host_init: 1
- luma_dispatch_ok: 37
- midpoint_present_accepted: 36
- midpoint_present_requested: 36
- midpoint_resources_ready: 2
- midpoint_warp_ok: 36

## Performance (ms unless noted)

- avg_cpu_present_ms (ms): mean 6.157, median 5.195, p95 12.231, p99 13.221, max 13.630 (n=36)
- max_cpu_present_ms (ms): mean 18.781, median 19.449, p95 26.376, p99 69.711, max 91.963 (n=36)
- avg_source_acquire_ms (ms): mean 0.001, median 0.001, p95 0.001, p99 0.001, max 0.001 (n=36)
- avg_generated_acquire_ms (ms): mean 0.000, median 0.000, p95 0.001, p99 0.001, max 0.001 (n=36)
- avg_source_handoff_wait_ms (ms): mean 4.850, median 4.181, p95 11.103, p99 11.766, max 11.980 (n=36)
- avg_generated_handoff_wait_ms (ms): mean 0.800, median 0.691, p95 1.478, p99 1.875, max 1.937 (n=36)
- avg_source_submission_wait_ms (ms): mean 0.798, median 0.690, p95 1.477, p99 1.874, max 1.936 (n=36)
- avg_generated_submission_wait_ms (ms): mean 4.835, median 4.174, p95 11.078, p99 11.742, max 11.956 (n=36)
- avg_generate_record_ms (ms): mean 0.085, median 0.083, p95 0.105, p99 0.110, max 0.113 (n=36)
- avg_generated_requested (ratio): mean 1.000, median 1.000, p95 1.000, p99 1.000, max 1.000 (n=36)
- avg_generated_queued (ratio): mean 1.000, median 1.000, p95 1.000, p99 1.000, max 1.000 (n=36)

## Findings

### [MEDIUM] FLOW_CSV_EMPTY
snapframe_flow.csv has no flow samples.

Recommendation: Check flow diagnostics export gating; accepted dispatch events alone do not prove flow quality.

## Files

| File | Bytes |
|---|---:|
| snapframe_events.csv | 61776 |
| snapframe_perf_on.csv | 5444 |
| snapframe_flow.csv | 668 |
| snapframe_gpu.csv | 5736 |
| snapframe.log | 15820 |
| log.txt | 1465380 |
| snapframe.conf | 47 |

Full logs remain local. This report contains aggregates and selected reasons; it does not upload files.
