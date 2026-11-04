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
- `SynchronizePresent()` drains the independently tracked source/generated submission slots through `WaitForPresentSubmissions()`. This retires the queued CS operations, but does not prove presentation-engine semaphore retirement.
- `SubmitPresent()` now uses separate source and generated `DxvkSubmitStatus` slots. These track queued submission results only; they do not provide independent WSI image/acquire ownership.
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

A prior attempt removed the generated-path `SynchronizePresent()` wait, but review found this unsafe with the current Presenter state model. The wait has been restored. The source and generated submissions remain independently tracked, but the Presenter still exposes one mutable acquire state (`m_acquireStatus`, `m_imageIndex`, `m_frameIndex`) and pre-acquires the next image on the CS thread. Do not remove the barrier until explicit acquire tickets and semaphore retirement are implemented and validated.

### Stage 4 — pacing

Revisit frame-latency accounting for two WSI submissions per source frame. The latency signal must represent the intended source-frame policy, not accidentally count generated submissions as additional game frames. Validate input latency, queue depth, frame pacing and source FPS independently.

## Changes made in this audit checkpoint

- Renamed a stale Vulkan presenter diagnostic prefix from `LSFG:` to `SnapFrame:`. The line only reports whether swapchain transfer-source usage is enabled; it was stale text, not an active LSFG implementation.
- The SDL runtime's two stale LSFG bootstrap references were removed in commit `bb41d4856aab09abfce4b48d11bfa3c6fcc7505e`; the bootstrap entry now calls `snapframe_bootstrap()`.
- Added independent source/generated present status slots and removed the CPU `SynchronizePresent()` wait between source and generated submissions. Queue ordering and WSI semaphore ownership are retained; both statuses are drained at the next frame boundary and before swapchain teardown/recreation.
- The generated present acceptance callback continues to use the actual generated WSI result, not enqueue success.
- No `vkQueueWaitIdle` or `vkDeviceWaitIdle` was added to the steady-state frame path. `waitForIdle` remains in swapchain recreation/invalidation paths as before.

## WSI acquire-ticket safety correction — 2026-11-04

Commit: `8702981ed6bdb41390f44f236652d8f733161056`.

A follow-up review found that the previous removal of the source-to-generated wait was not safe with the current `Presenter` API:

- `Presenter::acquireNextImage()` returns the single mutable `m_imageIndex`, `m_frameIndex`, and `m_acquireStatus` state.
- `Presenter::presentImage()` is executed asynchronously on the DXVK CS thread; it advances the frame index and pre-acquires the next WSI image.
- The application thread could request the generated image before the queued source present advanced that state. The second acquire could therefore observe the source acquire token/image again.
- Independent `DxvkSubmitStatus` objects do not make WSI acquire state independent.

For correctness, `PresentImage()` now calls `SynchronizePresent()` before generated-frame acquisition again. This intentionally restores the CPU wait until Presenter is redesigned around explicit per-acquire tickets carrying the acquired image index, semaphore pair, and generation. Do not remove this barrier until source and generated acquire tickets are independently reserved and consumed exactly once.

Consequences:
- The previous host-side overlap optimization is temporarily disabled.
- The independent status objects remain useful for explicit submission result tracking and safe teardown.
- No FPS improvement is claimed from the previous change.
- The next implementation must first add ticket-based acquire/present APIs and validate semaphore reuse, swapchain recreation, and CS-thread ordering; only then may the barrier be removed.

## Bounded WSI acquire recovery — 2026-11-04

Code commit: `241d00525003de96d4c2ba4f2eadd83519d8e910`.

- Source and generated WSI acquire now use a bounded three-attempt recovery loop instead of an unbounded retry loop.
- Each failed acquire is logged with its VkResult and retry count; the swapchain is recreated before the next attempt.
- If source acquisition still fails, `PresentImage()` exits without submitting commands that reference an unacquired image.
- If generated acquisition still fails, SnapFrame reports a rejected generated frame and skips the generated submission rather than using an invalid image/semaphore pair.
- This prevents a persistent surface/acquire error from trapping the render thread in an infinite retry loop. It is a reliability fix, not an FPS optimization.
- The temporary source-to-generated `SynchronizePresent()` barrier remains. Ticket-based independent WSI acquisition is not implemented; do not remove the barrier based on this change.
- CI and device validation are pending.

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


## Presenter ticket redesign — source audit and implementation gate (2026-11-04)

### Confirmed current implementation

- `Presenter::acquireNextImage(PresenterSync&, uint32_t&)` returns the current mutable `m_imageIndex` and semaphore pair selected by `m_frameIndex`. It only calls `vkAcquireNextImageKHR` while `m_acquireStatus == VK_NOT_READY`; repeated calls can return the same outstanding acquisition.
- `Presenter::presentImage()` reads the same mutable frame/image state, queues `vkQueuePresentKHR`, advances `m_frameIndex`, then immediately pre-acquires the next image into that mutable state.
- DXVK queues `SubmitPresent` work on the CS thread. The application thread and CS thread therefore interact with this state at different times. Separate source/generated `DxvkSubmitStatus` objects do not serialize or identify WSI acquisitions.
- `WaitForPresentSubmissions()` waits until the queued CS operation and `vkQueuePresentKHR` call have returned. That is not proof that the presentation engine has consumed the present-wait semaphore.
- `recreateSwapChain()` destroys image views and both semaphore arrays. Its callers must first retire queued submissions and ensure no acquire or presentation operation can still reference the old generation.

### Why a superficial ticket wrapper is insufficient

Copying `imageIndex` and the two semaphore handles into a struct, without changing acquisition reservation and semaphore retirement, would only snapshot the same mutable token. It would not make source and generated acquisitions independent. Likewise, a monotonically increasing ticket ID does not prove that a binary semaphore is safe to signal again or destroy.

### Required ticket model

1. Add a monotonically increasing `swapchainGeneration`; every successful swapchain creation gets a new generation.
2. Introduce a move-only `PresenterAcquireTicket` with generation, unique ticket ID, acquired image index, acquire semaphore, present semaphore, and explicit lifecycle state. It must not be implicitly copyable.
3. Reserve a distinct acquire semaphore for every outstanding acquisition. A ticket may be consumed exactly once by the matching queued present operation. Reject stale-generation, duplicate-consume, and invalid-state tickets.
4. Keep present-wait semaphores distinct from acquire semaphores. Reuse a present-wait semaphore only after a Vulkan-guaranteed retirement event. Preferred baseline: associate present semaphores with swapchain image indices and mark them reusable only after that same image is reacquired (or use a supported present-fence mechanism where available). A CPU wait for `vkQueuePresentKHR` return alone is insufficient.
5. Make the DXVK CS closure capture the ticket by move/value, not a pointer or reference to mutable Presenter state. The closure must submit commands waiting on that ticket's acquire semaphore and signal its matching present semaphore, then call `presentImage(ticket)`.
6. Keep queue ordering source command list → source present → generated command list → generated present. Ticketing must not silently move work to another queue or weaken the source-capture dependency.
7. On `OUT_OF_DATE`, `SURFACE_LOST`, device loss, or other acquire/present errors, transition the ticket to a terminal failed state. Never retry a failed ticket or submit using its image index. Recovery creates a new generation after old work is retired.
8. Swapchain recreation/destruction must stop new reservations, drain queued CS work, and retire all outstanding acquire/present operations before destroying semaphores, image views, or the old swapchain. Do not assume that a returned present call means the presentation engine has released its semaphore.
9. Add debug assertions and low-overhead counters for outstanding tickets, duplicate consumption, stale generations, semaphore reuse, failed acquisitions, and retirement reason.

### Implementation sequence

- A. Add the ticket type and state-machine unit tests without changing runtime behavior.
- B. Refactor Presenter acquisition to reserve independent tickets and make the CS closure own the ticket.
- C. Implement per-image present-semaphore retirement using reacquisition proof; add extension-based present fences only behind feature detection and only if the extension is actually enabled.
- D. Convert source and generated paths; preserve the temporary `SynchronizePresent()` barrier.
- E. Run Android ARM64 compile and Vulkan validation-layer tests where available. Exercise acquire failure, surface loss, swapchain resize/recreate, repeated present, and teardown.
- F. Only after all ownership tests pass, remove the source-to-generated barrier and compare source FPS, generated-present acceptance, acquire wait, retire wait, and frame-latency wait on device.

### Acceptance criteria

- No image or semaphore is used by two live tickets.
- Every successful acquire is consumed once or explicitly retired through a documented recovery path.
- No present semaphore is re-signaled before presentation-engine wait consumption is guaranteed.
- No old-generation ticket reaches a new swapchain.
- Source and generated presents remain ordered.
- The temporary synchronization barrier stays enabled until all above criteria pass.
- No FPS improvement is claimed until matched on-device A/B testing demonstrates it.
