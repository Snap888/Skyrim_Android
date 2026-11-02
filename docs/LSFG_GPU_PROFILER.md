# LSFG GPU timestamp log

The DXVK LSFG profiler is opt-in and writes `dxvk_lsfg_gpu.csv` asynchronously from DXVK command-list completion callbacks. It is disabled by default to avoid per-stage query allocation, timestamp commands, completion callbacks and CSV file I/O during normal gameplay.

Enable it for a diagnostic run from the Android launcher: turn on **LSFG GPU stage profiling (CSV)** in the experimental runtime settings. The launcher sets `DXVK_LSFG_GPU_PROFILE=1` before starting the game and passes an absolute output path through `DXVK_LSFG_GPU_PROFILE_LOG`. The native profiler also accepts `1`, `true` and `yes` (case-sensitive) when configured manually. It does not submit a separate Vulkan queue, wait for the GPU, or call `vkQueueWaitIdle`.

## Output location

1. `$DXVK_LOG_PATH/dxvk_lsfg_gpu.csv` when `DXVK_LOG_PATH` is a directory other than `none`.
2. Beside the file named by `DXVK_PERF_LOG`, when that variable is set.
3. Relative fallback: `port/dxvk_lsfg_gpu.csv` under the game's working directory.

The directory must already exist. The existing launcher A/B benchmark remains separate and continues to measure CPU-side PresentImage time.

## CSV columns

`frame,stage,gpu_ms,width,height,status`

Stages:
- `Mipmaps`: mipmap/flow preparation dispatch.
- `Alpha`: complete Alpha pass chain.
- `Beta`: complete Beta pass chain.
- `GammaDelta_L0` … `GammaDelta_L6`: paired Gamma/Delta pyramid work per level (the first four levels contain Gamma only), preserving WinNative dispatch order.
- `Generate`: final frame-generation compute dispatch.

A row with status `ok` contains a GPU timestamp delta converted using Vulkan's `timestampPeriod`. An `unavailable` row means the command-list completed but one or both timestamp results could not be read; it is not a zero-millisecond measurement.

## Test procedure

1. In the launcher, enable **LSFG GPU stage profiling (CSV)** under Experimental runtime tuning.
2. Build and install the APK from the current `snap-patch-5` branch.
3. Run Skyrim with LSFG enabled and allow the generation chain to warm up.
4. Close the game cleanly so pending command lists can retire.
5. Retrieve `port/dxvk_lsfg_gpu.csv`.
6. Send the CSV together with the corresponding `dxvk_perf_on.csv` and device/driver details.

The CSV is generated only when the instrumented GPU chain is executed. Java-only CI does not compile or validate the native C++/Vulkan code; a successful Android NDK build and an actual device run are still required.


## snap-patch-5 profiling review — 2026-11-02

The latest device CSV contains 23,407 successful stage samples across 2,130
source-frame groups. Average per-stage GPU time:

| Stage | Average |
|---|---:|
| Mipmaps | 0.987 ms |
| Alpha | 1.232 ms |
| Beta | 0.874 ms |
| GammaDelta L0–L3 | 0.227 ms combined |
| GammaDelta L4 | 0.258 ms |
| GammaDelta L5 | 0.612 ms |
| GammaDelta L6 | 2.082 ms |
| Generate | 0.930 ms |

The sum of recorded stages averages 7.196 ms per generated-frame chain
(median 6.178 ms, p95 10.475 ms, max 11.509 ms). GammaDelta L6 is the
largest measured group, about 29% of that sum. These are timestamp-query
intervals, not full game-frame time and not a measured FPS gain.

The profiler now defaults to OFF. This avoids its own diagnostic overhead in
ordinary gameplay. Re-enable it explicitly for a profiling run as described
above. The next algorithmic optimization must preserve WinNative pass order,
resource dependencies and DXVK command-list synchronization; do not skip or
merge Gamma/Delta stages based on timing alone.


## Follow-up ON/OFF run — 2026-11-02

The newer upload includes paired `dxvk_perf_off.csv` and
`dxvk_perf_on.csv`, but no `dxvk_lsfg_gpu.csv`. The CPU-side PresentImage
window averages are 5.137 ms OFF (21 windows) and 9.918 ms ON (15 windows).
The largest ON per-window maximum is 838.957 ms. These are not full frame
times or FPS, and the unequal run lengths plus isolated outliers prevent a
clean A/B conclusion. The ON runtime log confirms the DXVK command-list path
and completion through 1800; it does not contain per-stage GPU timestamps.

For the next measurement, keep the normal ON/OFF benchmark profiler-free and
match scene, duration, resolution and VSync. Collect GPU stage timings in a
separate ON diagnostic run by setting `DXVK_LSFG_GPU_PROFILE=1`.

## Latest uploaded runtime profile — 2026-11-02 19:13

The current `Logs/dxvk_lsfg_gpu.csv` contains 33,467 rows, all with
`status=ok`. Per-stage averages (ms) are:

| Stage | Average | P95 |
|---|---:|---:|
| Mipmaps | 0.945 | 1.449 |
| Alpha | 1.177 | 1.807 |
| Beta | 0.834 | 1.269 |
| GammaDelta L0 | 0.053 | 0.080 |
| GammaDelta L1 | 0.049 | 0.076 |
| GammaDelta L2 | 0.050 | 0.076 |
| GammaDelta L3 | 0.059 | 0.091 |
| GammaDelta L4 | 0.241 | 0.378 |
| GammaDelta L5 | 0.571 | 0.895 |
| GammaDelta L6 | 1.955 | 3.030 |
| Generate | 0.872 | 1.300 |

The measured Gamma/Delta/Generate chain averages 3.851 ms (P95 5.927 ms).
The once-per-source-frame Mipmaps/Alpha/Beta group averages 2.955 ms.
There are 3,247 generated chains for 2,497 source-frame groups in this run
(about 1.30 generated chains per source frame), so the sum of measured stage
intervals averages about 7.963 ms per source frame. This sum is not full game
frame time and does not include all DXVK/game work.

GammaDelta L6 remains the largest individual group. The CSV groups all shader
dispatches within each pyramid level; it does not yet identify which individual
Gamma/Delta shader dominates L6.

### CPU benchmark validity

The current ON performance CSV was captured in the same session as the GPU
profiler CSV. The LSFG runtime log also records multiplier changes from x2 to
x4 and back to x2 during that session. Therefore the ON/OFF CPU PresentImage
comparison is not controlled. The previous profiler implementation also
opened and closed the CSV for every stage row, adding avoidable filesystem
work to the instrumented run.

The profiler source now keeps the CSV open, serializes writes, and flushes
every 60 rows instead of probing/opening/closing the file for each stage.
This is a diagnostics-overhead reduction only; it does not change the LSFG
algorithm or claim a gameplay performance improvement. Android NDK build and
device validation of this source change are pending.
