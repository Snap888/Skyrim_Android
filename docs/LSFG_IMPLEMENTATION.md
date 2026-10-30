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
| History image allocation | `android/app/jni/lsfg/lsfg_step3d.cpp` | Two RGBA8 images allocated |
| Game-frame history copies | `android/app/jni/lsfg/lsfg_step3g.cpp` | Copies recorded through `DxvkContext::copyImage()` |
| Completion observation | `dxvk_context.*`, `dxvk_cmdlist.*`, `dxvk_queue.cpp` | Fence completion callbacks wired |
| Compute pipeline/descriptors/dispatch | — | **Not implemented** |
| Generated-frame output and pacing | — | **Not implemented** |

The runtime log previously supplied for this branch reported 300 successful CPU
reflection results, history-copy completion callbacks, and no reported copy
failures. These observations do not establish that the copied pixel contents are
correct. The same log explicitly reported that no GPU pipelines were created,
no descriptors were bound, and no compute dispatch or generated output occurred.

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
