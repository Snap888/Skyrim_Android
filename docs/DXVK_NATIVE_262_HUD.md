DXVK Native 2.6.2 HUD parity

The pinned 2.6.2 source already contains its Vulkan HUD renderer, D3D9 HUD items, and swapchain-blitter composition. The Android source patch restores the 1.9.2 default behavior: dxvk.conf wins, DXVK_HUD is the fallback, and FPS is enabled when neither is configured. Existing HUD items remain selectable with the standard HUD option string.

The HUD is drawn in the existing presentation command stream. It does not submit a second queue operation, acquire another swapchain image, or change WSI semaphore ownership. The built-in FPS reports actual DXVK presentation cadence. A generated-FPS value is not shown until generated frames are confirmed by their own successful WSI present.