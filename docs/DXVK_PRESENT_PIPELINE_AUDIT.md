# DXVK / Vulkan Present Pipeline Audit

Date: 2026-11-04
Branch: `Snap-AI-EngineFrame`

## Scope

This audit follows the Skyrim D3D9 Present path through DXVK, SnapFrame, Vulkan WSI and the Android surface. SDL is not the final image presenter: `runtime/platform_sdl.c` owns SDL window/event integration and invokes the platform present hook after the D3D9 present path. The image submitted to the Android surface is produced by DXVK's Vulkan presenter/WSI path.

## Current path

1. Skyrim renders into the D3D9 backbuffer.
2. `D3D9SwapChainEx::PresentImage()` flushes the D3D9 device and blits the source backbuffer into an acquired WSI image.
3. On the source pass, SnapFrame's copy hook records capture work into the same DXVK command list, before HUD composition.
4. DXVK submits and presents the source image.
5. If SnapFrame reports a midpoint ready, DXVK acquires another WSI image, records the generated image (or a source fallback if generation failed), composes the HUD, and submits the generated image.
6. WSI acceptance is resolved asynchronously at the next existing `SynchronizePresent()` call and reported back to SnapFrame.
7. `SyncFrameLatency()` applies the configured maximum frame latency.

## Confirmed serialization points

- `PresentImage()` calls `SynchronizePresent()` before each source WSI acquisition.
- After source submission, the generated-frame path calls `SynchronizePresent()` again before acquiring/recording the generated image.
- `SynchronizePresent()` calls `m_device->waitForSubmission(&m_presentStatus)`.
- `SubmitPresent()` reuses the single `m_presentStatus` member for both source and generated submissions.
- `SubmitPresent()` also calls `FlushCsChunk()` after queuing the command-list/present work.
- `SyncFrameLatency()` waits on `m_frameLatencySignal` using source `m_frameId` and `GetActualFrameLatency()`.

The source-to-generated wait is a real serialization point. However, removing it directly is unsafe: the current implementation reuses one submission-status object and the same completion path also resolves WSI acceptance and swapchain recreation. A correct overlap implementation needs independently owned per-submission status/lifetime, semaphore/image ownership tracking, and a defined frame-slot retirement policy. Do not simply delete `SynchronizePresent()` or replace it with queue/device idle.

## WSI behavior and robustness

- The presenter acquires WSI images with an infinite timeout. This can block when no swapchain image is available; it must be measured separately from GPU compute.
- The D3D9 present path retries acquisition after swapchain recreation. The retry loop currently has no explicit retry budget. A bounded recovery policy needs to distinguish recoverable `OUT_OF_DATE` / `SUBOPTIMAL` / `SURFACE_LOST` / device-lost results and preserve a valid fallback path.
- Generated-frame failure currently blits the source backbuffer as a valid fallback into the acquired image. This protects against an uninitialized WSI image, but the failure reason should remain separately counted.
- HUD is rendered for both source and generated submissions. This is intentional for HUD clarity, but costs another HUD pass and may create duplicate UI cadence; evaluate separately before changing behavior.
- WSI present acceptance means the Vulkan present operation completed with an accepted result; it is not proof of compositor scanout or physical display cadence.

## Safe next implementation stages

### Stage 1 — instrumentation (no scheduling behavior change)

Record per-frame tagged CPU timestamps for:
- D3D9 flush start/end;
- source WSI acquire duration/result;
- source command recording and submission enqueue;
- source present completion wait duration/result;
- SnapFrame midpoint dispatch duration on CPU;
- generated WSI acquire duration/result;
- generated command recording/submission;
- generated present completion wait duration/result;
- frame-latency wait duration;
- swapchain recreate count and reason;
- generated fallback reason.

Keep GPU timestamp measurements separate from CPU wall-clock measurements. Do not add blocking waits for measurement.

### Stage 2 — independent submission slots

Implemented in commit `b9a9c6ffd233e5957db0a3c9052ce6bc83f09018`: source and generated presents now have independent status objects and pending flags. Each status remains owned by the swap chain until the submission queue publishes its result. Swapchain recreation, reset, invalidation and destruction drain both pending statuses before waiting idle.

### Stage 3 — overlap only after lifetime proof

The generated path no longer calls `SynchronizePresent()` between source submission and generated submission. This removes the CPU-side wait for the source present result before recording/enqueuing FG. The DXVK submission queue still preserves source command-list → source present → generated command-list → generated present order. This is a host-side scheduling improvement; it does not prove GPU execution overlap or a measured FPS gain. The existing capture copy is recorded in the source command list before its present, so queue order preserves the dependency. Device-side resource retirement and capture/flow/output lifetime still require validation.

### Stage 4 — pacing

Revisit frame-latency accounting for two WSI submissions per source frame. The latency signal must represent the intended source-frame policy, not accidentally count generated submissions as additional game frames. Validate input latency, queue depth, frame pacing and source FPS independently.

## Changes made in this audit checkpoint

- Renamed a stale Vulkan presenter diagnostic prefix from `LSFG:` to `SnapFrame:`. The line only reports whether swapchain transfer-source usage is enabled; it was stale text, not an active LSFG implementation.
- The SDL runtime's two stale LSFG bootstrap references were removed in commit `bb41d4856aab09abfce4b48d11bfa3c6fcc7505e`; the bootstrap entry now calls `snapframe_bootstrap()`.
- Added independent source/generated present status slots and removed the CPU `SynchronizePresent()` wait between source and generated submissions. Queue ordering and WSI semaphore ownership are retained; both statuses are drained at the next frame boundary and before swapchain teardown/recreation.
- The generated present acceptance callback continues to use the actual generated WSI result, not enqueue success.
- No `vkQueueWaitIdle` or `vkDeviceWaitIdle` was added to the steady-state frame path. `waitForIdle` remains in swapchain recreation/invalidation paths as before.

## Validation status

- Android Java compilation: passed in workflow run [41874788768](https://github.com/Snap888/Skyrim_Android/actions/runs/41874788768).
- Portable Snap AI / SnapFrame API tests: passed.
- Portable ARM64 library build: passed.
- DXVK SnapFrame host ARM64 compile: passed.
- Full APK build: skipped by this workflow; this run is not an APK artifact validation.
- Physical-device WSI timing, overlap, frame pacing and FPS A/B: not yet validated.


## Follow-up change — independent source/generated submissions (2026-11-04)

- Commit: `b9a9c6ffd233e5957db0a3c9052ce6bc83f09018`.
- `D3D9SwapChainEx` now owns `m_sourcePresentStatus` and `m_generatedPresentStatus` independently; `SubmitPresent` receives the status object for the corresponding submission.
- `WaitForPresentSubmissions()` drains only statuses marked pending and is used before swapchain destruction/recreation and at the next frame boundary.
- Removed the intermediate `SynchronizePresent()` before generated-frame acquisition/recording. The submission queue still serializes Vulkan queue operations in source-before-generated order.
- This removes a CPU wait point but does not guarantee hardware overlap: source and FG still use the same Vulkan queue. Actual benefit, pacing, and device compatibility must be measured on Android.
- CI for this commit is queued as run [41878691981](https://github.com/Snap888/Skyrim_Android/actions/runs/41878691981); no compile or device validation claim is made yet.


## CPU timing instrumentation — 2026-11-04

Commit: `aa5a5e08ebeb675a5bd75bb98240e0d4c012ff2a`.

The existing 120-frame `dxvk_perf.log` sampler now records these additional per-frame CPU wall-clock averages:

- `avg_source_acquire_ms`: source WSI image acquire duration, including any retry/recreate loop.
- `avg_generated_acquire_ms`: generated WSI image acquire duration, including any retry/recreate loop.
- `avg_submit_enqueue_ms`: CPU duration of source + generated `SubmitPresent` calls, including command-list handoff and `FlushCsChunk`; not GPU execution time.
- `avg_generate_record_ms`: CPU duration spent in generated-frame hook, command recording, output blit and HUD recording; not GPU execution time.
- `avg_retire_wait_ms`: time spent resolving previous present submission statuses at the source-frame boundary.
- `avg_frame_latency_wait_ms`: CPU time spent in `SyncFrameLatency()`.

The existing `avg_cpu_present_ms` remains whole-call wall time. These values are sampled and accumulated without adding GPU waits or device/queue idle. A zero generated-acquire value in an OFF run is expected. Values are averages over 120 calls and should be compared using repeated ON/OFF runs in the same scene and with identical resolution/VSync settings.

This instrumentation does not provide GPU timestamps. GPU capture/flow/warp timings must be taken from SnapFrame's existing GPU timing path and correlated by frame ID before drawing conclusions about actual overlap or bottlenecks.
