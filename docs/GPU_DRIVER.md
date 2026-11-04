# Custom Vulkan GPU drivers

The Android launcher can import a user-provided Turnip driver ZIP and select it for the Skyrim process.

## Import and selection

1. Open **Vulkan GPU driver** in the launcher.
2. Choose **Import Turnip driver ZIP** and select a driver package.
3. Select the imported driver in the list. Choose **System (Android driver)** to return to the device driver.
4. Start Skyrim again. Vulkan is initialized by DXVK inside the game process, so changing the selection takes effect on the next launch.

Driver packages are extracted into the app-private `files/gpu-drivers/<driver-id>/` directory. The importer requires a root-level `meta.json` and the library named by `libraryName`. It rejects path traversal, packages larger than 256 MiB after extraction, non-ELF libraries, and libraries that are not AArch64. The package's `minApi` is checked against the Android version.

## Runtime loading

The launcher writes `port/gpu-driver.conf`. The native host reads it after changing the working directory to the game folder and exports the selected private driver path before platform/Vulkan initialization. DXVK then calls libadrenotools to open the custom Vulkan driver using the app's `nativeLibraryDir` for the AdrenoTools hook libraries.

If the custom driver cannot be opened, DXVK logs the error and falls back to the Android system Vulkan loader. Check both `port/log.txt` and logcat (tag `SkyrimVulkan`) to confirm which path was taken.

## On-screen driver status

In the launcher's **Performance HUD** section, enable **Show Vulkan driver information**. With the FPS HUD enabled, the overlay shows the Vulkan physical-device name, driver version, Vulkan API version, and ICD selection status:

- `Custom Vulkan ICD: ACTIVE` means AdrenoTools returned a custom driver's `vkGetInstanceProcAddr` and DXVK is using that loader entry point.
- `Custom Vulkan ICD: NOT ACTIVE` means a custom driver was requested but DXVK fell back to the system Vulkan loader.
- `System Vulkan ICD` means no custom driver was requested.

The device name and driver/API versions are queried from the physical device selected by Vulkan. `ACTIVE` confirms loader activation; it does not by itself certify stability or performance of the driver.

## Compatibility notes

- The app currently builds only `arm64-v8a`; imported libraries must be AArch64.
- Turnip builds are GPU-generation-specific. A package intended for Adreno 8xx is not guaranteed to work on older Adreno GPUs.
- The app does not bundle proprietary or user-provided driver binaries. Each user imports their own ZIP.
- Import success only confirms package structure and ELF architecture. It does not prove that the driver is compatible with the device or that Vulkan initialization succeeds.
