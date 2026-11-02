# DXVK performance A/B test (LSFG off / on)

## Purpose

Collect a separate, low-volume DXVK present-path log before changing allocators, caches,
descriptor handling, or command recording. The test is intentionally observational: it
does not change buffer sizes, cache sizes, pipeline compilation, or LSFG dispatch.

## Recovery point

The pre-test source snapshot is preserved by branch:

`backup/pre-dxvk-benchmark-2026-11-01`

It points to the exact `snap-patch-3` HEAD that existed before benchmark instrumentation.

## Log

The launcher now exposes a **DXVK Performance A/B test** section. Enable **Write separate DXVK benchmark log**, select pass A or B, then press Play:

- Pass A forces LSFG off for that launch and writes `port/dxvk_perf_off.csv` inside the selected Skyrim game folder.
- Pass B forces LSFG on for that launch and writes `port/dxvk_perf_on.csv` inside the selected Skyrim game folder. A valid Lossless.dll is required.
- The launcher creates the `port` directory if needed, removes the previous file for the selected pass, and sets `DXVK_PERF_LOG` and `DXVK_PERF_RUN` in the app process before SDL/DXVK starts.
- Disable the benchmark checkbox for ordinary launches; the launcher then unsets both benchmark variables and leaves the normal LSFG checkbox in control.

The benchmark logger writes independently of the regular DXVK log:

- Preferred: set `DXVK_PERF_LOG` to an absolute writable file path.
- Fallback: if `DXVK_PERF_LOG` is unset, it writes `dxvk_perf.log` into `DXVK_LOG_PATH`.
- If neither variable provides a path, benchmark logging is disabled.
- Set `DXVK_PERF_RUN` to identify the pass. The value is written in the log header.

The parent directory must already exist. Use a different log file for each pass, or copy
the file between passes so the two runs cannot be confused.

Example environment values (adapt the path to the app's writable log directory):

```sh
DXVK_PERF_LOG=/sdcard/Android/data/<package>/files/logs/dxvk_perf_off.csv
DXVK_PERF_RUN=lsfg_off
```

For the second pass:

```sh
DXVK_PERF_LOG=/sdcard/Android/data/<package>/files/logs/dxvk_perf_on.csv
DXVK_PERF_RUN=lsfg_on
```

The benchmark log is CSV-like text with a header. A data row is emitted after each 120
presented game frames. Columns:

- `frame`: cumulative frame count at the end of the sample window
- `avg_cpu_present_ms`: average CPU time spent in DXVK's `PresentImage` path
- `min_cpu_present_ms`, `max_cpu_present_ms`: min/max CPU present-path time in that window
- `avg_lsfg_requested`: average requested generated frames per source frame
- `avg_lsfg_generated`: average successfully generated frames per source frame
- `width`, `height`: present image extent
- `vsync`: whether DXVK's current present path uses vsync

This is CPU-side present-path timing, not GPU execution time and not total game frame
time. It includes DXVK flush, present acquisition/synchronization, command recording,
submission and generation/presentation work performed in this function. It does not
separate those components yet. No GPU timestamp queries are added in this first pass.

## Test procedure

Keep device, driver, game settings, resolution, location, camera direction, save, and
background apps identical.

### Pass A — LSFG disabled

1. Disable LSFG completely in the app/game settings.
2. Set `DXVK_PERF_RUN=lsfg_off` and a dedicated `DXVK_PERF_LOG` path.
3. Launch Skyrim and load a repeatable save/location.
4. Wait 60 seconds for loading and shader warm-up.
5. Keep the same scene running for at least 3 minutes; avoid menus, loading screens,
   fast travel, and camera/scene transitions where possible.
6. Exit normally so logs are flushed.

### Pass B — LSFG enabled

1. Enable LSFG at the intended test multiplier (start with x2).
2. Set `DXVK_PERF_RUN=lsfg_on` and a different log path.
3. Repeat exactly the same save, location, resolution, graphics settings, and camera path.
4. Wait 60 seconds, then collect at least 3 minutes of stable gameplay.
5. Exit normally.

Do not change graphics settings or FPS limits between passes. If the game is capped at a
fixed rate, keep that cap unchanged. Record the selected LSFG multiplier alongside the logs.

## What to send back

Send both benchmark logs and, if available, the regular DXVK log from each pass. Also note:

- device model and Android version
- GPU and driver version (if already shown in the normal log)
- game resolution and FPS cap
- LSFG multiplier for Pass B
- whether either pass had visible stutter, slowdown, or crashes

I will compare CPU present-path averages and spikes, LSFG generation success, and run
consistency before deciding which subsystem to optimize next.

## Interpretation limits

- This first test does not measure GPU frame time, draw-call count, texture upload bytes,
  pipeline cache hit rate, descriptor churn, or allocator fragmentation.
- A lower CPU present time does not automatically mean higher FPS; GPU bottlenecks may
  dominate.
- LSFG on/off results must be compared at the same source-game workload. Generated output
  frames are not equivalent to additional game simulation frames.
- The benchmark is passive and should not affect rendering decisions or LSFG resource
  lifetime.


## Latest ON-only capture — 2026-11-03 00:58

The uploaded `dxvk_perf_on.csv` is an ON-only run with
`lsfg_mode_is_runtime_observed=true`, source extent 2448x1080 and VSync=1.
It contains 76 windows from frame 120 through 9120. Requested and generated
frame averages are between 0.983333 and 1.0, consistent with multiplier x2.

| Metric | ON |
|---|---:|
| Mean of window averages | 7.112 ms |
| Median of window averages | 6.555 ms |
| P95 of window averages | 11.501 ms |
| Minimum / maximum window average | 1.290 / 13.152 ms |
| Maximum `max_cpu_present_ms` | 824.513 ms |
| First 10 window averages (mean) | 6.665 ms |
| Last 10 window averages (mean) | 10.880 ms |

The increase in the last ten windows suggests a sustained-load change, but this
single CSV cannot attribute it to thermals, scene complexity, scheduling or
another cause. The 824.513 ms value is an isolated CPU Present-path stall; it
must not be reported as normal frame time.

No `dxvk_perf_off.csv` accompanies this upload. Do not calculate an ON/OFF
delta from this run. Repeat both passes in the same scene with identical
duration, resolution, VSync, graphics settings and LSFG settings; disable GPU
timestamp profiling for both CPU benchmark passes. Keep the separate ON-only
GPU-profiled run for shader-stage diagnosis.
