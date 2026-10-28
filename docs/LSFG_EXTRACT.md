# LSFG step 1 \u2014 extract from Lossless.dll

On `lsfg_host_init`, if `enabled=1` and `Lossless.dll` is readable:

1. Parse PE resource tree for **RT_RCDATA** (type id 10).
2. Write each blob to `shader_cache/rcdata_<id>.bin`.
3. Write `shader_cache/manifest.txt` lines: `id size kind` (`dxbc` / `spirv` / `raw`).

Re-runs skip extract if `manifest.txt` exists (delete the folder to force redo).

## Expected logcat

```
skyrim-lsfg: extract: id=255 size=... kind=dxbc
...
skyrim-lsfg: extract: done, N RCDATA blob(s) -> .../port/lsfg_spirv
skyrim-lsfg: LSFG: extract OK (N resources). DXBC\u2192SPIR-V + present hook still TODO.
```

## Not yet

- DXBC \u2192 SPIR-V translation
- Vulkan present injection

Do not commit `Lossless.dll` or extracted blobs to git.
