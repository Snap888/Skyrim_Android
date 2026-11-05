# SnapFrame diagnostic report

- Schema: snapframe-diagnostic-report-v1
- Source folder: port
- Event rows: 524
- Flow rows: 0
- GPU stage rows: 151

## Present / WSI

- Generated accepted: 20
- Generated failed: 0
- VkResult counts: {}
- Modes observed: {"MAILBOX": 7}

## Event counts

- ai_scale_change_deferred: 27
- ai_wsi_backpressure: 75
- ai_wsi_backpressure_recovered: 75
- capture_copy_recorded: 38
- consistency_dispatch_ok: 20
- flow_pair_ok: 20
- frame_complete: 16
- gpu_metrics_copy_recorded: 76
- gpu_metrics_published: 76
- host_init: 1
- luma_dispatch_ok: 38
- midpoint_present_accepted: 20
- midpoint_present_requested: 20
- midpoint_resources_ready: 2
- midpoint_warp_ok: 20

## Performance (ms unless noted)

- avg_cpu_present_ms (ms): mean 6.251, median 4.199, p95 16.486, p99 18.357, max 19.342 (n=37)
- max_cpu_present_ms (ms): mean 39.100, median 22.110, p95 80.438, p99 97.954, max 103.607 (n=37)
- avg_source_acquire_ms (ms): mean 0.001, median 0.001, p95 0.001, p99 0.001, max 0.001 (n=37)
- avg_generated_acquire_ms (ms): mean 0.000, median 0.000, p95 0.001, p99 0.001, max 0.001 (n=37)
- avg_source_handoff_wait_ms (ms): mean 4.907, median 3.270, p95 14.600, p99 16.118, max 16.960 (n=37)
- avg_generated_handoff_wait_ms (ms): mean 0.984, median 0.806, p95 1.736, p99 1.974, max 2.095 (n=37)
- avg_source_submission_wait_ms (ms): mean 4.795, median 2.090, p95 15.636, p99 17.466, max 18.430 (n=37)
- avg_generated_submission_wait_ms (ms): mean 1.090, median 0.548, p95 3.179, p99 3.250, max 3.289 (n=37)
- avg_generate_record_ms (ms): mean 0.045, median 0.042, p95 0.091, p99 0.102, max 0.105 (n=37)
- avg_generated_requested (ratio): mean 0.493, median 0.392, p95 1.000, p99 1.000, max 1.000 (n=37)
- avg_generated_queued (ratio): mean 0.493, median 0.392, p95 1.000, p99 1.000, max 1.000 (n=37)

## Findings

### [MEDIUM] FLOW_CSV_EMPTY
snapframe_flow.csv has no flow samples.

Recommendation: Check flow diagnostics export gating; accepted dispatch events alone do not prove flow quality.

## Files

| File | Bytes |
|---|---:|
| snapframe_events.csv | 48439 |
| snapframe_perf_on.csv | 5891 |
| snapframe_flow.csv | 668 |
| snapframe_gpu.csv | 5892 |
| snapframe.log | 41834 |
| log.txt | 1510792 |
| snapframe.conf | 47 |

Full logs remain local. This report contains aggregates and selected reasons; it does not upload files.
