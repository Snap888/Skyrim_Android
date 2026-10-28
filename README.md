# Fallout 3 for Android (static recompilation)

> **Branch `fallout3`** — work in progress.
> Based on [ttom795/Skyrim_Android](https://github.com/ttom795/Skyrim_Android):
> x86 → ARM64 static recompilation, DXVK-native (D3D9→Vulkan), SDL2.

This is **not** a ready-to-play port. The branch starts from the Skyrim
runtime and documents how to retarget it to **Fallout 3** (PC, 32-bit,
Direct3D 9, Gamebryo).

**No game files or copyrighted code are included.** You must own a legal
PC copy of Fallout 3.

---

## Status

| Area | State |
|------|--------|
| Shared runtime (kernel32, user32, XAudio2, DXVK glue) | inherited from Skyrim/LEGO ports |
| `GAME_FALLOUT3` / FO3 module list | planned |
| Discovery + lift of `Fallout3.exe` | not started |
| FO3-specific shims (INI, BSA, saves, Steam) | not started |
| Android launcher / package id | planned (`com.tj.fallout3`) |
| Playable APK | no |

See **[docs/FALLOUT3_PLAN.md](docs/FALLOUT3_PLAN.md)** for the full roadmap.

---

## Why Fallout 3 fits this stack

| Requirement | Fallout 3 |
|-------------|-----------|
| 32-bit x86 Windows | yes |
| Direct3D 9 | yes |
| Same era as Skyrim tools | Gamebryo (Skyrim is Creation — related lineage) |
| BSA / ESM / ESP data model | yes (same family as Oblivion/Skyrim LE) |
| XAudio2 / similar audio | largely shared with Skyrim path |

Fallout 3 is a **natural next target** after Skyrim, but it is **not** a
drop-in rename: fixed addresses in `skyrim_shims.c`, EXE layout, and
launcher UI are Skyrim-specific.

---

## Target game build (expected)

- **Fallout 3** PC (not New Vegas, not 4).
- Prefer **Steam** or GOG **GOTY** with latest official patch.
- Main binary typically: `Fallout3.exe` (32-bit).
- DRM: remove SteamStub with [Steamless](https://github.com/atom0s/Steamless) if present → e.g. `Fallout3_unpacked.exe` for discovery/lift only.
- Graphics stack on PC: **D3D9** (DXVK will translate on Android).

Exact SHA-1 / version pins will be filled in after the first successful
discovery run (see plan).

### Files you will need (planned)

| Role | Files |
|------|--------|
| Lift / discovery | `Fallout3_unpacked.exe`, linked system DLLs as required by the PE |
| Runtime on device | `Fallout3.exe` (or unpacked), `Data/` (ESM, BSA, etc.), INIs |
| Likely recompiled modules | `Fallout3.exe`, possibly `d3dx9_*.dll`, `binkw32.dll`, XAudio/X3DAudio if used |

---

## High-level approach

1. **Fork runtime defines** — add `GAME_FALLOUT3` next to `GAME_SKYRIM` / `GAME_MARVEL` in `runtime/game.h`.
2. **Discovery** — run `tools/discover.py` on FO3 PE; store analysis under `analysis/`.
3. **Lift** — `tools/lift.py` / `regen_gen.sh` → new `gen/` (do not reuse Skyrim `gen/`).
4. **Shims** — replace Skyrim fixed-address hooks with FO3 equivalents (`runtime/fallout3_shims.c`).
5. **Android** — package id, app name, launcher strings, CMake `-DGAME_FALLOUT3`.
6. **Iterate** — logcat / `port/log.txt`, fix missing imports, audio, saves.

Details and ordered tasks: **[docs/FALLOUT3_PLAN.md](docs/FALLOUT3_PLAN.md)**.

---

## Related branches

| Branch | Purpose |
|--------|--------|
| `main` | upstream Skyrim port baseline |
| `snap-patch` | Skyrim audio (xWMA/FFmpeg) and CI experiments |
| **`fallout3`** | this branch — FO3 adaptation |

---

## Legal

- Bring **your own** Fallout 3 PC installation.
- Do not redistribute Bethesda executables, `Data/`, or generated `gen/` derived from them if that would violate your license.
- This repository hosts only host/runtime code and documentation.

---

## Credits

- Original Skyrim Android static recomp: [ttom795](https://github.com/ttom795/Skyrim_Android)
- Fallout 3 adaptation branch: Snap888 / contributors
