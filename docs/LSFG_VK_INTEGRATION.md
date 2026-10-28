# LSFG-VK (Lossless Scaling Frame Generation) для Skyrim_Android

Ветка: **`snap-patch`**  
Вопрос: можно ли добавить генерацию кадров через **LSFG-VK** / **`Lossless.dll`** в нативный порт Skyrim (D3D9 → DXVK → Vulkan).

**Краткий ответ:** да, *принципиально возможно*, но **не** как «положить `Lossless.dll` рядом с игрой». На Android нужен **свой in-process** путь (явный Vulkan layer / встроенный framegen), лицензионный **`Lossless.dll` пользователя**, и GPU не слабее класса **Adreno 7xx** для полноценного FG. Это отдельная большая фича, не hotfix.

---

## 1. Как устроен рендер в snap-patch / Skyrim_Android

```
TESV (recompiled x86 guest)
    → D3D9 API (game)
        → dxvk-native (android/app/jni/dxvk, ~native-1.9.2b)
            → Vulkan 1.1 device + swapchain
                → SDL2 Android window / present
```

- Игра **не** Windows-процесс: нет Wine, нет загрузки PE-`Lossless.dll` как на ПК.
- Кадры уже в **Vulkan** после DXVK; LSFG имеет смысл **на выходе swapchain** (после D3D9→VK), а не «внутрь TESV.exe».
- Лаунчер уже умеет FPS cap / vsync-подобные опции (идея 0.24 upstream) — FG должен **дружить** с capping базового FPS.

---

## 2. Что такое LSFG-VK и `Lossless.dll`

| Компонент | Роль |
|-----------|------|
| **Lossless Scaling** (Steam, платно) | Оригинальный продукт; внутри **`Lossless.dll`** — шейдеры/логика FG (исторически D3D11) |
| **lsfg-vk** (Linux) | Open-source **Vulkan layer**: перехватывает `vkQueuePresentKHR` / swapchain, гоняет FG, используя шейдеры из **вашего** `Lossless.dll` |
| **LSFG-Android** | Порт pipeline на Android: из‑за политики ОС **нельзя** как на Linux молча воткнуть implicit layer в чужой APK; обычно **MediaProjection + overlay** или FG **внутри** контейнера (GameNative и т.п.) |

**Юридически:** `Lossless.dll` **нельзя** класть в git/APK. Только путь «пользователь указал свой файл из купленного Lossless Scaling»; проект лишь читает/извлекает SPIR-V (как делают LSFG-Android / GameNative).

Лицензия lsfg-vk (см. lsfg-vk.dev) — отдельное ограничение на распространение **кода layer**; перед встраиванием сверить актуальную лицензию.

---

## 3. Почему «просто lossless.dll» не сработает

| Идея | Почему нет |
|------|------------|
| Положить `Lossless.dll` в `Data/` | Это **Win32 PE**, не ARM `.so`; recomp-рантайм его не загрузит как нативный FG |
| Implicit Vulkan layer как на Linux (`VK_LAYER_PATH`) | Android 12+ **блокирует** подгрузку внешних layer в чужие/обычные процессы; у Skyrim свой APK — layer должен быть **частью приложения** или грузиться **явно** |
| Отдельное приложение LSFG-Android поверх Skyrim | Возможно **без** патча порта (overlay), но +латентность, MediaProjection, хуже для тач/оверлея управления |
| FG «внутри» DXVK без layer | Нужна правка present-path DXVK или post-process после `vkQueuePresent` — по сути тот же объём, что explicit layer |

---

## 4. Реалистичные варианты интеграции

### Вариант A — Explicit Vulkan layer **внутри** APK (рекомендуемый для «встроено в Skyrim»)

**Идея:** собрать `liblsfg-vk.so` (или форк lsfg-vk-android) под `arm64-v8a`, упаковать в APK, при создании Vulkan instance DXVK/SDL **включить layer по имени** (`ppEnabledLayerNames`), передать путь к user-`Lossless.dll` (или кэшу SPIR-V) через env / файл конфига.

```
DXVK creates VkInstance
  → layer: LS FG (in-process)
      → reads shaders (from user DLL extract once)
      → intercepts swapchain present
      → inserts interpolated frames
  → real driver present
```

**Плюсы:** один процесс, ниже латентность, чем overlay; не нужен второй APK.  
**Минусы:** слияние с **dxvk-native 1.9.2b** + Android WSI (AHB, present mode); отладка sync/latency; лицензии; GPU extensions.

**Зависимости устройства (типично для FG на Android):**

- Vulkan **1.2+** предпочтительно (lsfg-vk 2.x ориентир; порт Skyrim сейчас заявляет **1.1** — может понадобиться поднять требования или feature checks).
- `VK_ANDROID_external_memory_android_hardware_buffer` и связанное (в android-форках).
- Для полного FG часто нужен **`VK_EXT_robustness2`** → на практике **Adreno 7xx+** (SD 8 Gen 2 и новее). На Adreno 610 (как в README Skyrim) FG может **не** завестись или только mirror.

### Вариант B — Post-present hook в DXVK (без формального layer JSON)

Патч `dxvk` present: после готовности backbuffer — отдать кадр в framegen API, present уже сгенерированную последовательность.

**Плюсы:** полный контроль.  
**Минусы:** форк DXVK нужно сопровождать; легко сломать pipeline cache / vsync.

### Вариант C — Внешний LSFG-Android (без изменений Skyrim)

Пользователь ставит [LSFG-Android](https://github.com/FrankBarretta/LSFG-Android), указывает Skyrim, свой `Lossless.dll`.

**Плюсы:** ноль кода в `snap-patch`.  
**Минусы:** overlay latency (десятки ms), разрешения, конфликт с touch HUD; не «фича порта».

### Вариант D — Только документация «используйте GameNative FG»

Skyrim_Android — **нативный** APK, не контейнер GameNative. FG из GameNative **не** применяется к `com.tj.skyrim` автоматически. Имеет смысл лишь если когда-нибудь запускать тот же бинарь внутри контейнера (другой продукт).

---

## 5. Рекомендуемый план внедрения (вариант A)

### Phase 0 — Решение go / no-go

- [ ] Целевые GPU: только 7xx+ или попытка fallback на weak Adreno?
- [ ] Пользователь **обязан** купить Lossless Scaling (Steam) — UI + README.
- [ ] Лицензия lsfg-vk / android fork — можно ли static/dynamic link в APK.
- [ ] Не коммитить `Lossless.dll`.

### Phase 1 — Прототип вне игры

1. Собрать **lsfg-vk** или **lsfg-vk-android** под NDK `arm64-v8a`.
2. Минимальный Android Vulkan sample (или `vkcube`-аналог): explicit layer, present с 2× FG.
3. Проверка extract SPIR-V из user `Lossless.dll` on-device (pe-parse + DXBC→SPIR-V, как в LSFG-Android).
4. Замер latency / GPU time на целевом SoC.

### Phase 2 — Стыковка с DXVK в Skyrim

1. Найти в `android/app/jni/dxvk` создание `VkInstance` / `VkDevice` / swapchain (WSI path для Android).
2. Включить layer:
   - `VK_INSTANCE_LAYERS` **не** надёжен на всех Android;
   - лучше **явный** `enabledLayerNames` при `vkCreateInstance` в патче DXVK **или** thin wrapper вокруг loader.
3. Конфиг (пример):
   ```
   port/lsfg.conf
   enabled=1
   multiplier=2
   flow_scale=0.85
   performance=1
   dll_path=/storage/.../Lossless.dll   # или SAF URI → copy to filesDir
   ```
4. Лаунчер Skyrim: toggle **Frame generation**, multiplier 2–4, предупреждение «нужен свой Lossless.dll + сильный GPU».
5. Согласовать с **FPS cap**: база 30/40/45 → FG 2×; не включать FG при нестабильных &lt;30 base.

### Phase 3 — Продуктовая полировка

- [ ] Pipeline cache DXVK не инвалидировать без нужды.
- [ ] Отключение FG при cutscene / очень низком FPS (опционально).
- [ ] HUD: real fps vs presented fps (как в demos LSFG).
- [ ] Не ломать touch overlay (координаты = logical size после FG).
- [ ] CI: сборка APK **без** DLL; optional job с mock layer.

### Phase 4 — Документация для игрока

1. Купить Lossless Scaling в Steam.  
2. Скопировать `Lossless.dll` на телефон (из install dir Windows).  
3. В лаунчере Skyrim: указать файл / Import.  
4. Включить FG 2×, выставить cap базового FPS.  

---

## 6. Где трогать код в репозитории (ориентир)

| Путь | Зачем |
|------|--------|
| `android/app/jni/dxvk/` | Instance/device/swapchain, explicit layers |
| `android/app/jni/game/CMakeLists.txt` | link `liblsfg-*.so` optional `-DLSFG=ON` |
| `android/app/` Java/Kotlin launcher | UI toggle, SAF pick DLL, write `port/lsfg.conf` |
| `docs/LSFG_VK_INTEGRATION.md` | этот файл |
| **Не** `runtime/xaudio2.c` / gen | FG не про audio/recomp CPU |

Новых `HOST_OVERRIDES` в lift для FG **не** нужно: это host Vulkan path.

---

## 7. Риски и ограничения

| Риск | Комментарий |
|------|-------------|
| Слабый GPU (Adreno 610 и т.д.) | Базовый Skyrim target; FG может быть бесполезен или не поддержан extensions |
| Input lag | FG всегда добавляет задержку; для атаки ближнего боя в Skyrim держать **2×** и стабильный base FPS |
| Артефакты UI | Магия/компасы/меню — типичная боль optical FG |
| Нагрев / батарея | Дополнительная нагрузка на GPU |
| Лицензии | LS DLL + lsfg-vk license + не раздавать проприетарное |
| DXVK 1.9.2b vs новый lsfg-vk | Возможны API/present несовместимости — закладывать время на патчи |
| Vulkan 1.1 only devices | Может отсечь часть текущей аудитории порта |

---

## 8. Оценка трудозатрат (грубо)

| Этап | Порядок |
|------|--------|
| Phase 1 prototype | 1–3 недели |
| Phase 2 в Skyrim APK | 2–6 недель |
| Phase 3 polish | 1–2 недели |

Зависит от опыта с Vulkan layers и от того, берётся ли готовый **lsfg-vk-android** или чистый Linux lsfg-vk.

---

## 9. Практическая рекомендация для snap-patch **сейчас**

1. **Не** блокировать xWMA/audio и FO3-планы ради FG.  
2. Короткий путь для пользователей на мощных SoC: **внешний LSFG-Android** (вариант C) + инструкция в README.  
3. Средний путь: experimental branch `lsfg` с `-DLSFG=ON`, explicit layer, только 2×, BYO `Lossless.dll`.  
4. Долгий путь: поддержка в лаунчере, auto-extract shaders, OSD fps.

**Итог:** генерация кадров **совместима по архитектуре** с цепочкой DXVK→Vulkan в Skyrim_Android, но это **встраивание framegen layer/process**, а не подключение Windows-`Lossless.dll` как игрового мода. Без покупки Lossless Scaling и без GPU с нужными Vulkan extensions полноценного LSFG не будет.

---

## 10. Ссылки

- Skyrim_Android (рендер): README — dxvk-native, Vulkan 1.1  
- lsfg-vk: https://lsfg-vk.dev/  
- LSFG-Android: https://github.com/FrankBarretta/LSFG-Android  
- Контекст Android FG / GameNative: обзоры 2026 (overlay vs in-process layer)

---

*Документ описывает feasibility и план. Код layer/DLL в репозиторий не добавлялся.*
