# LSFG plan — snap-patch-5

- Repository: `Snap888/Skyrim_Android`
- Base branch: `snap-patch-4`
- Working branch: `snap-patch-5`
- Scope: LSFG / `Lossless.dll` Vulkan compute path only.
- Baseline commit: `c2a1077d631e552896b19da42e405579138e7a2b`

## Goals

1. Preserve the existing DXVK command-list architecture. All LSFG copies and compute passes must be recorded into the active `DxvkContext`; do not add a parallel `vkQueueSubmit` path or a speculative `hist-copy` shader.
2. Make shader-resource ingestion deterministic and safe: validate PE resource bounds, preserve original SPIR-V when supplied by Lossless.dll, and use DXBC-to-SPIR-V translation only for DXBC resources.
3. Validate shader interface requirements before creating DXVK compute shaders: descriptor set/binding layout, supported descriptor types, push-constant limits, and SPIR-V capabilities required by the target device.
4. Audit the real pass chain and its per-frame state: history copy → mipmaps (255) → Alpha (290–293) → Beta (298–302) → Gamma/Delta → Generate (256).
5. Ensure history and flow resources are invalidated on device/framebuffer generation, extent, effective flow-scale, or shader-cache changes. Never generate from stale history after a reset.
6. Keep dispatch failures fail-closed: a failed pipeline bind/dispatch must stop dependent passes and must not produce or count a generated frame.
7. Keep synchronization owned by DXVK resource tracking. Add only barriers or waits supported by inspected DXVK APIs and demonstrated hazards; do not add CPU waits to the per-frame path.
8. Bound generated-frame work and preserve existing pacing/latency controls. Do not introduce an unbounded queue.
9. Improve diagnostics so logs distinguish resource creation, pipeline creation, dispatch recording, command-list completion, and verified output pixels. Counters are not proof of correct pixels.
10. Document test procedure and exact artifacts needed for device validation.

## Audit findings from snap-patch-4

- The source already records history copies and the mipmaps/Alpha/Beta/Gamma/Delta/Generate passes on DXVK command lists. The previous task is therefore not to replace this architecture, but to verify and correct it.
- History images are double-buffered and flow resources are recreated when the framebuffer generation, extent, effective flow scale, or cache path changes.
- `dispatchChecked()` is already used by LSFG compute passes, but source-level success still does not prove correct GPU output.
- The current PE extractor contains arithmetic based on untrusted PE offsets and section ranges. Bounds checks must use overflow-safe range checks before dereferencing headers, directories, and resource payloads.
- The translator currently accepts any cached file with a SPIR-V magic/size check; cache validity must be tied to the DLL fingerprint and module validation, and failed/partial extraction must not leave a cache that appears complete.
- The runtime path currently prefers a translated/cached `<id>.spv` file without recording whether the original resource was SPIR-V or translated DXBC. Preserve source-kind metadata and explicitly report native-SPIR-V versus DXBC fallback in diagnostics.

## Work sequence

### P0 — branch and plan
- Create this branch from the exact `snap-patch-4` HEAD.
- Keep `snap-patch-4` unchanged.

### P1 — resource/cache correctness
- Harden PE parsing and all offset/size arithmetic.
- Make extraction and manifest publication atomic.
- Tie the shader cache to the Lossless.dll fingerprint.
- Keep original SPIR-V bytecode unchanged; translate only DXBC resources.
- Reject malformed or unsupported SPIR-V before runtime pipeline creation.

### P2 — GPU pass correctness
- Review the DXVK shader adapter and descriptor remapping against all LSFG shader roles.
- Verify UBO layout, image format/usage/extent, mip dimensions, and dispatch group sizes.
- Verify Alpha history recurrence and Beta/Gamma/Delta producer-consumer mapping against the WinNative reference.
- Verify per-output generation indices/timestamps and target lifetime.

### P3 — synchronization and presentation
- Verify that DXVK hazard tracking covers copy → mipmaps → shared passes → Generate → present.
- Verify output targets are not reused before their command list retires.
- Preserve bounded in-flight work and frame pacing.
- Do not introduce host waits or a second queue unless source evidence proves it necessary.

### P4 — validation
- Run available static/source checks and Android native CI where possible.
- Do not claim APK/native build success unless the native job actually runs.
- Device test: x2 first, rapid camera pan; then x4.
- Required artifacts: full `port/lsfg.log`; `port/log.txt` and logcat on crash/startup failure.
- Inspect ghosting, blur, warped edges, black/corrupt frames, pacing, and generated-frame count.

## Acceptance criteria

- A DLL cache is either fully valid for the current DLL fingerprint or rebuilt; no partial cache is treated as ready.
- Native SPIR-V resources are not needlessly translated.
- Every required pass fails closed when its shader, resources, pipeline, or dispatch cannot be committed.
- History resets cannot leak stale temporal data into generation.
- No per-frame `vkDeviceWaitIdle`, `vkQueueWaitIdle`, or CPU readback is introduced.
- Documentation reports source/build/device validation separately.

## Status

- [x] Branch created from `snap-patch-4`.
- [x] Baseline LSFG source and documentation inspected.
- [x] Plan recorded.
- [x] Resource/cache hardening: overflow-safe PE RVA mapping and resource-directory bounds; atomic resource-blob publication.
- [x] Temporal warm-up reset when the effective flow scale changes.
- [ ] Full shader/pass audit fixes (descriptor and temporal producer/consumer mapping).
- [ ] Native build / CI verification.
- [ ] Device image-quality validation.

## Implemented in snap-patch-5

### Commit `15c70a5` — PE resource bounds
- RVA mapping now accepts a required byte range and maps only bytes backed by section raw data.
- Uses subtraction-based range checks to avoid integer overflow in file-offset and resource-directory bounds.
- Validates resource-directory-relative child offsets and complete resource-data payload ranges before dereferencing or writing.

### Commit `c41de0b` / `25aff4d` — atomic extraction
- Extracted RCDATA blobs are written to temporary files, flushed, closed, and renamed into place.
- The manifest line is emitted only after the blob is published; manifest line endings remain standard newline bytes.

### Commit `85eef73` — temporal history reset
- A change in effective flow scale now resets the temporal warm-up counter.
- Generate remains gated until three successful shared-pass frames have repopulated the newly allocated mipmap and Alpha/Beta resources.

These are source-level changes only. Android NDK compilation, APK packaging, Vulkan runtime behavior, and output-image quality remain unverified.
