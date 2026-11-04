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

Replace the single mutable `m_presentStatus` with a bounded per-submission status/retirement structure. Each slot must own its status until the corresponding completion has been observed. Keep source and generated submissions separately tagged. Preserve WSI semaphore reuse rules and make swapchain recreation drain only the submissions that reference the old swapchain.

### Stage 3 — overlap only after lifetime proof

Once slot ownership and completion are independently tracked, allow source present and SnapFrame compute to overlap where resource dependencies permit. Keep the visible order source → generated → next source. Do not let generated work read a source image before its capture copy is ordered, and do not reuse capture/flow/output resources before their GPU completion values retire.

### Stage 4 — pacing

Revisit frame-latency accounting for two WSI submissions per source frame. The latency signal must represent the intended source-frame policy, not accidentally count generated submissions as additional game frames. Validate input latency, queue depth, frame pacing and source FPS independently.

## Changes made in this audit checkpoint

- Renamed a stale Vulkan presenter diagnostic prefix from `LSFG:` to `SnapFrame:`. The line only reports whether swapchain transfer-source usage is enabled; it was stale text, not an active LSFG implementation.
- The SDL runtime's two stale LSFG bootstrap references were removed in commit `bb41d4856aab09abfce4b48d11bfa3c6fcc7505e`; the bootstrap entry now calls `snapframe_bootstrap()`.
- No unsafe synchronization removal was made. The current source/generated serialization is documented for the next staged implementation rather than bypassed without per-submission lifetime tracking.

## Validation status

- Android Java compilation: passed in workflow run [41874788768](https://github.com/Snap888/Skyrim_Android/actions/runs/41874788768).
- Portable Snap AI / SnapFrame API tests: passed.
- Portable ARM64 library build: passed.
- DXVK SnapFrame host ARM64 compile: passed.
- Full APK build: skipped by this workflow; this run is not an APK artifact validation.
- Physical-device WSI timing, overlap, frame pacing and FPS A/B: not yet validated.
