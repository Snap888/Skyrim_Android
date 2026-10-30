> **Актуализация статуса:** этот файл содержит исторические заметки и отдельные более ранние runtime checkpoints. Для текущего состояния исходников, последовательности DXVK dispatch, карты файлов, roadmap и актуальных инструкций тестирования см. [docs/README.md](README.md). Не трактуйте старые строки «Compute pipeline/descriptors/dispatch — Not implemented» как состояние текущего HEAD: последующие commits добавили mipmaps, Alpha/Beta, Gamma/Delta/Generate и generated-frame presentation в DXVK path. Исполнение на Android GPU всё ещё требует повторного подтверждения актуальным APK и runtime log.

# LSFG implementation status (`snap-patch-3`)

## Goal

In-process Lossless Scaling Frame Generation on the Android Vulkan path after DXVK.
The user's `Lossless.dll` is treated as a shader-data source; it is not loaded as a
Windows DLL on Android. No proprietary DLL or extracted shader payload is committed.

## Current verified implementation

| Stage | Location | Status |
|---|---|---|
| Host config/bootstrap | `android/app/jni/lsfg/lsfg_host.*`, `runtime/lsfg_bootstrap.c` | Implemented scaffold |
| DXVK present integration | `android/app/jni/lsfg/lsfg_present.cpp` | Receives present notifications |
| RCDATA extraction | `android/app/jni/lsfg/lsfg_extract.cpp` | Implemented; runtime log reported 300 resources extracted |
| DXBC to SPIR-V translation | `android/app/jni/lsfg/lsfg_translate.cpp` | Implemented; runtime log reported 300 modules processed |
| CPU-side SPIR-V reflection | `android/app/jni/lsfg/lsfg_step3d.cpp` | Descriptor metadata and workgroup-size audit; no GPU pipelines |
| Reflection metadata export | `lsfg_reflection.tsv` in the runtime shader-cache directory | CPU-only TSV export of shader entry point/workgroup size and reflected descriptor set/binding/type/count; no shader bytecode |
| History image allocation | `android/app/jni/lsfg/lsfg_step3d.cpp` | Two RGBA8 images allocated |
| Game-frame history copies | `android/app/jni/lsfg/lsfg_step3g.cpp` | LSFG diagnostics/history path; separate from the blitter prototype |
| Experimental temporal blend | `android/app/jni/dxvk/src/dxvk/dxvk_swapchain_blitter.cpp` | Previous single-sample game image is sampled and blended 50/50 over the current present image; current image is then copied into persistent DXVK-managed history |
| Completion observation | `dxvk_context.*`, `dxvk_cmdlist.*`, `dxvk_queue.cpp` | Fence completion callbacks wired |
| Compute pipeline/descriptors/dispatch | — | **Not implemented** |
| Additional WSI presentation / generated-frame pacing | — | **Not implemented** |

The runtime log previously supplied for this branch reported 300 successful CPU
reflection results, history-copy completion callbacks, and no reported copy
failures. These observations do not establish that the copied pixel contents are
correct. The same log explicitly reported that no GPU pipelines were created,
no descriptors were bound, and no compute dispatch or generated output occurred.

The reflection pass also writes `lsfg_reflection.tsv` beside the runtime SPIR-V
cache. It contains `shader` and `binding` rows, plus extended `image`,
`ubo_member`, `ssbo_member`, `push_member`, and `array_stride` rows where
the declarations are present. Image rows expose the SPIR-V `OpTypeImage`
operands (dimension, depth, arrayed, multisample, sampled mode, and format);
buffer-member rows expose declared member offsets and matrix strides.
Specialization-dependent values are not evaluated, and these declarations do
not prove the runtime image view format, image usage, descriptor contents, or
host-side buffer packing. This remains offline metadata, not authorization to
create or dispatch pipelines.

## Recovered LSFG execution chain

The recovered 3.1 pass graph, resource topology, generation timestamps, and remaining integration gates are documented in [LSFG_CHAIN_RECOVERY.md](LSFG_CHAIN_RECOVERY.md). This advances the graph reconstruction beyond static shader reflection, but does not yet implement or validate compute dispatch.

## Shader variant index map

The candidate base/+49/+98 archive ID correspondence for the recovered chain shaders is recorded in [LSFG_VARIANT_MAP.md](LSFG_VARIANT_MAP.md). Treat it as an index hypothesis, not proof of equivalent shader behavior or runtime variant selection. The descriptor families remain separate.

## Shader archive audit

A static audit of the supplied archive is recorded in [`LSFG_SHADER_AUDIT.md`](LSFG_SHADER_AUDIT.md). It distinguishes 202 DXBC-derived modules from 98 original SPIR-V modules and documents their incompatible descriptor-binding conventions. This metadata is not yet a recovered execution graph; no dispatch should be inferred from it alone.

## Experimental present-path blend (prototype)

The D3D9 swapchain blitter now contains a first visual experiment that reuses
DXVK's existing presentation fragment shader and fixed-function alpha blending.
For single-sample source images it:
1. draws the current game image to the acquired WSI image;
2. if a compatible history image exists, overlays the previous game image at
   50% constant alpha;
3. records a copy of the current source image into a persistent, sampled
   history image through `DxvkContext::copyImage()`.

The first frame and the first frame after a source extent/format change are
presented without blending. Multisampled sources bypass the blend and invalidate
the temporal history. Old history resources are released through DXVK reference
ownership rather than an immediate Vulkan device-idle wait.

**Important limitation:** this is temporal frame blending, not yet true 2x frame
generation. It produces one blended presentation for each game frame; it does
not insert an additional WSI present between game frames, estimate motion, or
use the 300 extracted LSFG shaders. Ghosting is expected in moving scenes.
The prototype has not been compiled or tested on an Android device. It must be
treated as experimental until an APK build and runtime visual validation pass.

## DXVK completion callback audit

`DxvkContext::addCompletionCallback` appends a callback to the active command
list. The submission queue's finish thread waits for the command-list fence,
calls `notifyCompletion(status)`, then notifies signals, resets the command list,
and recycles it. This is the correct point for CPU-side completion accounting;
callbacks must not record more commands into the completed command list.

The callback proves command-list retirement status, not image pixel validity.
Keep callback captures small and independent of transient stack state. Do not
submit Vulkan work or wait for the device from the callback.

## Resize and resource lifetime caveat

`lsfg_step3d.cpp:create_fb()` currently calls `vkDeviceWaitIdle()` before
destroying history images on a size change. This avoids freeing images that may
still be referenced by in-flight DXVK command lists, but can stall the caller
during resize. It must not be replaced by immediate destruction or by clearing
the persistent wrappers alone: DXVK command lists may still reference the
external VkImages and their backing memory.

A future non-blocking resize implementation needs explicit deferred retirement
of each old image, image view, and allocation until every command list that
references that generation has completed. The wrapper's shared ownership alone
does not own/destroy the externally-created VkImage or its VkDeviceMemory.

**Present-thread caveat:** `lsfg_step3d_run()` is called synchronously from
`lsfg_host_on_present_ex()`. Therefore the current `vkDeviceWaitIdle()` on a
history-size change is in the present-hook call path and can cause a visible
resize hitch. It is not executed on every frame, but it is not an asynchronous
resize. Do not remove the wait until deferred retirement is implemented; doing
so would risk destroying images still referenced by in-flight DXVK command
lists.


## Next implementation gates

1. Keep the existing CPU-only path stable; do not compile hundreds of pipelines
   from the present hook.
2. Extend reflection to capture names, member offsets, push-constant blocks,
   image dimensions/formats, and specialization-dependent metadata. Preserve
   unknown values rather than guessing.
3. Correlate shader metadata with the actual LSFG pass/resource graph. A list of
   shader modules or symbolic role names alone is not sufficient to infer
   dispatch order or resource bindings.
4. Create pipelines incrementally on a controlled path, using the exact
   descriptor layouts and push constants established by reflection.
5. Record dispatches and barriers through DXVK-managed command recording and
   lifetime tracking. Do not call an independent `vkQueueSubmit` from present.
6. Add a verified output path and frame pacing only after the compute graph is
   correct and GPU output can be validated.

## Build and validation policy

No CI workflow, APK build, release, or device test is implied by this document.
Do not commit `Lossless.dll`, extracted proprietary resources, or runtime shader
cache files.

## Latest Android runtime observation (2026-10-30)

The latest user-supplied runtime log reports:

- 300/300 compute SPIR-V modules passed CPU reflection; maximum reflected descriptor count was 20.
- The two DXVK history slots were created at 2448×1080.
- History-copy command lists completed through copy #5400, with completion counters matching the submitted copy count at the sampled checkpoints (#1, #60, #300, #1800, #3600, and #5400).
- The log does not report a copy failure in the supplied excerpt.
- The same run still reports `CPU reflection only; no GPU pipelines created, no descriptors bound, no dispatch submitted` and `generated_frames=0`.

This is stronger evidence that the DXVK command-list copy path remains active over a long session. It is not evidence that history pixels are correct, that any LSFG compute pass ran, or that an interpolated frame was produced. The next blocker is unchanged: recover the Windows runtime's actual shader/pass selection, resource bindings, constant-buffer contents, and dispatch sequence. Until those values are known, enabling guessed compute dispatches would be unsafe and would not constitute a correct LSFG implementation.


### Runtime reflection variant observation (2026-10-30 19:44)

The latest user-supplied excerpt expands the visible copied-SPIR-V reflection through IDs 348–400. It shows matching workgroup/interface signatures for candidate pairs 348↔397, 349↔398, 350↔399, and 351↔400, while also showing distinct layouts and workgroup sizes across other IDs. The details and limitations are recorded in [LSFG_VARIANT_MAP.md](LSFG_VARIANT_MAP.md). Matching reflection signatures are not proof of byte identity or runtime selection.


### Runtime update (2026-10-30 20:25)

A later user-supplied log extends reflection observations through copied-SPIR-V IDs 388–400. It records distinct Alpha, Delta, and Beta candidate interfaces; the detailed per-ID table and arithmetic candidate comparisons are in [LSFG_VARIANT_MAP.md](LSFG_VARIANT_MAP.md). The same excerpt reports 300/300 successful CPU reflections and, at copy #1800, 1800 aggregate DXVK command-list completions with 900 copies per history slot. It still reports no GPU pipeline creation, descriptor binding, compute dispatch, or generated frames. These results validate neither history pixel contents nor shader equivalence/runtime selection.


## snap-patch-2 experiment review (2026-10-30)

The older `snap-patch-2` branch was reviewed for reusable implementation work. Its
`lsfg_step3g.cpp` contains a WSI injection helper that wraps a generated
`VkImage` as a DXVK image and records `DxvkContext::copyImage()` into the
presentation command stream. The idea of doing final output composition inside
the existing DXVK command list is sound, but the helper is not a complete
frame-generation implementation: it assumes a generated image already exists,
does not establish that image's producer/consumer synchronization, and only
copies a compatible RGBA8 region. The current `snap-patch-3` tree does not
contain the `step3hj` family or that injection hook, so no unverified code was
blindly copied.

The older branch's asynchronous `step3hj` experiment also uses a direct
`vkQueueSubmit` path for independently recorded work. That path is not adopted:
it can race DXVK's queued history copies and bypasses DXVK command-list resource
tracking. Any future async compute must be integrated with DXVK's own command
submission/lifetime model, not added as a second submit stream from present.

The old branch's `step3hg` pass map and `step3he` resource experiments are
useful research references, not runtime proof. Their pass order and resource
semantics must be reconciled against the recovered chain, actual SPIR-V
instructions, descriptor layouts, image formats/usages, and constant-buffer
layouts before implementation.

### Current next implementation gate

The offline comparator `tools/compare_lsfg_spirv.py` now compares the
candidate base/+98 module pairs without modifying or committing shader cache
contents. Use it on the user's runtime cache first. After binary comparison,
the next required artifact is a per-shader metadata dump that includes complete
SPIR-V types, image formats, descriptor arrays, push constants, and UBO member
offsets. Only then should DXVK compute pipeline/layout creation and dispatch be
implemented. The current branch still has no LSFG compute dispatch or generated
frame output.


### Runtime update (2026-10-30 21:20)

The latest supplied run reports 300/300 CPU reflection successes and both
history slots active at 2448×1080. At the sampled checkpoints, command-list
completion counters match the history-copy count through #1800 (900 copies per
slot). It continues to report `generated_frames=0`, with no GPU pipelines,
descriptor sets, or dispatches. This confirms sustained command-list retirement,
not history pixel correctness or generated-frame output.

The CPU reflection path now additionally writes `lsfg_reflection.tsv` into the
runtime shader-cache directory. The TSV exports each compute module's entry
point and known workgroup size, plus descriptor set/binding/type/count rows.
This is an offline-analysis aid; its current schema does not yet expose image
formats/dimensions, UBO member layouts, push constants, or specialization
values. The code change has been committed, but no Android build or device
validation has been run for it.


### Extended reflection update (2026-10-30)

The CPU metadata exporter now also records `OpTypeImage` declarations,
uniform/storage/push-constant struct member offsets and matrix strides, and
`ArrayStride` decorations. It unwraps `OpTypeSampledImage` to associate
combined-image-sampler descriptors with their underlying image declaration.
This is still a partial SPIR-V layout audit: it does not calculate complete
nested type sizes, member scalar/vector/matrix types, runtime image dimensions,
actual VkImageView formats/usages, descriptor writes, or host-side constant
buffer values. The change has been committed but has not been compiled or
validated against the user's runtime cache/device.
