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

## Pinned DXVK submission queue: implementation-level findings

The queue implementation in this revision is `src/dxvk/dxvk_queue.h` and
`src/dxvk/dxvk_queue.cpp` (not `dxvk_submission_queue.*`). The relevant
objects and thread boundaries were checked directly:

- `DxvkSubmissionQueue::submit` appends a command-list entry to
  `m_submitQueue` under `m_mutex`.
- `DxvkSubmissionQueue::present` appends a present entry to that same FIFO
  under the same mutex. It does not call `vkQueuePresentKHR` on the caller.
- `submitCmdLists` is the submit worker. It drains FIFO entries and holds
  `m_mutexQueue` while submitting a command list or calling
  `Presenter::presentImage`. This serializes DXVK operations on the graphics
  VkQueue.
- A command-list entry is submitted through `DxvkCommandList::submit` and
  moved to `m_finishQueue` with its timeline values.
- `finishCmdLists` is a separate finish worker. For command-list entries it
  waits on DXVK timeline semaphores before notifying tracked objects and
  recycling the command list. For present entries it calls
  `Presenter::signalFrame` and drops the retained Presenter reference.
- `DxvkDevice::presentImage` is therefore an enqueue operation; the actual
  Presenter call happens later on the submit worker. The present operation
  follows the preceding command-list entry because both are appended to the
  same FIFO.

### Implications for the scheduler

The existing queue already provides a useful ordering primitive, but it is
not yet a generated-frame scheduler:

1. The current D3D9 closure calls `synchronizeWsi`, enqueues its command list
   with `flushCommandList`, then enqueues one present entry. A generated
   transaction must be inserted into this same ordered stream, not sent
   through a private Vulkan queue call.
2. `m_mutexQueue` protects DXVK's graphics VkQueue. New generated operations
   must use the same serialization domain; taking a second independent mutex
   around raw Vulkan queue calls would not preserve queue ownership.
3. `m_finishQueue` is the command-list retirement path. A generated image,
   its views, and its transaction record must remain retained until the
   generated command list has completed and its associated present completion
   policy has been satisfied. Enqueueing a present is not retirement.
4. The present worker currently calls `Presenter::presentImage`, whose
   implementation uses mutable singleton fields (`m_imageIndex`,
   `m_frameIndex`, `m_acquireStatus`, `m_presentPending`). A queued second
   present cannot safely use those fields without first introducing an
   immutable per-transaction Presenter API.
5. The existing `Presenter::acquireNextImage` is explicitly documented as
   potentially blocking. It cannot be called from the source CS closure for
   optional generated work. A future non-blocking generated acquire must use
   a distinct acquired image and semaphore pair, and treat no-ready-image as
   "skip this generated frame".
6. The finish worker's present path calls `signalFrame`; frame IDs and
   latency-tracker callbacks must not be conflated between source and
   generated presents. A separate generated-present accounting path is needed
   before claiming a higher displayed frame rate.

The Android patcher now checks the exact queue file names, FIFO append points,
submit/finish worker ownership, graphics-queue mutex, present dispatch and
finish retirement anchors, plus the D3D9 command-submit-before-present order.
It aborts if those pinned-revision assumptions change. This is a source
compatibility guard, not a scheduler implementation.

### Safe implementation boundary

Do not add a second `DxvkPresentInfo` entry alone: it contains only a
Presenter pointer and frame ID, and therefore has no immutable acquired-image
index, semaphore pair, swapchain generation, generated-image reference, or
completion token. The next production change must extend the queue entry and
Presenter transaction API together, with per-transaction ownership and a
non-blocking generated-image acquisition path. Until then the source present
continues unchanged and generated presentation stays disabled.


## Submit-worker WSI blocking gate

A focused read of the pinned Presenter::presentImage body confirms a
second, more immediate scheduling hazard. The call is made by
DxvkSubmissionQueue::submitCmdLists while its m_mutexQueue lock is held.
After vkQueuePresentKHR, the success path calls
waitForSwapchainFence(nextSync) and then
vkAcquireNextImageKHR with
std::numeric_limits<uint64_t>::max() as the timeout. This is not a
non-blocking poll.

Consequences:

- A generated-present entry placed behind a source present cannot execute
  until the source present's pre-acquire returns.
- A generated-present entry placed ahead of a source command submission can
  delay the source frame and violate the project's source-FPS priority.
- A second caller of the existing Presenter API still shares mutable
  m_imageIndex, m_frameIndex, and m_presentPending state; queue FIFO
  alone does not make that API transaction-safe.

The Android patcher now extracts the pinned Presenter::presentImage
function and fails closed unless the fence wait and infinite-timeout
pre-acquire anchors are present. This guard deliberately makes no claim that
the WSI operation is always blocked on every frame; it records that the API
permits a blocking wait and therefore cannot be used for optional generated
work on the source-critical path.

Implementation decision: do not add generated presents to the current
submit worker as a simple FIFO entry. The production scheduler needs a
non-blocking, separately owned WSI acquisition/present transaction and a
source-priority policy that cannot be held hostage by generated work. If the
platform/driver cannot provide a free generated slot without waiting, drop
that generated frame. The ordinary source present remains unchanged.


## Source-priority admission policy (reference model)

The current DXVK queue uses one submit worker and serializes graphics queue
operations with m_mutexQueue. A generated present cannot be given a hard
source-latency guarantee merely by putting it in a second software queue:
once a Vulkan queue operation is executing under the queue mutex, a newly
arriving source submission cannot preempt it.

The reference model now includes a source-priority admission policy:

- Do not admit generated work while any source command/present is pending.
- Permit at most one generated operation in flight; do not build a backlog of
  stale midpoint frames.
- If source work arrives before generated work is admitted, drop the generated
  frame immediately.
- Keep source submissions independent of generated completion accounting.
- Never describe this policy as preemption: it cannot interrupt a Vulkan or
  driver call that has already started.

This is a scheduler admission contract, not production Vulkan code. Before
enabling generated presentation, the DXVK implementation still needs a
non-blocking WSI path or a platform-validated mechanism that prevents
generated queue operations from holding the source-critical queue lock.
Until that exists, the production hook remains skip-only.


## Source-priority scheduling contract

The reference model now includes a bounded optional-work scheduler. Source
command submissions and source presents are always dequeued before generated
presents. Generated work has a fixed queue capacity; when it is full, new
generated frames are dropped instead of adding latency or building an
unbounded backlog. Continuous source work cannot be preempted by generated
work.

This policy is intentionally separate from the WSI slot-ownership model:
priority alone does not make Vulkan presentation non-blocking. The eventual
DXVK implementation must satisfy both contracts simultaneously:

- acquire a distinct WSI image and semaphore pair without waiting;
- enqueue only immutable generated transactions;
- keep source submit/present work ahead of generated work;
- ensure the generated transaction's submit/present path itself cannot block
the source queue;
- drop generated work if no immediately available WSI slot exists;
- retire the slot only from a real completion/reuse signal.

The scheduler tests validate policy only; they are not evidence of a live
DXVK worker or Android WSI implementation.


## Refined DXVK integration seam: WSI semaphores already travel with command lists

A review of the pinned command-list path found a concrete extension seam:

- `DxvkContext::synchronizeWsi(PresenterSync)` stores the acquire/present
  semaphore pair on the command list.
- `DxvkCommandList::submit` waits the WSI acquire semaphore on the first
  graphics submission and signals the WSI present semaphore on the final
  graphics submission.
- The command list then performs an extra timeline-only graphics submission
  so DXVK can safely retire the WSI semaphore use.

This means the WSI synchronization is already coupled to command-list
submission, rather than being hidden in the D3D9 frontend. A multi-transaction
implementation should preserve that mechanism but generalize the single
`m_wsiSemaphores` member into an explicit collection of immutable WSI
transaction records. Each record must bind one acquired image index to its
acquire semaphore, present semaphore, swapchain generation, and transaction
serial. The matching present queue entry must carry the same record; it must
not reconstruct ownership from mutable `m_frameIndex` or `m_imageIndex`.

### Candidate way to remove submit-worker blocking

The current `presentImage` success path waits the next slot's WSI fence and
performs an infinite-timeout `vkAcquireNextImageKHR` before returning to the
submit worker. A promising refactor is to stop pre-acquiring in
`presentImage`:

1. Submit and present the immutable transaction already acquired for this
   frame.
2. Retire the present operation through the existing completion mechanism.
3. Let the next ordinary source `acquireNextImage` perform its existing
   blocking acquire on the frontend/application thread when needed.
4. Add a separate generated acquire API that uses timeout `0` and returns
   immediately with `VK_NOT_READY` when no WSI image is available.
5. Keep the generated transaction optional and drop it on `VK_NOT_READY`.

This moves the potential WSI wait away from `dxvk-submit`, which is the
critical prerequisite for not delaying source command submissions. It is not
yet implemented: the single `m_presentPending`, `m_imageIndex`, and
`m_frameIndex` fields still need to become per-transaction ownership, and
swapchain recreation must retain old-generation transaction state until every
in-flight record is retired. The change must also preserve the exact binary
semaphore reuse rules on devices both with and without
`VK_EXT_swapchain_maintenance1` / present fences.

Do not remove the blocking pre-acquire as an isolated patch. Until the
per-transaction state and semaphore retirement are implemented together,
the current source present path remains untouched.


## Reference-model cleanup and non-blocking acquire outcomes

The transaction reference tests now model the result of an optional generated
acquire explicitly. A not-ready/timeout result drops the generated frame
without changing slot ownership; out-of-date and surface-lost retire the
current generation; device-lost abandons all device-owned slots. Additional
tests cover these paths.

The same update removes a duplicated scheduler implementation and duplicate
test class that had shadowed earlier source-priority tests in the Python
module. The remaining source-priority model is an admission-policy test only.
Neither model claims to exercise Vulkan or prove the Android driver's acquire
latency.


## First DXVK-owned immutable source transaction step

The Android port patcher now modifies the pinned DXVK 2.6.2 source so the successful source acquire is stamped with a transaction ID, swapchain generation, acquired image index, and semaphore-ring index. That exact PresenterSync record is copied into the command list and into the queued DxvkPresentInfo. The submission worker passes it to Presenter::presentImage, which validates the active token and uses the captured image index and semaphore-ring entry rather than reconstructing them from the current mutable indices.

This is intentionally a single-in-flight source transaction implementation. It does **not** yet permit multiple outstanding acquires, does not introduce a second generated present, and does not remove the submit worker's blocking pre-acquire/fence wait. The active-token validation and generation checks are a prerequisite for that later multi-slot refactor, not proof that it is safe today. Source present behavior and the existing semaphore retirement strategy remain the baseline.


## Android APK candidate packaging verification (2026-11-07)

The full APK workflow for commit
`221d0b7a17339e50a3af85eb1ad71e3674cc41a1` completed successfully in
GitHub Actions run `42351177321` (all eight jobs successful). The produced
artifact `Skyrim-0.21.913.apk` contains
`lib/arm64-v8a/libdxvk_d3d9_262.so` alongside the legacy
`lib/arm64-v8a/libdxvk_d3d9.so`.

The packaged 2.6.2 candidate was extracted from the APK and checked:
- ELF64, AArch64, Android API 28, shared object, stripped.
- SONAME: `libdxvk_d3d9_262.so`.
- SnapFrame hook exports are present:
  `snapframe_dxvk262_record_source`,
  `snapframe_dxvk262_set_enabled`, and
  `snapframe_dxvk262_get_enabled`.
- SHA-256 of the exact packaged DSO:
  `e70bc7478b16d772b441584ce05243f9403c6d2a5209523e30b7b2a069ec2708`.
- SHA-256 of the APK:
  `039010e9af8efb8d42b16e004e0294408154ba03cf9c930fe6c76a35ef367b92`.

This is packaging and ELF validation only. The APK does not contain
`lib/arm64-v8a/libmain_262.so`; the runtime pair is therefore incomplete,
and the 2.6.2 selector must remain unavailable. No device-side loading,
Vulkan initialization, source capture, or generated scanout is implied by
this CI result.


## DXVK 2.6.2 runtime ABI adapter (2026-11-07)

A dedicated `snapframe_runtime_262.cpp` now implements the public SnapFrame
host C ABI inside the candidate DSO. It is compiled alongside
`snapframe_host_262.cpp`; it does not include or link the legacy
DXVK 1.9.2b implementation. The Android overlay's D3D9 version-script export
list is extended for the complete runtime ABI so a matching `main_262`
can resolve it.

The adapter is deliberately fail-closed:
- Defaults to disabled, 50% flow scale, AUTO present mode, multiplier 1,
  performance quality, motion analysis on, and manual AI mode.
- Accepts only the implemented configuration: 50% flow scale, AUTO present
  mode, performance quality, motion analysis enabled, manual AI, no profiling
  or diagnostic suite. Enabling with multiplier 2 is rejected because no
  generated WSI present exists yet.
- Unsupported flow scales/present modes and unsupported enabled settings
  return `-ENOTSUP`; uninitialized controls return an error rather than
  reporting success.
- Telemetry returns `-ENODATA` and zero values until actual 2.6.2 telemetry
  is wired. Stats are zeroed, not fabricated.
- The compute hook now returns accepted (0) after recording compute work;
  early skips return non-zero. This return value is not a generated-frame
  counter and is not used to alter source-present priority.

The runtime pair build gate remains OFF. This adapter is an ABI implementation,
not evidence that `libmain_262.so` builds or that generated frames are
presented. The asynchronous WSI transaction scheduler remains the next gate.
