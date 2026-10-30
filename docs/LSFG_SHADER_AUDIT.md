# LSFG shader archive audit (2026-10-30)

This note records static inspection of the user-supplied `lsfg.zip` archive. The proprietary DLL and shader binaries are not copied into this repository.

## Archive inventory

- `Lossless.dll`: 7,521,280 bytes, PE32+ Windows x86-64. It is not loadable as an Android native library.
- 300 cached SPIR-V modules, IDs 101–400.
- IDs 101–302 (202 modules): translated from embedded DXBC by the existing extraction/translation path.
- IDs 303–400 (98 modules): already-SPIR-V RCDATA copied from the DLL; these are not outputs of DXBC translation.
- The earlier extraction log reports successful CPU reflection for 300/300 modules. This is reflection only, not proof that pipelines or dispatches are valid.

## Important distinction: two descriptor-layout families

The first family (IDs 101–302) uses the translated DXBC/DXVK binding convention. Representative decorations in module 101 include constant buffer binding 800, sampled images 832/833 and storage images 1088/1089. Other modules use the same high-numbered ranges with varying counts.

The embedded-SPIR-V family (IDs 303–400) retains named HLSL-style resources and a different binding convention. Static SPIR-V inspection found:
- constant buffer at binding 0;
- samplers at bindings 16 and 17;
- sampled input images beginning at binding 32;
- storage output images beginning at binding 48 (some passes expose several outputs, including mip outputs);
- names such as `Input1`, `Output1`, `OutputMip0`, and `cb`.

These are materially different descriptor layouts. A pipeline/layout builder must not apply one family's binding map to the other.

## Workgroup sizes in IDs 303–400

- 90 modules: 8×8×1
- 6 modules: 32×32×1
- 2 modules: 16×16×1

The modules include passes with multiple sampled inputs and multiple storage outputs. For example, modules 304/353 expose seven storage outputs, and modules 328/351/377/400 expose six named mip outputs. These facts constrain descriptor allocation and dispatch dimensions, but do not identify the producer/consumer ordering.

## What this audit does and does not establish

Established by the archive:
1. There are 202 DXBC-derived SPIR-V modules and 98 original SPIR-V modules.
2. The two groups use different descriptor binding conventions.
3. The original SPIR-V modules retain resource names and explicit local workgroup sizes.
4. Some passes write multiple outputs, including mip levels.

Not established:
1. The exact runtime pass order or which shader variant is selected for a given LSFG mode.
2. The semantic meaning, format, extent, lifetime, or aliasing rules for every input/output resource.
3. The complete constant-buffer layouts and the values the Windows runtime writes into them.
4. Synchronization dependencies between every pass.
5. Whether a guessed dispatch produces valid optical flow or an intermediate frame.

Therefore no shader is dispatched speculatively. The next implementation step is to recover the runtime's shader-selection/pass graph and constant-buffer population, then encode that graph as DXVK-context commands with explicit per-family descriptor layouts and resource transitions. The existing temporal blend remains only a visual smoke-test prototype, not LSFG frame generation.

## Reproducibility

The counts above were obtained by parsing SPIR-V instruction streams (entry points, execution modes, names, decorations, variables and image types) from the supplied archive. They are static metadata observations; no GPU execution or Android build was performed for this audit.
