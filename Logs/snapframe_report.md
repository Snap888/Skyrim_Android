# SnapFrame diagnostic report

- Schema: snapframe-diagnostic-report-v1
- Source folder: port
- Event rows: 503
- Flow rows: 0
- GPU stage rows: 130

## Present / WSI

- Generated accepted: 32
- Generated failed: 0
- VkResult counts: {}
- Modes observed: {"MAILBOX": 7}

## Event counts

- capture_copy_recorded: 33
- consistency_dispatch_ok: 32
- flow_pair_ok: 32
- frame_complete: 14
- gpu_metrics_copy_recorded: 130
- gpu_metrics_published: 130
- host_init: 1
- luma_dispatch_ok: 33
- midpoint_present_accepted: 32
- midpoint_present_requested: 32
- midpoint_resources_ready: 2
- midpoint_warp_ok: 32

## Performance (ms unless noted)

- avg_cpu_present_ms (ms): mean 5.314, median 4.605, p95 11.630, p99 12.624, max 13.006 (n=32)
- max_cpu_present_ms (ms): mean 17.486, median 17.377, p95 27.327, p99 46.395, max 54.689 (n=32)
- avg_source_acquire_ms (ms): mean 0.001, median 0.001, p95 0.001, p99 0.001, max 0.001 (n=32)
- avg_generated_acquire_ms (ms): mean 0.000, median 0.000, p95 0.001, p99 0.001, max 0.001 (n=32)
- avg_source_handoff_wait_ms (ms): mean 3.912, median 3.215, p95 10.391, p99 10.545, max 10.555 (n=32)
- avg_generated_handoff_wait_ms (ms): mean 0.906, median 0.686, p95 1.827, p99 2.056, max 2.140 (n=32)
- avg_source_submission_wait_ms (ms): mean 0.905, median 0.685, p95 1.826, p99 2.054, max 2.139 (n=32)
- avg_generated_submission_wait_ms (ms): mean 3.899, median 3.203, p95 10.368, p99 10.522, max 10.531 (n=32)
- avg_generate_record_ms (ms): mean 0.089, median 0.088, p95 0.111, p99 0.113, max 0.113 (n=32)
- avg_generated_requested (ratio): mean 0.999, median 1.000, p95 1.000, p99 1.000, max 1.000 (n=32)
- avg_generated_queued (ratio): mean 0.999, median 1.000, p95 1.000, p99 1.000, max 1.000 (n=32)

## Findings

### [MEDIUM] FLOW_CSV_EMPTY
snapframe_flow.csv has no flow samples.

Recommendation: Check flow diagnostics export gating; accepted dispatch events alone do not prove flow quality.

## Files

| File | Bytes |
|---|---:|
| snapframe_events.csv | 45479 |
| snapframe_perf_on.csv | 4907 |
| snapframe_flow.csv | 668 |
| snapframe_gpu.csv | 5073 |
| snapframe.log | 14587 |
| log.txt | 1460403 |
| snapframe.conf | 47 |

Full logs remain local. This report contains aggregates and selected reasons; it does not upload files.
