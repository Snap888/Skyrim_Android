# DXVK WSI acquire-ticket redesign

Status: design specification; implementation intentionally gated on the ownership and retirement rules below.

## Why the current Presenter cannot safely overlap source and generated acquisition

The current `vk::Presenter` has one mutable acquisition state:

- `m_frameIndex` selects a semaphore pair.
- `m_imageIndex` stores the currently acquired swapchain image.
- `m_acquireStatus` records whether that image has been acquired.
- `acquireNextImage(sync, index)` returns the current state; it does not create an independent acquisition object.
- `presentImage()` consumes the current image, calls `vkQueuePresentKHR`, advances the frame slot and immediately pre-acquires the next image.

DXVK queues `presentImage()` on the CS thread. The game thread records/submits the source present and then may proceed before the CS-thread present operation has advanced the Presenter state. A generated acquire on the game thread at that point can therefore observe the source acquisition state. Independent `DxvkSubmitStatus` objects only track completion of queued operations; they do not provide independent WSI image ownership.

The current `SynchronizePresent()` between source and generated acquisition is consequently a required correctness barrier. Do not remove it until the acquisition lifecycle is redesigned and tested.

## Current safety boundaries

- Source acquisition is performed before its command list is queued.
- The CS-thread operation submits the command list using the acquired semaphore pair and then calls Presenter present.
- Presenter present queues `vkQueuePresentKHR`, advances its frame slot and pre-acquires the next image.
- The generated path currently waits for the source submission to retire before asking Presenter for its next acquisition.
- Swapchain recreation waits for outstanding present submissions and device idle before destroying image views, semaphores and the old swapchain.
- Bounded acquire retries (three attempts) are failure recovery, not a concurrency mechanism.

## Target API

Replace the implicit current-image state with an explicit, move-only acquisition record. The record is a reservation, not merely a copy of Vulkan handles.

Conceptual shape:

```cpp
struct PresenterAcquireTicket {
  uint64_t generation;
  uint64_t serial;
  uint32_t imageIndex;
  uint32_t acquireSlot;
  VkSemaphore acquireSemaphore;
  VkSemaphore presentSemaphore;
  // private ownership token/state; move-only
};

VkResult acquireNextImage(PresenterAcquireTicket& ticket);
VkResult presentImage(PresenterAcquireTicket&& ticket);
void abandonAcquire(PresenterAcquireTicket&& ticket, ...);
```

The actual implementation must not expose freely copyable tickets. A ticket must be consumed exactly once. A second consume, stale generation, invalid semaphore, or already-retired ticket must fail closed and emit a diagnostic.

### Ownership rules

1. A successful `vkAcquireNextImageKHR` creates exactly one ticket.
2. The ticket binds the swapchain generation, acquired image index, acquire semaphore and the present semaphore reserved for that image.
3. The exact ticket must travel by value/move through the DXVK CS-thread closure. Do not re-read mutable Presenter fields inside that closure.
4. Submission waits on the ticket's acquire semaphore and signals its present semaphore.
5. `vkQueuePresentKHR` waits on that ticket's present semaphore and presents that ticket's image index.
6. The ticket is marked consumed only after the present operation has been enqueued. Enqueue success is not the same as presentation completion.
7. A ticket cannot be silently discarded after a successful acquire. Any cancellation path must consume the acquire semaphore safely or force a controlled swapchain recovery; it must never submit/present an image that was not acquired.
8. Queue ordering must remain source command submission/present before generated command submission/present on the same queue unless an explicit synchronization proof permits otherwise.

## Semaphore retirement

A ticket design must distinguish three events:

- acquire returned an image and signaled the acquire semaphore;
- graphics submission consumed the acquire semaphore and signaled the present semaphore;
- presentation engine consumed the present semaphore.

A graphics submission fence proves the second event, not necessarily the third. Do not recycle a present-wait semaphore merely because `vkQueuePresentKHR` returned or because the graphics submission completed.

Prefer present-wait semaphore ownership tied to the swapchain image. Reacquiring the same image is the natural proof point that the previous presentation of that image has progressed sufficiently for its present semaphore to be reused. If a different retirement mechanism is selected, it must be supported by enabled Vulkan extensions/features and have an explicit fallback; do not assume `VK_KHR_present_wait` or present fences are available on Android/Turnip.

Acquire semaphores also need explicit retirement after the queue submission has consumed their signal. A per-frame acquire-slot ring may be used only when its slot cannot be reused before the corresponding submission wait has completed.

## Swapchain generation and teardown

- Increment a monotonically increasing generation whenever the swapchain is replaced or invalidated.
- Every ticket carries the generation at acquisition.
- Before destroying swapchain resources, stop issuing acquisitions and drain all queued command-list/present closures that hold tickets.
- Wait for graphics submissions and presentation retirement according to the selected semaphore strategy.
- Invalidate remaining tickets only after their work is known to be retired; stale tickets must never reach `vkQueuePresentKHR`.
- On surface loss, swapchain out-of-date or persistent acquire failure, fail the affected ticket and enter the existing controlled recreate path.
- Keep the current device-idle recreate path until a narrower retirement protocol is demonstrated correct.

## Required implementation stages

1. **Inventory and contract tests:** enumerate every Presenter acquire/present caller and every swapchain recreation/destruction path. Verify whether Presenter is ever shared across queues or threads.
2. **Ticket API without overlap:** introduce move-only tickets while retaining the source-to-generated retirement barrier. Prove exact acquire/submit/present matching and generation invalidation.
3. **Semaphore retirement:** implement image-owned present semaphores and acquire-slot retirement. Add assertions and diagnostics for double-consume, stale generation, semaphore reuse and unconsumed tickets.
4. **Failure-path validation:** test acquire out-of-date, surface lost, suboptimal, submit failure, present failure, reset, resize, minimize/zero extent, and teardown with queued work.
5. **Controlled overlap experiment:** only after stages 1–4 pass, remove the source/generated CPU retirement barrier behind a diagnostic runtime switch. Compare source FPS, generated WSI acceptance, acquire latency, CS queue latency, frame-latency wait and GPU stage times.
6. **Default enablement:** remove the experimental switch only after repeated Android device tests show no validation-layer errors, stale images, semaphore reuse errors, flicker or black frames.

## Acceptance criteria

- Every successful acquire maps to one and only one present or explicit safe cancellation.
- No ticket crosses swapchain generations.
- No semaphore is signaled twice without a valid intervening wait/retirement.
- No present references an image that was not acquired by that ticket.
- Resize/reset/teardown cannot destroy resources referenced by queued work.
- Source/generated WSI results are accounted separately and only after their actual `vkQueuePresentKHR` result is observed.
- A/B testing demonstrates reduced CPU wait without reducing source FPS or increasing rejected generated presents.
- Android ARM64 CI passes and the implementation is exercised on the target Vulkan driver.

## Current decision

Keep `SynchronizePresent()` before generated acquisition. The presenter's current mutable state and asynchronous CS-thread advancement make removal unsafe. This document is an implementation contract, not a claim that ticket-based acquisition or overlap is already implemented.
