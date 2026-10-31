# LSFG (Lossless Scaling Frame Generation) в Skyrim Android

**Репозиторий:** https://github.com/Snap888/Skyrim_Android  
**Ветка разработки:** `snap-patch-2`  
**Документ:** описание внедрения, проделанной работы, тестов, файлов и текущего статуса  
**Дата актуализации:** 2026-10-31  

---

## 1. Цель

Встроить **frame generation** по принципу LSFG-VK (шейдеры из `Lossless.dll`) в Android-порт Skyrim:

- извлечение SPIR-V / RCDATA из `Lossless.dll`;
- кэш на устройстве;
- хук present DXVK (D3D9 → Vulkan);
- лёгкая GPU-цепочка на Adreno: **mipmaps (pass 255) → generate (pass 256)**;
- inject сгенерированного кадра в swapchain (double-present).

Целевая платформа: **Android arm64**, GPU **Qualcomm Adreno**, рендер через **DXVK + Vulkan**.

---

## 2. Архитектура (кратко)

```
Lossless.dll (RCDATA)
        │
        ▼
  lsfg_extract  →  port/lsfg_spirv/  (сырые модули)
        │
        ▼
  lsfg_translate → SPIR-V cache + manifest_spirv.txt
        │
        ▼
  Present hook (DXVK d3d9_swapchain)
        │
        ├─ hist copy (step3g)  — кадры истории в ping-pong
        ├─ descriptor reflect (step3d)
        ├─ GPU compute (step3hj): mipmaps → generate
        └─ inject (step3g/hj)  — gen → WSI image, double-present
```

**Важно:** полный 25-pass граф LSFG на Adreno не используется — только **light chain (2 pass)** из-за лимитов pipeline/bandwidth и риска hang.

---

## 3. Проделанная работа (этапы)

| Этап | Содержание | Статус |
|------|------------|--------|
| CI / APK | Workflow `build-and-release.yml`, `gen_url`, артефакты APK | Готово |
| Звук | WMAV2 через FFmpeg + `xaudio2.c` | Отдельная ветка/работа |
| Extract | PE RCDATA из `Lossless.dll` → кэш | Готово |
| Translate | Модули → SPIR-V, ~300 модулей | Готово |
| Present hook | Хук PresentImage / device+queue registration | Готово |
| step3d | Descriptor reflection, frame pair | Готово |
| step3f/g | History images, hist-copy через DxvkContext | Готово |
| step3hj | Light GPU: pass 255+256, UBO, fence, inject | В работе / стабилизация |
| Double-present | `presentLoops=2` при `should_inject` | Готово (CI restore swapchain) |
| Adreno safety | Cap intermediate, dispatch size, WaitIdle, throttle | В коде патча CI |

### Ключевые технические решения

1. **Linkage:** `extern "C"` trampolines в `dxvk_device.cpp` для `lsfg_step3hj_should_inject` / `insert_inject` (избежание C++ mangling / undefined symbols).
2. **Swapchain:** в CI файл `d3d9_swapchain.cpp` восстанавливается с известного коммита и патчится (double-present + LSFG decls).
3. **step3hj:** в дереве часто placeholder; полный код подтягивается в CI с коммита `445743c` и патчится `tools/patch_step3hj_adreno.py`.
4. **Adreno:**
   - intermediate текстуры **cap ≤ 960** (не full 2448×1080);
   - `vkCmdDispatch` только по `g_iw/g_ih` (иначе OOB → hang);
   - `vkQueueWaitIdle` перед `vkQueueSubmit` (сериализация с DXVK на той же graphics queue);
   - throttle ≥ 8, cooldown после inject, fence watchdog.

### Известный критический баг (исправлен в патче)

Раньше: intermediate **1280×564**, а dispatch **2448×1080** → запись за границы → **GPU hang** через несколько кадров.  
Правильный лог: `GPU dispatch #N 960x… (fb 2448x1080)`.

---

## 4. Структура исходников (релевантное)

```
android/app/jni/lsfg/
  lsfg_host.cpp / .h      — конфиг, init, extract/translate, set_live/throttle/inject
  lsfg_extract.*         — PE RCDATA
  lsfg_translate.*       — перевод в SPIR-V
  lsfg_step3d.*          — reflection, frame pair, queue/phys
  lsfg_step3f / 3g / 3h* — history, copy, roles, descriptors
  lsfg_step3hj.cpp       — light GPU chain + inject API (_impl)
  lsfg_log.*             — лог на устройство

android/app/jni/dxvk/src/
  dxvk/dxvk_device.cpp   — trampolines, регистрация device/queue
  d3d9/d3d9_swapchain.cpp — present hook, double-present (часто через CI restore)

.github/workflows/
  build-and-release.yml  — сборка APK + restore swapchain/step3hj + gen/

tools/
  patch_step3hj_adreno.py — Adreno-safe патч step3hj при сборке
```

Ветки: основная разработка LSFG — **`snap-patch-2`** (от `snap-patch`).

---

## 5. Необходимые файлы на устройстве

Базовый каталог игры (пример):

```
/storage/emulated/0/Download/Flud/The Elder Scrolls V - Skyrim/
```

| Путь | Назначение |
|------|------------|
| `port/lsfg.conf` | Конфиг (не TOML!) |
| `port/Lossless.dll` | DLL Lossless Scaling (источник шейдеров) |
| `port/lsfg_spirv/` | Кэш extract/translate (создаётся/пополняется при старте) |
| APK порта | Собранный debug/release с LSFG |

Лог (типично): каталог игры / Download, тег logcat вроде `skyrim-lsfg` / файловый лог LSFG (см. `lsfg_log_open(game_dir)`).

### Формат `port/lsfg.conf`

Порт читает **только** `ключ=значение`, **не** TOML `[[game]]`.

```
enabled=1
multiplier=2
flow_scale=1.00
performance=1

fg_live=1
fg_inject=1
fg_throttle=8

dll_path=/storage/emulated/0/Download/Flud/The Elder Scrolls V - Skyrim/port/Lossless.dll
shader_cache=/storage/emulated/0/Download/Flud/The Elder Scrolls V - Skyrim/port/lsfg_spirv
```

| Ключ | Смысл | Рекомендация Adreno |
|------|--------|---------------------|
| `enabled` | Вкл. LSFG host | `1` |
| `multiplier` | Множитель FG (в коде clamp 1–4) | `2` (не 20) |
| `performance` | Performance mode шейдеров | `1` |
| `fg_live` | Реальный GPU dispatch | `1` для теста FG; `0` — только hist-copy |
| `fg_inject` | Blit gen → swapchain | `1` после стабильного dispatch |
| `fg_throttle` | Submit каждые N кадров | `8`…`30` |
| `dll_path` | Путь к Lossless.dll | абсолютный |
| `shader_cache` | Кэш SPIR-V | `…/port/lsfg_spirv` |

Код: `lsfg_load_config(game_dir)` → файл **`{game_dir}/port/lsfg.conf`**.

---

## 6. Сборка APK

1. GitHub → Actions → workflow **Build & Publish APK**  
   https://github.com/Snap888/Skyrim_Android/actions/workflows/build-and-release.yml  
2. Ветка: **`snap-patch-2`**  
3. Input **`gen_url`**: URL архива `gen/` (tar.xz / tar.gz / zip), если `gen/` нет в runner.  
4. CI шаги:
   - restore + patch `d3d9_swapchain.cpp` (extern C + double-present);
   - restore `lsfg_step3hj.cpp` с `445743c` + `tools/patch_step3hj_adreno.py`;
   - проверка наличия `groups(dw)` и `vkQueueWaitIdle` (иначе fail);
   - download `gen/`;
   - `gradlew assembleDebug`.

Артефакт: `Skyrim-<version>.apk`.

---

## 7. Тесты и маркеры в логе

### 7.1. Базовый (FG off) — стабильность

`fg_live=0` в `lsfg.conf`.

Ожидается:
```
step3h-j: READY live=0 inject=… throttle=…
step3h-j: idle (fg_live=0)
step3g: hist copy #N → slot …
present: frame #N …
```
Игра не должна зависать. Hist-copy может идти тысячи кадров.

### 7.2. GPU chain (FG on)

`fg_live=1`, `fg_inject=1`, `fg_throttle=8`, `multiplier=2`.

**Обязательные** строки (новый APK с OOB-фиксом):
```
step3h-j: READY live=1 inject=1 throttle=8 stages=2
step3h-j: intermediate capped to 960x… (Adreno-safe)
step3h-j: intermediates OK 960x… gen+mip7+motion+detail
step3h-j: GPU dispatch #1 960x… (fb 2448x1080) mipmaps→generate
step3h-j: INJECT #1 (gen→swapchain GPU)
```

| Плохой признак (старый/битый билд) | Хороший |
|------------------------------------|---------|
| `GPU dispatch #1 2448x1080` без `(fb …)` | `960x… (fb 2448x1080)` |
| `capped to 1280x564` без правки dispatch | `capped to 960x…` + dispatch 960 |
| Hang после 3–5 INJECT | Продолжение hist copy / frame # сотни+ |

### 7.3. Translate / extract

```
translate done: ok=300 … → …/lsfg_spirv/manifest_spirv.txt
translate OK: 300 SPIR-V module(s)
LSFG: Vk context hook installed
LSFG: present-image hook installed
```

### 7.4. Наблюдаемые проблемы на тестах

1. **Hang после нескольких кадров** при `fg_live=1` — в основном OOB dispatch (исправлено патчем) и гонка `vkQueueSubmit` с DXVK (смягчено `WaitIdle`).
2. **Рывки** — double-present + throttle + WaitIdle (осознанный trade-off стабильности).
3. **Inject не на весь экран** — blit capped размера gen в угол framebuffer (пока без upscale на full FB).
4. **`compute_ok=0` / no interpolation yet** — диагностика present path; не всегда равна факту inject.
5. **TOML конфиг** — игнорируется; нужен только `lsfg.conf`.

---

## 8. Ограничения Adreno / мобильный Vulkan

- Не запускать полный optical-flow graph (десятки pass).
- Не делать full-res compute на 2448×1080 без необходимости.
- Не submit'ить compute на graphics queue параллельно с DXVK без синхронизации.
- `multiplier` > 2–4 на практике бесполезен и опасен.
- `WaitIdle` снижает FPS, но убирает типичный hang; дальше — перенос submit в cmd-stream DXVK.

---

## 9. Текущий статус (2026-10-31)

| Компонент | Состояние |
|-----------|-----------|
| Extract + translate | Работает |
| Present hook + hist-copy | Работает, стабильно при `fg_live=0` |
| Light GPU mipmaps→generate | Реализовано; требует APK с патчем OOB + WaitIdle |
| Inject → swapchain | Есть; частичный extent; зависит от стабильности submit |
| Full FG quality / full-screen scale | Не завершено |
| CI | YAML + verify маркеров патча |

**Практическая проверка «новый ли APK»:** в логе при `fg_live=1` должна быть строка  
`GPU dispatch #… 960x… (fb 2448x1080)`.  
Иначе пересобрать `snap-patch-2` и установить свежий артефакт.

---

## 10. Рекомендуемый порядок теста на устройстве

1. Установить свежий APK с Actions (`snap-patch-2`).
2. Положить `Lossless.dll`, создать `port/lsfg.conf` (пути абсолютные).
3. Сначала `fg_live=0` — убедиться, что игра идёт, hist copy растёт.
4. Затем `fg_live=1`, `fg_throttle=8`, `multiplier=2`.
5. Снять лог с метками `step3h-j` / `LSFG` / `present`.
6. При hang — сохранить последние 50–100 строк до зависания.

---

## 11. Возможные следующие шаги

1. Убрать `WaitIdle`: встраивать compute в command buffer DXVK (без отдельного `vkQueueSubmit`).
2. Upscale/blit gen 960 → full WSI (или generate сразу ближе к FB size при доказанной стабильности).
3. Аккуратное увеличение числа pass (motion/detail), если budget Adreno позволяет.
4. Хранение полного `lsfg_step3hj.cpp` в дереве (без restore с другого SHA), чтобы сборка не зависела от внешнего коммита.
5. Отдельный transfer/compute queue, если драйвер отдаёт.

---

## 12. Ссылки

- Репозиторий: https://github.com/Snap888/Skyrim_Android  
- Ветка: `snap-patch-2`  
- Workflow: `.github/workflows/build-and-release.yml`  
- Патч Adreno: `tools/patch_step3hj_adreno.py`  
- Конфиг на устройстве: `{game}/port/lsfg.conf`  

---

*Документ описывает фактическую реализацию в порте Skyrim Android, а не desktop LSFG-VK / Lossless Scaling UI. Формат конфига и пути — специфичны для этого NDK/DXVK порта.*
