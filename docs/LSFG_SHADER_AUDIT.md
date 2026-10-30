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

## Deeper reflection of original SPIR-V (IDs 303–400)

A second pass over the SPIR-V instruction streams inspected `OpName`, `OpMemberName`, `OpDecorate`, `OpMemberDecorate`, `OpTypePointer`, `OpTypeImage`, `OpTypeStruct`, `OpVariable`, and `OpExecutionMode`.

### Named descriptor contract

The 98 original modules expose the following stable naming convention where present:

- `Sampler` / `Sampler1`: separate sampler descriptors at bindings 16 / 17.
- `Input1` … `InputN`: sampled images starting at binding 32.
- `Output1` … `OutputN`: storage images starting at binding 48.
- Mipmap-producing variants expose `OutputMip0` … `OutputMip5` at bindings 48–53. Some seven-output variants expose bindings 48–54.
- `cb`: a uniform-buffer struct at binding 0.

This confirms that the embedded-SPIR-V family is not just a different numeric binding map: its resource names provide semantic *roles* for each descriptor. The names still do not reveal which runtime image is assigned to each input or output.

### Constant-buffer structure

The embedded modules with a `cb` uniform buffer use SPIR-V struct member decorations with byte offsets. The observed common layouts include:

- Seven members at offsets 0, 8, 12, 16, 20, 24, 28.
- Eight members at offsets 0, 8, 12, 16, 20, 24, 28, 32.

Other seven-member variants retain the same offset pattern while changing member scalar/vector types. The first member is commonly a larger vector/aggregate type; several following members are scalar-sized fields; trailing members are image-dimension or scale-like values by shape only. Their exact semantic meaning must not be guessed from type IDs or offsets. A correct host-side constant-buffer writer still requires recovering the actual values written by the Windows runtime (or authoritative shader source / disassembly).

### What this adds to implementation planning

The embedded-SPIR-V family can support a future descriptor-set builder keyed by reflected names and binding decorations, with per-shader descriptor counts and workgroup sizes. It does **not** yet support an execution graph: producer/consumer image identity, image formats/extents, mip allocation, per-pass constants, and pass ordering remain unknown. The 202 DXBC-derived modules also require a separate reflection path because DXVK's translated binding numbers and resource conventions differ.
