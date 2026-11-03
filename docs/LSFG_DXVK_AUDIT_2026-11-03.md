# LSFG / DXVK integration audit — 2026-11-03

Branch: `snap-patch-5`

## Scope

Reviewed the project-owned LSFG implementation, its D3D9 Present integration, the DXVK swapchain blitter/presenter boundary, and the current implementation/status/optimization documents. This is a source-level audit; it is not an Android NDK build, Vulkan validation run, or device test.

## Change made

- Runtime LSFG enable/disable transitions now request a temporal warm-up reset.
- The request is atomic and consumed by the DXVK render-thread copy hook. The UI thread does not mutate render-thread temporal state directly.
- On transition, the warm-up counter, generation guard and history-copy mask are reset. History images are still copied through the active DXVK command list; the existing three-frame shared-pass warm-up remains in force.

## Important findings

1. **Shared passes execute on every active source Present.** The source history copy, mipmap dispatch and Alpha/Beta shared chain are currently recorded from `lsfg_step3g_copy_impl` before the source Present is submitted. The generation-count callback is currently called later, after the source Present submission. Therefore a simple zero-generation result in the existing callback cannot avoid the already-recorded Shared Passes.
2. **Do not skip Alpha/Beta without a temporal-state proof.** The shared chain maintains internal Alpha history slots and Beta outputs. Skipping source frames may leave those resources stale relative to the two external frame-history images. A safe optimization must establish how frame indices and internal temporal slots are expected to advance when no output frame is generated.
3. **Refresh-rate input is not universally available.** DXVK's D3D9 swapchain stores a display refresh rate and passes it to the presenter's FPS limiter, but the inspected code only guarantees a non-zero value when the display-mode path reports one. Windowed Android operation may have no reliable refresh-rate value. A Pacer must have a documented fallback and must not invent a target refresh rate.
4. **Present integration is single-thread-sensitive.** The copy and generation hooks are called from the D3D9 Present path and record commands into the active DXVK context. Keep resource changes on that path; do not introduce a separate Vulkan queue submission.
5. **Generation pass order is intentionally preserved.** Gamma/Delta remain interleaved at paired levels and then complete the remaining Delta-only stages. No pass reordering or shader math change was made.
6. **Profiler overhead is opt-in.** GPU timestamp query allocation, completion callbacks and CSV writes are guarded by the profiler setting. Keep ordinary gameplay free of profiler query work.

## Next implementation gates

- Define a Pacer input contract: monotonic source-Present timestamps, effective display refresh when genuinely known, VSync interval, and a fixed-multiplier fallback.
- Decide the generation budget before recording the shared chain, not after source submission.
- Validate temporal history semantics for skipped-generation frames before conditionally skipping Alpha/Beta.
- Add counters for requested, scheduled, successfully generated and fallback-presented frames; distinguish these from source simulation FPS.
- Add capability/format diagnostics against the already-created DXVK device rather than trying to enable Vulkan features after device creation.

## Verification state

- [x] Source-level review of LSFG host, history-copy path, shared Alpha/Beta path, Gamma/Delta/Generate path, and D3D9 Present hook.
- [x] Runtime enable/disable temporal warm-up reset implemented.
- [ ] Android NDK native build.
- [ ] APK packaging.
- [ ] Vulkan validation and device test.
- [ ] Controlled LSFG ON/OFF benchmark and visual motion test.

No performance improvement or runtime correctness on device is claimed until those tests are completed.
