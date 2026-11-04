# Android Runtime Tuning — implementation map

**Branch:** `snap-patch-5`  
**Status:** partial implementation; native build and device validation are still required.

This document maps the runtime-tuning changes inspired by the upstream release notes. It distinguishes implemented behavior from work that must not be approximated by changing unrelated DXVK threads.

## 1. User-facing settings

The launcher section **Experimental runtime tuning** stores its controls in the existing `launcher` SharedPreferences. The values are persisted by the normal `saveChoices()` path and applied before `SkyrimActivity` starts SDL/DXVK.

| Setting key | UI label | Default | Effect |
|---|---|---:|---|
| `runtimeSyncPipelines` | Synchronous shader pipeline compilation (noasync.txt) | Off | Creates/removes `port/save/noasync.txt`, exports `DXVK_NO_ASYNC=1/0` |
| `runtimeThreadLogs` | Log DXVK worker thread scheduling | Off | Exports `DXVK_THREAD_DIAGNOSTICS=1/0`; logs TID and current CPU for DXVK shader-cache, submit and finish threads |
| `runtimeSurfaceLogs` | Log Android Surface lifecycle and dimensions | On | Enables `SURFACE_DIAG` log entries from SDL's Android SurfaceView callbacks |
| `runtimeTuningLogs` | Write runtime tuning configuration and diagnostics | On | Exports `DXVK_RUNTIME_DIAGNOSTICS=1/0`; logs asynchronous pipeline compile durations |

The launcher writes `port/runtime_tuning.conf` for inspection. It includes explicit `not_implemented_game_threads` and `not_implemented_game_workers` markers so the configuration does not imply that those two optimizations are active.

## 2. Synchronous first-use pipeline compilation

### Existing pipeline

DXVK's `DxvkStateCache` restores cached pipeline descriptions, maps shader keys to pipeline descriptions, and normally sends newly-ready pipeline work to `m_workerQueue`. The queue is consumed by `workerFunc()` on the `dxvk-shader` worker threads. A separate writer thread persists cache entries.

### Implemented mode

When `DXVK_NO_ASYNC=1` is present at DXVK initialization:

1. `DxvkStateCache` starts in inline-compile mode.
2. It does not create shader-cache compilation workers.
3. `registerShader()` resolves newly-ready pipeline descriptions while holding the entry lock.
4. It releases the entry lock before calling `compilePipelines()`.
5. Pipeline compilation runs synchronously on the caller that registered the shader.
6. Cache writing remains asynchronous; only pipeline compilation changes mode.
7. A log line reports the number of pipelines and elapsed microseconds for each synchronous first-use batch.

The launcher creates the empty marker `port/save/noasync.txt` when the setting is enabled and removes it when disabled. The launcher mirrors the marker state into `DXVK_NO_ASYNC` because the native DXVK process reads the environment at initialization.

**Expected trade-off:** this may prevent content from being drawn before its required cached pipeline is ready, but the first use can stall the registering/render thread. It is not a universal fix for every kind of shader pop-in: shader translation, Vulkan pipeline creation, driver compilation, and cache misses are distinct stages.

## 3. DXVK thread diagnostics

When `DXVK_THREAD_DIAGNOSTICS=1`, Android builds log the Linux TID and CPU observed at thread start for:

- `dxvk-shader` — DXVK state-cache compiler worker;
- `dxvk-submit` — DXVK command submission thread;
- `dxvk-queue` — DXVK submission completion/fence thread.

When `DXVK_RUNTIME_DIAGNOSTICS=1`, asynchronous pipeline jobs log their elapsed compilation time.

These are DXVK infrastructure threads. **They are not Skyrim's main, Havok, streaming, IO, or job-system threads.** The logs are useful to characterize DXVK scheduling, but they cannot prove that the game worker threads are placed on performance cores.

## 4. Android Surface lifecycle diagnostics

The Android SDL `SDLSurface` callbacks now emit `SURFACE_DIAG` records when enabled:

- `created`: surface validity and current view dimensions;
- `changed`: SurfaceView dimensions, real display dimensions, refresh rate, validity, ready state;
- `destroyed`: last known dimensions and validity during teardown.

The existing SDL lifecycle still owns pause/resume and native surface notifications. This patch adds visibility; it does not replace SDL's Surface lifecycle or introduce a second wait loop.

### Why no additional DXVK wait loop was added

The custom `dxvk_native_surface()` registration is used by the port's native-window WSI path. Waiting in `createSurface()` without a matching lifetime protocol could block DXVK while Android's UI thread is responsible for publishing the replacement window. A correct recovery change needs a generation-aware handoff, explicit native-window reference ownership, swapchain invalidation, and a bounded wait that does not hold DXVK locks.

## 5. Work intentionally not implemented yet

### Skyrim game-thread CPU affinity

The upstream behavior applies to the game's own main/job/Havok threads and exempts below-normal streaming/IO/background threads. The checked-in source exposes DXVK's thread wrapper and its own compiler/submission workers, but that is not the game's thread-creation or priority-classification layer.

Do not pin every DXVK thread and call that a Skyrim fix. A correct implementation needs a hook at the game/Wine thread creation and priority assignment boundary, with:

- runtime CPU topology discovery (allowed CPUs and performance-core identification);
- preservation of Android cpuset and thermal restrictions;
- affinity only for game threads at or above the intended priority;
- explicit exemption for below-normal streaming/IO/background work;
- TID, role, priority, allowed mask, applied mask and failure-reason logs;
- a safe fallback to the Android scheduler.

### Skyrim worker spin-wait back-off

The upstream change modifies a particular worker wait loop that repeatedly calls Windows `Sleep(0)`. The current checked-in DXVK worker queues use condition variables, not that Skyrim worker loop. Altering DXVK's condition-variable waits or globally replacing `yield()` would change unrelated synchronization and could increase power use without fixing Havok/job waits.

Required next step: locate the exact Skyrim worker implementation and its wait loop, instrument yield count and wait duration, then make the 32/50-us versus 256/20-us policy configurable only at that loop.

## 6. Log collection

For a test run, enable:

- **Log DXVK worker thread scheduling**;
- **Log Android Surface lifecycle and dimensions**;
- **Write runtime tuning configuration and diagnostics**.

Relevant outputs:

| Output | Location / source | What it answers |
|---|---|---|
| Runtime settings snapshot | `<game>/port/runtime_tuning.conf` | Which modes were requested |
| noasync marker | `<game>/port/save/noasync.txt` | Whether synchronous pipeline mode was requested |
| DXVK log | Existing DXVK log destination configured by the port | Pipeline mode, compile duration, DXVK thread TID/CPU |
| Android Surface log | `adb logcat -s SDL:I` | Surface create/change/destroy, actual sizes and refresh |

Suggested capture command:

```sh
adb logcat -c
# Launch Skyrim and reproduce the issue, then:
adb logcat -d -s SDL:I SkyrimRuntime:I > skyrim_runtime_surface.log
```

The DXVK log must be collected separately from its configured file destination. Do not assume the Android logcat contains all DXVK Logger output.

## 7. Validation matrix

Run the same save, location, camera direction, graphics settings, resolution, refresh rate, and SnapFrame configuration for each pass. Warm up the device and avoid thermal throttling.

| Pass | Sync pipelines | Thread logs | Surface logs | Purpose |
|---|---:|---:|---:|---|
| A | Off | On | On | Baseline |
| B | On | On | On | First-use hitch vs object pop-in |
| C | Off | On | On | Repeat after cache warm-up |
| D | Off | On | On | Background/foreground and rotation lifecycle |

Record average FPS, 1%/0.1% lows, frame-time trace, first-use stalls, CPU core placement, clocks, temperature, and whether objects appear late. Compare source-game FPS separately from accepted SnapFrame output FPS.

### Acceptance criteria

- Default settings preserve current asynchronous behavior.
- Synchronous mode does not create compiler workers and logs each first-use batch duration.
- Cache writer still shuts down cleanly and cache files remain readable.
- Surface diagnostics report coherent dimensions and state transitions without changing normal lifecycle behavior.
- No SnapFrame command recording, queue synchronization, or generation resource lifetime is changed by these options.
- NDK build and real-device tests pass before enabling any mode by default.

## 8. Current implementation status

- [x] Launcher controls and persistent settings.
- [x] Empty `port/save/noasync.txt` marker management.
- [x] Runtime environment flags set before SDL/DXVK startup.
- [x] Synchronous first-use compilation path in `DxvkStateCache`.
- [x] DXVK shader/submit/finish worker TID and CPU diagnostics.
- [x] Async and synchronous pipeline compile timing logs.
- [x] Android Surface lifecycle/dimension diagnostics.
- [x] Runtime configuration snapshot.
- [ ] Skyrim main/job/Havok thread affinity integration.
- [ ] Skyrim worker spin-wait loop identification and configurable back-off.
- [ ] Generation-aware Android Surface recovery protocol in DXVK WSI.
- [ ] NDK build, APK install, runtime and performance validation.

**No claim of successful compilation or device validation is made by this document.**
