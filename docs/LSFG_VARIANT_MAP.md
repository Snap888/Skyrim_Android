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
| Gamma / Alpha / Beta stage ID | 283 | 332 | 381 |
| Gamma / Alpha / Beta stage ID | 284 | 333 | 382 |
| Delta | 285 | 334 | 383 |
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
