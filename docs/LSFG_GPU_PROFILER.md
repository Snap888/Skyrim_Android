# LSFG GPU timestamp log

The DXVK LSFG profiler writes `dxvk_lsfg_gpu.csv` asynchronously from DXVK command-list completion callbacks. It does not submit a separate Vulkan queue, wait for the GPU, or call `vkQueueWaitIdle`.

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
- `GammaDelta`: paired Gamma/Delta pyramid chain, preserving WinNative dispatch order.
- `Generate`: final frame-generation compute dispatch.

A row with status `ok` contains a GPU timestamp delta converted using Vulkan's `timestampPeriod`. An `unavailable` row means the command-list completed but one or both timestamp results could not be read; it is not a zero-millisecond measurement.

## Test procedure

1. Build and install the APK from `snap-patch-3`.
2. Run Skyrim with LSFG enabled and allow the generation chain to warm up.
3. Close the game cleanly so pending command lists can retire.
4. Retrieve `port/dxvk_lsfg_gpu.csv` (or the configured log directory).
5. Send the CSV together with the corresponding `dxvk_perf_on.csv` / `dxvk_perf.log` and the device/driver details.

The CSV is generated only when the instrumented GPU chain is executed. Java-only CI does not compile or validate the native C++/Vulkan code; a successful Android NDK build and an actual device run are still required.
