# SnapFrame + DXVK Native 2.6.2: presentation integration findings

Status: architecture investigation; no generated frame is presented yet.

Pinned upstream source:
- Repository: Snap888/dxvk
- Revision: 9d6f54a1ade20d1d27dd421024717a636f3d8c68

## Verified D3D9 present sequence

In `src/d3d9/d3d9_swapchain.cpp`, `D3D9SwapChainEx::PresentImage` currently:

1. Ends the application frame and flushes the D3D9 parent context.
2. Calls `Presenter::acquireNextImage(sync, backBuffer)` on the CPU side.
3. Emits one CS-thread closure carrying that acquired image and its `PresenterSync`.
4. The closure calls `ctx->beginExternalRendering()` exactly once.
5. The SnapFrame source hook records compute work into that same DXVK command list.
6. `DxvkBlitter::present` blits the real game backbuffer into the acquired WSI image.
7. `ctx->synchronizeWsi(sync)` prepares the WSI semaphore handoff.
8. `ctx->flushCommandList(nullptr, nullptr)` submits through DXVK's normal submission queue.
9. `DxvkDevice::presentImage` queues the present operation through the same DXVK submission queue.

This is a single acquire / single command-list submission / single present transaction. The real game image is still the WSI blit source.

## Verified Presenter constraints

In `src/dxvk/dxvk_presenter.cpp`:

- `acquireNextImage` waits until `m_presentPending` is false; it intentionally allows only one acquired/present-pending image per Presenter.
- The acquired image and acquire/present semaphores are selected from the current `m_frameIndex`.
- `presentImage` waits on that frame slot's `present` semaphore and presents `m_imageIndex`.
- On success it advances `m_frameIndex` and eagerly acquires the next swapchain image using the next slot's acquire semaphore.
- The next acquire is tracked in `m_acquireStatus` / `m_imageIndex`; it is not a spare image that a second independent present may reuse.
- The Presenter uses a single `m_imageIndex` and one acquire status, so a second present cannot safely be inserted by calling `presentImage` twice or by reusing the same `PresenterSync`.

## Consequences for frame generation

The current SnapFrame hook must remain non-presenting until a separate generated-frame transaction exists. Replacing `cSrcView` with the midpoint output would replace the real game frame, not insert an extra frame. Calling `presentImage` twice would reuse mutable Presenter state and invalid semaphore/image ownership. Creating a second raw Vulkan queue submission or presenting outside DXVK would bypass DXVK's submission ordering and is explicitly out of scope.

The hook currently returns the skip/no-generated-frame result. This is intentional and must not be changed to report FG active while the generated image is not displayed.

## Required safe integration shape

A correct implementation needs a DXVK-owned presentation scheduler, not an extra call from the SnapFrame host:

1. Keep the ordinary game present transaction intact and ordered.
2. Give the generated image an explicit lifetime and tracked DXVK image state/layout.
3. Acquire a distinct WSI image and matching acquire/present semaphore slot for the generated transaction. Never reuse the game transaction's acquired image or semaphores.
4. Extend Presenter state to represent more than one outstanding acquired/present transaction, or introduce a serialized second transaction that uses a separately acquired image and its own semaphore slot. The implementation must respect the platform's swapchain image count and WSI restrictions.
5. Queue the generated blit/submit/present through DXVK's existing submission queue, with dependencies expressed through DXVK command lists and semaphore tracking.
6. Ensure the next game frame cannot overwrite the history/source resources while the generated command list still references them. Use DXVK resource tracking and retained references; do not add CPU/GPU waits.
7. Preserve frame IDs and latency tracking semantics. Distinguish source-frame counters from generated-present counters; WSI present acceptance is not proof of physical scanout.
8. On resize, surface loss, swapchain recreation, device loss, and presenter teardown, retire generated transactions and invalidate temporal history safely.

## Current safe state

- SnapFrame compute stages produce owned midpoint output on the DXVK command list.
- The source image is copied into owned history because swapchain/backbuffer images can be reused.
- No manual `vkQueueSubmit`, `vkQueuePresentKHR`, `vkDeviceWaitIdle`, `vkWaitForFences`, readback, or explicit queue flush is performed by the SnapFrame host.
- The current hook does not claim a generated frame was presented.
- DXVK 2.6.2 must not be selected as a complete runtime until its matching Android `libmain_262.so` and native build/test path are available.

## Next implementation gate

Before changing Presenter code, design and test a small transaction model for (a) acquired image, (b) acquire semaphore, (c) command-list submission, (d) present semaphore, (e) present completion/reuse. The model must prove no image or binary semaphore is reused while still owned by WSI. Then implement the model inside DXVK's submission/Presenter layer and add tests for normal, delayed, out-of-date, resize, and teardown paths. Do not enable generated presentation before those invariants hold.


## Executable transaction ownership contract

A small executable reference model now lives at
`tools/tests/test_snapframe_presenter_transaction_model.py`. It is deliberately
not wired into Vulkan or DXVK. Its purpose is to make the minimum slot-ownership
rules testable before touching Presenter internals.

Each slot represents one WSI image together with its acquire and present
binary semaphores. Its only permitted lifecycle is:

`FREE -> ACQUIRED -> SUBMITTED -> PRESENT_QUEUED -> FREE`

Rules enforced by the model:

- A slot cannot be acquired while its previous transaction is live.
- The game and generated transaction must use distinct slot identities.
- A present cannot be queued before the command-list submission stage.
- A slot cannot be recycled merely because present was queued; its owner must
  observe the completion/reuse condition first.
- Duplicate completion and invalid transitions fail closed.
- A swapchain-generation replacement is a new pool; it must not silently
  reset still-live slots from the old generation.

The model tests normal independent transactions, attempted early reuse,
invalid ordering, duplicate completion, resize/out-of-date retirement, and
minimum image-count validation.

Important limitation: this state machine is a safety contract only. It does
not prove Vulkan WSI completion semantics, DXVK queue ordering, or Android
surface behavior. The real Presenter integration must map each transition to
the exact upstream acquire, command submission, present, and completion
signals at the pinned revision. In particular, a successful
`vkQueuePresentKHR` return is not by itself treated as physical scanout or
as permission to recycle every object referenced by the transaction.


## Pinned Presenter source review: serialized double-present is not yet safe

At pinned DXVK revision `9d6f54a1ade20d1d27dd421024717a636f3d8c68`,
the following implementation details were checked directly:

- `Presenter::acquireNextImage` waits on `m_surfaceCond` until
  `m_presentPending` becomes false, then returns the currently acquired
  `m_imageIndex` and semaphore pair at `m_frameIndex`.
- `Presenter::presentImage` calls `vkQueuePresentKHR`, advances
  `m_frameIndex` on non-negative status, and on exact success immediately
  calls `waitForSwapchainFence(nextSync)` followed by another
  `vkAcquireNextImageKHR`.
- The present path then clears `m_presentPending` and wakes acquire waiters.
- `waitForSwapchainFence` may call `vkWaitForFences(..., ~0ull)`; it is a
  real CPU-side wait, not a non-blocking slot poll.
- `DxvkDevice::presentImage` enqueues a present operation on
  `DxvkSubmissionQueue`; it does not synchronously complete the WSI present.
- `DxvkContext::flushCommandList` enqueues a command-list submission and
  immediately begins a fresh recording command list.

This confirms that a naïve second call from the current D3D9 CS closure is not
acceptable. It may wait on `m_presentPending` or a WSI fence from the CS thread,
and it can disturb source-frame latency tracking. Merely observing that the
Presenter advances its semaphore ring does not make a second present
non-blocking or correctly associated with its own frame ID.

### Required next change in DXVK (not in SnapFrame host)

The next implementation must move generated-frame scheduling into
`DxvkSubmissionQueue` / `Presenter` as an asynchronous queued operation.
It must carry an immutable transaction record containing at least:

- swapchain generation;
- acquired WSI image index and retained image reference;
- exact acquire and present semaphore pair;
- generated/source frame IDs and present mode snapshot;
- completion primitive / reuse state for that slot;
- retained generated-image reference until the command list and present
  operation no longer reference it.

The scheduler must not call the public blocking `acquireNextImage` from the
CS thread. It must also avoid `waitForSwapchainFence` on the source-frame
critical path. If no free WSI slot is available, it should skip that generated
frame and preserve the ordinary game present.

The source present and generated present must be ordered by the existing
submission queue, while their independent WSI transactions retain independent
image/semaphore ownership. Completion, not present enqueue, retires a slot.
The implementation must explicitly handle swapchain generation changes and
all non-success present/acquire results.

The Android patcher now verifies the pinned single-transaction Presenter
assumptions and fails closed if upstream changes those anchors. This is a
compatibility guard, not a generated-present implementation.

## Expanded transaction model coverage

The reference model has been extended to cover scheduler failure and lifecycle
cases without changing the production Presenter path:

- immutable transaction tokens carry swapchain generation, image index, and a
  per-slot serial, so stale callbacks cannot complete a later reuse;
- acquisition is non-blocking in the model and returns no token when all slots
  are owned, representing the required "skip generated frame" behavior;
- resize creates a new generation while old-generation slots remain retained
  until their outstanding present transactions complete;
- OUT_OF_DATE and SURFACE_LOST retire the affected generation;
- DEVICE_LOST abandons all handles as terminal teardown and never makes them
  available for reuse;
- completion is distinct from present enqueue, and duplicate/stale completion
  is rejected.

These tests remain a reference contract. In particular, the model's
completion callback must eventually be mapped to a real DXVK/Vulkan completion
signal; a return from `vkQueuePresentKHR` alone is not sufficient. The
production generated-present path remains disabled.
