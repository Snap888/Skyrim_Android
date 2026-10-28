# LSFG step 3 — present hook (scaffold)

`D3D9SwapChainEx::PresentImage` calls `lsfg_host_on_present(w, h, vkFormat)` each frame.

## Log markers

```
=== LSFG step 3: present hook LIVE ===
present: first frame WxH vkfmt=0x... spirv_modules=N
STATUS: hook receives frames; compute FG pipeline NOT wired yet
present: frame #60 ... (still no interpolation)
```

## Not yet

- VkPipeline from `port/lsfg_spirv/*.spv`
- Intermediate images + generate passes
- Multiplier timing

Real FG needs the lsfg-vk compute graph ordered by Lossless resource IDs.
