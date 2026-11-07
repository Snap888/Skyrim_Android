# Skyrim Android — driver, texture and FPS analysis (2026-11-07)

## Evidence from the uploaded run

- The game data root is readable. `Data/Skyrim.esm` and `Data/Skyrim - Misc.bsa` both pass stat/open probes. The Data listing contains 30 entries, including `Skyrim - Textures.bsa`, the three HighResTexturePack archives and their ESP files.
- DXVK Native 2.6.2 successfully opens `Skyrim - Textures.bsa`, `Skyrim - Meshes.bsa`, `Skyrim - Shaders.bsa`, the interface, sound, voice and update archives.
- The log reports `textureCompressionBC = 1` and `BC texture emulation off`. This is not evidence of a BC texture capability failure.
- Four loose FaceTint lookups for `00000007.dds` fail under `Data/Textures/...`. The log does not prove that these assets are absent from BSA archives; keep these separate from confirmed archive-open failures.
- Repeated file probes contain `DATA/DATA/...` although the install root has one `DATA` directory. Some equivalent single-`DATA` paths are present. The path mapper now retries the de-duplicated path only when the actual target exists.
- The launcher log says Turnip `libvulkan_freedreno.so` was requested from private app storage. However, DXVK 2.6.2 reports `Qualcomm Technologies Inc. Adreno Vulkan Driver 0.676.66`. This means the selected custom ICD was not actually used in this run.
- The system driver advertises `VK_KHR_maintenance5`, but reports feature `maintenance5 = 0`. DXVK 2.7.1 must not be selected on this driver until its required feature is genuinely enabled.
- The process profile shows sustained CPU activity across the guest SDL thread and DXVK command-stream/worker threads. It does not contain GPU timestamp or per-frame CPU/GPU wait data sufficient to attribute every FPS drop to one stage.
- SnapFrame was disabled in this run. It cannot be used to infer FG overhead from this capture.

## Changes made

1. Ported the existing AdrenoTools custom-ICD loading path into the pinned DXVK Native 2.6.2 Vulkan loader. The selected private Turnip ICD is now opened through `adrenotools_open_libvulkan`; the returned loader handle is retained by DXVK. Failure falls back to system Vulkan and is logged explicitly.
2. Added the AdrenoTools dependency and made the dynamically loaded `libmain_hook.so` and `libhook_impl.so` part of the candidate build graph.
3. Added safe `DATA/DATA` retry logic to the Win32 path mapper. It only changes the resolved path when the single-`DATA` target exists; genuine missing assets and real nested paths are not rewritten.
4. Added CI regression checks for the custom ICD loader integration and duplicate-DATA path handling.

## DXVK version decision

- Keep 2.6.2 as the baseline while validating the corrected custom ICD path.
- Do not yet expose 2.7.1 as a selectable runtime. It requires `VK_KHR_maintenance5`; the system driver in this run reports the feature disabled. First verify that the selected Turnip driver is actually active and reports the required feature.
- 2.7.2 was not confirmed as an official upstream release at the time of this analysis. Do not pin an unverified tag or invent a versioned runtime pair.

## Device validation

1. Select the imported Turnip driver and run with SnapFrame OFF.
2. Confirm the log contains `AdrenoTools activated custom Vulkan driver` and DXVK reports the Mesa/Turnip driver, not Qualcomm 0.676.66.
3. Check Vulkan extension/feature output for `VK_KHR_maintenance5` and `maintenance5 = 1`.
4. Check whether normalized duplicate-DATA paths reduce file-not-found probes and whether the missing texture symptoms change.
5. Capture the DXVK A/B timing CSV with SnapFrame OFF. Then repeat the same route with SnapFrame ON; do not compare different scenes or different driver versions.
6. Only after the above checks, build and test a separately named DXVK 2.7.1 + matching Skyrim runtime pair. Keep 2.6.2 available for rollback.


## Build

The Android ARM64 APK build is triggered from the commit carrying the AdrenoTools loader and path-normalization changes. The DXVK 2.6.2 runtime remains the selected test candidate; DXVK 2.7.1 stays gated on verifying that the selected Turnip ICD is truly active and exposes `maintenance5 = 1`.
