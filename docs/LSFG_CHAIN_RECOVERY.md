# LSFG 3.1 execution-chain recovery

Date: 2026-10-30
Branch: `snap-patch-3`

## Why this document exists

The previous shader archive audit established that the supplied DLL contains 202 DXBC-derived modules and 98 precompiled SPIR-V modules, but did not establish the runtime pass graph. A public implementation-level reconstruction now provides a concrete reference for the LSFG 3.1 chain. This document records the recovered graph as an integration specification; it does not copy implementation code.

Reference material inspected:
- WinNative frame-generation design: https://github.com/WinNative-Emu/WinNative/blob/main/docs/lsfg-frame-generation.md
- WinNative LSFG chain source at revision `72d145bccff348f71b1c8baf90a180ccaa474464`: `lsfg_chain.cpp`, `lsfg_chain.hpp`, `lsfg_mipmaps.cpp`, `lsfg_alpha.cpp`, `lsfg_beta.cpp`, `lsfg_gamma.cpp`, `lsfg_delta.cpp`, `lsfg_generate.cpp`.
- The referenced implementation files carry GPL-3.0-or-later notices. They were inspected as technical reference; their source code is not copied into this repository. Before porting any implementation code, verify the destination repository's licensing and preserve all required notices. Prefer a clean-room implementation from the documented behavior if compatibility is unclear.

## Recovered high-level graph

The shared work runs once for each newly accepted real frame pair; generation-specific work runs for each requested intermediate timestamp.

1. **Input history** — two frame images form the input pair. The reference chain also maintains three history slots for recurrence/history-dependent passes.
2. **Mipmaps / luma pyramid** — shader 255, seven flow-scale levels. Dispatch tile is 64x64; output format in the reference implementation is R8_UNORM.
3. **Alpha / coarse flow** — shaders 290–293, four stages at each of seven pyramid levels. The first stages create temporary images; the final stage uses three history slots. The reference dispatch tile is 8x8.
4. **Beta / flow refinement** — shaders 298–302, five stages. The first stage consumes six sampled images and writes two storage images; the final stage writes six flow outputs. First four stages use 8x8 groups; final output uses 32x32 groups.
5. **Gamma / per-generation motion/confidence** — shaders 280, 282–285, five stages per generation slot and pyramid level. It consumes the frame history, alpha outputs, beta flow, and previous gamma state. Output format is RGBA16F in the reference.
6. **Delta / occlusion and warp detail** — shaders 280, 286–289, 281, 294–297, ten stages on pyramid levels 4–6. Gamma and delta stages are interleaved per step where both have work. Outputs two RGBA16F detail/motion images.
7. **Generate** — shader 256. One dispatch per generated timestamp. It samples both input frames, motion, and two detail images, and writes the interpolated frame into a storage-capable target image. Dispatch tile is 16x16.

The chain's shared pass order is: prepare dummy resources -> mipmaps -> alpha stages (each stage across all seven levels) -> beta.
For each generation timestamp, levels are processed coarse-to-fine. On levels without delta, gamma runs by itself. On levels 4–6, gamma and delta stages are interleaved step-by-step. Generate runs last.

## Confirmed resource and timing facts from the reference

- Three history slots are used by recurrence-dependent passes.
- Seven pyramid levels are used.
- Delta is enabled on pyramid levels 4, 5, and 6.
- The frame-pair input ring has two images.
- Generation timestamps are fractional positions between the two real frames; the reference formula is (generation index + 1) / (generation count + 1).
- Flow image format: R8_UNORM.
- Motion/detail outputs: R16G16B16A16_SFLOAT.
- Default frame/pyramid input format in the reference: R8G8B8A8_UNORM.
- The reference implementation uses flow scale to size the pyramid, independently of output extent.
- The generated target must support storage writes; the Android swapchain image must not be assumed to support storage usage.

## Mapping to this repository's archive

The existing archive has:
- DXBC base resources, including the shader IDs listed above.
- Original SPIR-V variants at base ID + 49 and base ID + 98 for many chain shaders, according to the external reconstruction. In the supplied archive this corresponds to the ID ranges already recorded in `LSFG_SHADER_AUDIT.md`.
- A second translated-DXBC SPIR-V family whose binding numbers follow DXVK's translated convention.

Do not mix descriptor conventions. The existing CPU reflection results are useful for pipeline-layout construction, but descriptor counts and symbolic names alone do not assign concrete images to descriptors.

## What is still not recovered for this Android integration

The graph and broad resource topology are now known from a reference implementation, but this repository still needs its own verified implementation details:

1. Verify the exact DLL version and shader variant selection used by the supplied archive (FP16, FP32, or translated DXBC) and validate every selected module on the target Vulkan device.
2. Reconcile the reference descriptor ordering with each actual SPIR-V module's set/binding decorations. Do not assume source-level append order equals binding order without checking.
3. Recover and implement the exact constant-buffer byte layout and values for each pass/timestamp. Existing reflection found multiple CB signatures; guessing semantics is prohibited.
4. Verify image formats, dimensions, usage flags, mip/array interpretation, sampler modes, and dummy-image initialization against the actual shader modules and runtime behavior.
5. Integrate all dispatches and image barriers into DXVK-managed command recording and image lifetime tracking. No independent queue submission from the present hook.
6. Implement a two-frame warm-up and one-frame presentation latency: generated frames belong between N-1 and N, so frame N cannot be shown before its intermediate frames.
7. Provide an owned offscreen storage-capable output image and a paced presentation path for generated frames. Do not write directly to the WSI image unless storage support is explicitly queried and enabled.
8. Validate pixel output and frame pacing on device before claiming frame generation works.

## Immediate next implementation sequence

1. Add a host-side shader registry for the exact 25 logical chain stages, with explicit selected resource IDs and per-module reflected descriptor metadata.
2. Extend reflection to retain names, member offsets/types, image dimensions/formats, and specialization information in machine-readable manifests.
3. Implement per-pass descriptor-layout validation and resource-topology allocation, initially without dispatch.
4. Implement and test constant-buffer serialization from verified source data.
5. Record the shared chain and one generation into a DXVK command list using the existing DXVK context APIs and tracked resources.
6. Add offscreen generated-frame output and pacing only after the first compute output is validated.

## Status

This is a major recovery of the pass graph, not a completed port. The current branch still has no valid LSFG compute dispatch and reports `generated_frames=0`. The temporal blend prototype remains unrelated to the LSFG chain and must not be used as evidence of LSFG correctness.
