# DXVK 2.6.2 WSI crash analysis — 2026-11-07

## Evidence from the uploaded device logs

Device: ASUS_AI2205_C, Android 15, Adreno 740, 2448x1080.

- Process exit: `CRASH_NATIVE`, signal 11 (`SIGSEGV`), status 11.
- Fault: `SEGV_MAPERR`, address `0x2000000001`, thread 25020.
- Crash handler reports the faulting DXVK frames:
  - `libdxvk_d3d9_262.so + 0x175a60`
  - `libdxvk_d3d9_262.so + 0x1758c0`
  - callers at `+0x176a38`, `+0x1c4518`, `+0x1b65a4`, `+0x1b16b0`, `+0x1b0d58`, `+0xfae38`, `+0x15434c`, `+0x1546c8`.
- Immediately before the crash, `log.txt` contains 3,226 occurrences of `Presenter: Got VK_SUBOPTIMAL_KHR, recreating swapchain`.
- Across those repeated recreations, the reported swapchain configuration stays at 2448x1080, FIFO, six images, `VK_FORMAT_A2B10G10R10_UNORM_PACK32`. There is no evidence in this log of a changing extent that would require rebuilding the swapchain on every frame.
- System RAM was not exhausted: 4,863 MB available at the next launch.
- SnapFrame was initialized disabled (`enabled=0`) in the uploaded `snapframe.log`; this crash run does not validate FG.
- The uploaded `snapframe_events.csv`, `snapframe_flow.csv`, and `snapframe_gpu.csv` are empty. The report's aggregate counts and byte sizes do not match the files uploaded to GitHub, so those aggregates are not treated as device evidence.

## Root cause hypothesis and fix

The pinned DXVK 2.6.2 Presenter treats every non-`VK_SUCCESS` acquire/present result as a reason to recreate the swapchain. On this Android/Adreno surface, `VK_SUBOPTIMAL_KHR` is returned persistently while the swapchain extent and image configuration remain stable. Vulkan defines SUBOPTIMAL as a usable swapchain status; it is not the same as `VK_ERROR_OUT_OF_DATE_KHR`.

The Android overlay now normalizes `VK_SUBOPTIMAL_KHR` to `VK_SUCCESS` at both `vkAcquireNextImageKHR` call sites and after `vkQueuePresentKHR`. This keeps the already acquired image and its semaphore transaction alive, permits the normal next-image pre-acquire path, and prevents the dirty-swapchain flag from being set for a usable surface. Genuine negative errors such as OUT_OF_DATE and SURFACE_LOST retain the upstream recovery path. The change is Android-only; desktop DXVK behavior is unchanged.

This addresses the strongest reproducible WSI defect visible immediately before the crash. The exact machine instruction at the faulting DXVK offset still requires symbolizing the unstripped 2.6.2 build, so the fix is a targeted containment of the demonstrated swapchain churn, not a claim that every possible SIGSEGV is solved.

## Audio and FG observations

The same run reports per-source xWMA starvation counters between roughly 3,700 and 7,800 empty-queue events per one-second interval, while the output queue reports `queue_low=0` and no enqueue failures. This needs a separate audio-source timing investigation; it is not evidence of the WSI crash cause and no speculative audio scheduling change is included here.

The next FG test should enable SnapFrame explicitly from the in-game X11 settings (the uploaded config only shows the startup default), then verify that the actual runtime config and non-empty event/flow/GPU CSVs are copied from the device's `port` directory. Keep FG off for the first stability run to isolate the WSI fix.

## Validation required from the next APK

1. Confirm the log no longer repeats `Got VK_SUBOPTIMAL_KHR, recreating swapchain` on every frame.
2. Confirm swapchain creation occurs at startup and only after real surface/out-of-date transitions.
3. Run the same scene for at least 10 minutes with FG off; collect `log.txt`, `app_process64_d3d9.log`, `last_exit.txt`, and any `last_exit_*.trace`.
4. Then enable FG and collect all SnapFrame CSV files and the matching runtime config/log from the same run.
5. If SIGSEGV remains, symbolize the reported DXVK offsets using the exact unstripped 2.6.2 build from that APK.

