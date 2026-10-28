# Plan: Fallout 3 on the Skyrim_Android runtime

Branch: **`fallout3`**  
Goal: playable **Fallout 3** (PC 32-bit D3D9) APK via static recompilation.

---

## 0. Reality check

| Myth | Reality |
|------|--------|
| Copy Skyrim APK, replace Data/ | **No** — code is lifted from TESV.exe |
| Same `gen/` as Skyrim | **No** — FO3 has a different PE |
| Only rename GAME_SKYRIM | **No** — shims use **fixed guest addresses** |
| FO3 ≈ Skyrim engine | **Related** (Gamebryo → Creation), still a full port |

Expect **weeks–months**, not a weekend: discovery alone for a large EXE is slow; shims dominate calendar time.

---

## 1. Goals / non-goals

### Goals (MVP)

- Boot to main menu
- New game / load save (single-player)
- Render via DXVK (Vulkan 1.1+)
- Basic audio (SFX; voice/music if XAudio/xWMA path works)
- Touch or gamepad via existing SDL overlay patterns

### Non-goals (later)

- FOSE / Script Extender
- Full mod manager parity
- Multiplayer / GFWL leftovers
- Fallout **New Vegas** (separate PE and version matrix)
- Fallout **4** (64-bit, DX11)

---

## 2. Technical baseline (from Skyrim port)

Reuse as much as possible:

| Component | Reuse? | Notes |
|-----------|--------|--------|
| `tools/discover.py`, `lift.py` | yes | set `LIFT_GAME=fallout3` when added |
| `runtime/` Win32 + memory model | yes | |
| DXVK-native D3D9 | yes | |
| SDL2 activity / input | yes | |
| XAudio2 / xWMA (snap-patch) | partial | FO3 audio stack may differ; verify imports |
| `skyrim_shims.c` | **no** | replace with FO3 shims |
| Skyrim `gen/` | **no** | regenerate from FO3 binaries |
| Android package `com.tj.skyrim` | **no** | use e.g. `com.tj.fallout3` |

---

## 3. Phased plan

### Phase A — Inventory (1–3 days)

1. Install legal FO3 PC (note patch / GOTY / language).
2. List PE imports of `Fallout3.exe` (DLL list: d3d9, d3dx9_*, bink, xaudio, steam_api, …).
3. Confirm 32-bit + D3D9 (Dependency Walker / `pefile`).
4. SteamStub → Steamless → `Fallout3_unpacked.exe`; record SHA-1.
5. Document required on-disk layout (`Data/Fallout3.esm`, BSA names, INI names).
6. Write findings into `docs/FALLOUT3_INVENTORY.md` (create when ready).

**Exit criteria:** known EXE hash, import list, data folder checklist.

### Phase B — Build flags & empty target (2–5 days)

1. `runtime/game.h` — add:
   ```c
   #elif defined(GAME_FALLOUT3)
   #define GAME_EXE       "Fallout3.exe"
   #define GAME_TITLE     "Fallout 3"
   #define GAME_PKG_DIR   "com.tj.fallout3"
   /* paths, d3dx dll names after inventory */
   ```
2. CMake / Gradle: `-DGAME_FALLOUT3`, applicationId `com.tj.fallout3`.
3. Stub `runtime/fallout3_shims.c` (empty or copy only generic INI helpers).
4. Stop compiling `skyrim_shims.c` when `GAME_FALLOUT3`.
5. CI workflow copy: build APK **without** FO3 `gen/` should still compile runtime (optional smoke).

**Exit criteria:** APK builds with FO3 branding; does not need to run game code yet.

### Phase C — Discovery & lift (heavy)

1. Extend `tools/regen_gen.sh` / lift config for FO3 module set:
   - primary: `Fallout3_unpacked.exe`
   - plus any DLL you decide to recompile (d3dx9, bink, x3daudio, …) — **mirror Skyrim’s four-module pattern only if needed**.
2. Run discovery (long). Prefer desktop Linux/WSL with lots of RAM.
3. Run lift → `gen/` (hundreds of MB C).
4. Commit policy: **do not** publish `gen/` derived from proprietary EXE if policy forbids; keep local / CI artifact like Skyrim `gen_url`.

**Exit criteria:** `gen/` links into `libmain.so` for FO3.

### Phase D — Host overrides & first boot

1. Map imports to existing SHIMs; implement missing ones.
2. `HOST_OVERRIDES` in `lift.py` for functions replaced by host (file IO, graphics present, …).
3. First run under Android: catch `rt_fatal`, missing ordinal, bad entry point.
4. Log strategy: `port/log.txt` + logcat tags (reuse skyrim-xaudio2 patterns where useful).

**Exit criteria:** process starts; reaches early engine init or a known crash address.

### Phase E — Graphics & main loop

1. D3D9 device creation via existing DXVK bridge.
2. Window size / focus / Android lifecycle.
3. Fix any FO3-specific D3DX or shader binary paths.

**Exit criteria:** main menu visible.

### Phase F — Input, saves, audio

1. Map touch overlay / gamepad to FO3 controls (may differ from Skyrim).
2. Save path: redirect to Android-accessible dir (`port/save` style).
3. Audio: confirm XAudio2 vs DirectSound; reuse xWMA work only if FO3 uses same path.

**Exit criteria:** can move in-game and save/load once.

### Phase G — Polish

1. Launcher UI strings / icons (`tools/make_icon.py` adapted).
2. Performance (core affinity, pipeline cache — steal from Skyrim 0.24 ideas).
3. GOTY DLC data paths if applicable.
4. README install guide for end users.

---

## 4. Suggested directory / file changes

```
runtime/
  game.h                 # + GAME_FALLOUT3
  fallout3_shims.c       # NEW — FO3-specific
  skyrim_shims.c         # not used for FO3 builds
  marvel_shims.c         # unchanged
android/app/
  build.gradle           # applicationId, app name
  …
tools/
  lift.py                # HOST_OVERRIDES_FALLOUT3, LIFT_GAME=fallout3
  regen_gen.sh           # FO3 module list / paths
docs/
  FALLOUT3_PLAN.md       # this file
  FALLOUT3_INVENTORY.md  # after Phase A
gen/                     # FO3-only; not Skyrim gen
```

---

## 5. Risk register

| Risk | Mitigation |
|------|------------|
| Different audio API | inventory imports early; keep DirectSound stubs |
| Games for Windows Live / legacy online | stub or no-op network APIs |
| Large discovery time / RAM | machine with ≥16 GB; cache `analysis/` |
| Address-dependent patches | minimize absolute hooks; prefer import SHIMs |
| Legal / redistribution | never ship Bethesda bins or `Data/` |
| Scope creep → FNV | freeze MVP on FO3 retail EXE only |

---

## 6. Milestone checklist

- [ ] Phase A inventory doc + EXE SHA-1
- [ ] `GAME_FALLOUT3` builds empty/runtime APK
- [ ] Discovery completes for FO3 EXE
- [ ] Lift produces linkable `gen/`
- [ ] First frame / logo / menu
- [ ] In-game movement + save
- [ ] Public test APK instructions (BYO game files)

---

## 7. Immediate next actions (this branch)

1. Complete **Phase A** on a PC with FO3 installed.
2. Land **Phase B** patches (`game.h`, Gradle, stub shims).
3. Do **not** delete Skyrim paths yet — keep `#ifdef GAME_SKYRIM` so one tree serves both until FO3 is stable.

---

## 8. References

- Upstream Skyrim port: https://github.com/ttom795/Skyrim_Android
- Branch `snap-patch`: xWMA / FFmpeg audio experiments (optional later for FO3)
- Steamless (SteamStub): https://github.com/atom0s/Steamless
