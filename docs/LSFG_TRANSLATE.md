# LSFG step 2 — DXBC → SPIR-V

After extract, `lsfg_translate_cache()`:

1. Reads `shader_cache/manifest.txt`
2. For `kind=dxbc`: `dxvk::DxbcModule::compile` → `DxvkShader::dump` → `id.spv`
3. For `kind=spirv`: copy to `id.spv`
4. Writes `shader_cache/manifest_spirv.txt`

Uses in-tree DXVK `src/dxbc/*` (linked into `dxvk_d3d9`).

## Log markers

```
=== LSFG step 2: DXBC -> SPIR-V ===
translate: id=N DXBC -> SPIR-V (M dwords)
translate done: ok=A fail=B skip=C
```

## Still missing (step 3)

Vulkan compute pipeline + present hook — no FPS gain until that lands.
