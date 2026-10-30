# LSFG shader variant index map

Date: 2026-10-30
Branch: `snap-patch-3`

## Purpose

This note correlates the logical shader IDs in the recovered LSFG 3.1 chain with the three archive ID ranges. It is an **index map**, not proof that the three entries are byte-identical or behaviorally interchangeable.

The extraction code preserves the PE RCDATA resource ID in `rcdata_<id>.bin`. The translation code also preserves that ID in `<id>.spv`: DXBC resources are compiled, while resources detected as SPIR-V are copied after basic magic/size validation. Neither stage renumbers IDs.

The archive audit records:
- IDs 101–302: 202 DXBC resources, translated to SPIR-V.
- IDs 303–400: 98 resources already encoded as SPIR-V, copied unchanged.

## Candidate index correspondence

For the chain shader IDs, the archive layout gives candidate companions at base ID + 49 and base ID + 98:

| Logical stage family | Base ID | Candidate +49 | Candidate +98 |
|---|---:|---:|---:|
| Mipmaps | 255 | 304 | 353 |
| Generate | 256 | 305 | 354 |
| Gamma / Delta shared | 280 | 329 | 378 |
| Delta | 281 | 330 | 379 |
| Gamma | 282 | 331 | 380 |
| Gamma | 283 | 332 | 381 |
| Gamma | 284 | 333 | 382 |
| Gamma | 285 | 334 | 383 |
| Delta | 286 | 335 | 384 |
| Gamma / Delta | 287 | 336 | 385 |
| Gamma / Delta | 288 | 337 | 386 |
| Delta | 289 | 338 | 387 |
| Alpha | 290 | 339 | 388 |
| Alpha | 291 | 340 | 389 |
| Alpha | 292 | 341 | 390 |
| Alpha | 293 | 342 | 391 |
| Delta | 294 | 343 | 392 |
| Delta | 295 | 344 | 393 |
| Delta | 296 | 345 | 394 |
| Delta | 297 | 346 | 395 |
| Beta | 298 | 347 | 396 |
| Beta | 299 | 348 | 397 |
| Beta | 300 | 349 | 398 |
| Beta | 301 | 350 | 399 |
| Beta | 302 | 351 | 400 |

The stage labels above follow the recovered logical chain in [LSFG_CHAIN_RECOVERY.md](LSFG_CHAIN_RECOVERY.md). They describe the base shader's role in that reference graph; they do not establish the role or runtime selection of either candidate companion.

## What the user's manifest confirms

The supplied `manifest_spirv` excerpt reports:
- IDs 101–302 as `ok dxbc`;
- IDs 303–400 as `ok spirv-copy`.

This confirms successful translation/copy status as recorded by that run. It does not prove that every candidate +49/+98 module is a runtime-selected variant of the corresponding base ID.

## Required validation before selecting a variant

For each candidate triple, compare:
1. SPIR-V instruction streams after normalizing non-semantic/debug instructions.
2. `OpEntryPoint`, `OpExecutionMode`, and `LocalSize`.
3. Descriptor set/binding decorations and resource names.
4. `OpTypeImage` sampled/storage mode, format, and dimensionality.
5. Push-constant and uniform-buffer member offsets/types.
6. Specialization constants and capabilities/extensions.
7. Runtime shader creation/selection evidence from the Windows implementation or a trustworthy capture.

A matching interface is not proof of matching shader behavior. A matching hash is proof only of byte identity, not of runtime selection. The final registry must therefore record the selected variant and the evidence for that selection separately.

## Implementation consequence

Do not build one pipeline per arithmetic triple and do not dispatch all three variants. Keep the families separate:
- translated DXBC SPIR-V uses DXVK's translated binding convention;
- copied SPIR-V retains the embedded HLSL-style bindings (including bindings 0, 16/17, 32+, and 48+ where present).

The current Android branch still has no LSFG compute pipeline, descriptor binding, or dispatch. No generated frames are produced. This map is documentation for the next static comparison step only.


## Runtime reflection evidence: IDs 348–400 (2026-10-30 19:44)

The user supplied a runtime reflection excerpt for IDs 348–400. All listed modules reported `entry=main` and `reflection OK`. The following structural patterns are directly visible in that log:

| Candidate group | IDs | Workgroup | Reflected interface |
|---|---|---|---|
| Beta candidate set for base 299–301, +49 | 348–350 | 8×8×1 | sampler b16; sampled b32–b33; storage b48–b49 |
| Beta candidate set for base 299–301, +98 | 397–399 | 8×8×1 | sampler b16; sampled b32–b33; storage b48–b49 |
| Beta candidate for base 302, +49 | 351 | 32×32×1 | UBO b0; sampler b16; sampled b32–b33; storage b48–b53 |
| Beta candidate for base 302, +98 | 400 | 32×32×1 | UBO b0; sampler b16; sampled b32–b33; storage b48–b53 |

This is useful evidence that the +49 and +98 candidates in these specific groups have matching reflected workgroup sizes and descriptor interfaces. It does **not** establish identical SPIR-V instructions, image formats, UBO layouts/values, or runtime selection. In particular, the different interfaces between IDs 348–350 and 351 show that even adjacent IDs within the same logical Beta family are not one universal pipeline layout.

The wider excerpt also shows substantial interface variation among the copied modules:
- ID 353: 32×32×1, UBO b0, sampler b16, sampled b32, storage b48–b54.
- ID 354: 16×16×1, UBO b0, samplers b16/b17, sampled b32–b36, storage b48.
- IDs 355–356: 8×8×1, UBO b0, samplers b16/b17, many sampled resources, and multiple storage outputs.
- ID 373: 8×8×1, sampler b16, sampled b32–b43, storage b48–b49.
- IDs 377 and 400: 32×32×1; their exact binding sets differ (377 includes UBO b0, sampler b16, sampled b32/b33, storage b48–b53; 400 has the same visible shape).
- ID 378: 8×8×1, UBO b0, samplers b16/b17, sampled b32–b36, storage b48–b50.
- IDs 383, 387, and 395 include UBO b0 and sampler b16/b17 but have distinct sampled/storage counts.

The excerpt's summary again reports 300/300 reflection success. Runtime then allocated history at 2448×1080 and recorded copies to two persistent slots; at copy #60 and #300, completion counts matched the per-slot recorded counts. It still explicitly reports CPU reflection only, no GPU pipelines/descriptors/dispatch, and zero generated frames. The history log also says pixel contents have not been read back.

### Consequence

The next comparison should prioritize actual binary comparisons for the observed candidate pairs (348↔397, 349↔398, 350↔399, 351↔400), followed by full SPIR-V type/image/UBO reflection. Do not merge their layouts solely because the visible interfaces match. Preserve a distinct layout per reflected interface until the full metadata and runtime selection are known.


## Runtime reflection evidence: IDs 388–400 (2026-10-30 20:25)

The user supplied a later runtime excerpt covering copied-SPIR-V IDs 388–400. Every listed module reports `entry=main` and `reflection OK`. The visible descriptor interfaces are:

| ID | Candidate base role | Workgroup | Reflected interface |
|---:|---|---|---|
| 388 | Alpha 290 (+98) | 8×8×1 | sampler b16; sampled b32; storage b48 |
| 389 | Alpha 291 (+98) | 8×8×1 | sampler b16; sampled b32; storage b48 |
| 390 | Alpha 292 (+98) | 8×8×1 | sampler b16; sampled b32; storage b48–b49 |
| 391 | Alpha 293 (+98) | 8×8×1 | sampler b16; sampled b32–b33; storage b48–b49 |
| 392 | Delta 294 (+98) | 8×8×1 | sampler b16; sampled b32; storage b48 |
| 393 | Delta 295 (+98) | 8×8×1 | sampler b16; sampled b32; storage b48 |
| 394 | Delta 296 (+98) | 8×8×1 | sampler b16; sampled b32; storage b48 |
| 395 | Delta 297 (+98) | 8×8×1 | UBO b0; samplers b16/b17; sampled b32–b33; storage b48 |
| 396 | Beta 298 (+98) | 8×8×1 | sampler b16; sampled b32–b37; storage b48–b49 |
| 397–399 | Beta 299–301 (+98) | 8×8×1 | sampler b16; sampled b32–b33; storage b48–b49 |
| 400 | Beta 302 (+98) | 32×32×1 | UBO b0; sampler b16; sampled b32–b33; storage b48–b53 |

The role/base labels are arithmetic archive correspondences from the candidate map, not evidence that the Android runtime selected those modules for those passes. IDs 388–391 add the +98 Alpha candidates to the earlier +49 observations 339–342; IDs 392–396 similarly cover the later Delta/Beta candidate IDs. The table records only what the CPU reflection log exposes. It does not include image formats/dimensions, UBO member layout or contents, instruction equivalence, or runtime selection.

The 20:25 summary again reports 300 compute modules, 300 with bindings, and 300 successful reflections. The same run records two history slots, each receiving 900 copies by copy #1800, with the aggregate completion count reaching 1800. This is command-list completion evidence, not a pixel readback or proof of correct temporal contents. No GPU pipelines, descriptor sets, or compute dispatches were created; generated frame count remains zero.

### Updated comparison priority

The new +98 candidates make these comparisons useful next:
- Alpha: 339↔388, 340↔389, 341↔390, 342↔391.
- Delta: 343↔392, 344↔393, 345↔394, 346↔395.
- Beta: 347↔396, plus the previously noted 348↔397, 349↔398, 350↔399, 351↔400.

Matching binding signatures should be treated as layout clues only. Before pipeline creation, compare the actual SPIR-V instructions and complete type/format/UBO metadata, and establish which module the Windows runtime actually selects.


## Offline binary comparison tool

Use `tools/compare_lsfg_spirv.py` against a runtime shader-cache directory containing
files named `<resource-id>.spv`. The default comparisons cover the candidate
base/+98 pairs for Alpha, Delta, and Beta:

```sh
python3 tools/compare_lsfg_spirv.py /path/to/lsfg_spirv
```

To compare selected pairs only:

```sh
python3 tools/compare_lsfg_spirv.py /path/to/lsfg_spirv --pairs 339:388 340:389
```

If SPIRV-Tools is installed, the script also records disassembly-text hashes.
It does not modify the cache or upload/commit shader binaries. A byte-identical
pair is strong evidence that the files are the same module; differing hashes
show only that the binaries differ. Equal disassembly text is not proof of
runtime selection or algorithmic equivalence, and different disassembly text
does not by itself prove different visual behavior. Missing/invalid modules are
reported as unavailable rather than silently treated as unequal.
