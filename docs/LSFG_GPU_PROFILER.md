# LSFG GPU timestamp log

The DXVK LSFG profiler is opt-in and writes `dxvk_lsfg_gpu.csv` asynchronously from DXVK command-list completion callbacks. It is disabled by default to avoid per-stage query allocation, timestamp commands, completion callbacks and CSV file I/O during normal gameplay.

Enable it for a diagnostic run by setting `DXVK_LSFG_GPU_PROFILE=1` in the game process environment. Accepted values are `1`, `true` and `yes` (case-sensitive). Unset the variable or use another value for normal gameplay. It does not submit a separate Vulkan queue, wait for the GPU, or call `vkQueueWaitIdle`.

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

1. Set `DXVK_LSFG_GPU_PROFILE=1` for the diagnostic run.
2. Build and install the APK from the current `snap-patch-5` branch.
3. Run Skyrim with LSFG enabled and allow the generation chain to warm up.
4. Close the game cleanly so pending command lists can retire.
5. Retrieve `port/dxvk_lsfg_gpu.csv` (or the configured log directory).
6. Send the CSV together with the corresponding `dxvk_perf_on.csv` / `dxvk_perf.log` and the device/driver details.

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
