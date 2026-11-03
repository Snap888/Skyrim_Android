# Skyrim Android — документация работ и карта LSFG

> Документ сохраняет историческую карту LSFG и журнал работ. `snap-patch-5` остаётся reference-веткой LSFG/audio, а текущая ветка разработки SnapFrame — `frame_gen` (см. последние разделы). Исторические сведения ниже не являются заявлением о подтверждённой работоспособности frame generation на устройстве.

## 1. Назначение проекта

Ветка `snap-patch-3` исторически развивала Android-порт оригинальной PC-версии
*The Elder Scrolls V: Skyrim (2011)* и добавляет экспериментальную интеграцию
Lossless Scaling Frame Generation (LSFG) в графический путь Android.

Целевая цепочка:

- игра выдаёт D3D9 кадры;
- DXVK переводит D3D9 в Vulkan;
- LSFG получает пару последовательных кадров и выполняет compute-цепочку
  через **DXVK command list / DxvkContext**;
- промежуточные изображения передаются в существующий DXVK presentation path;
- Android APK собирается владельцем проекта вручную.

На Android не загружается Windows `Lossless.dll` как исполняемая DLL.
Она используется как источник ресурсов RCDATA с shader blobs. DLL и извлечённые
проприетарные shader payloads в репозиторий не добавляются.

## 2. Текущее состояние — коротко

| Подсистема | Состояние в исходниках ветки | Что подтверждено |
|---|---|---|
| LSFG config/bootstrap | Реализовано | Чтение конфигурации и регистрация DXVK hooks описаны в коде |
| Извлечение RCDATA | Реализовано | В ранее присланном runtime-логе извлечено 300 ресурсов |
| DXBC → SPIR-V | Реализовано | Ранее логировалась обработка 300 модулей |
| Descriptor binding normalization | Реализована в LSFG shader adapter | Требуется runtime-проверка созданных pipeline/descriptors |
| SPIR-V reflection / TSV | Реализовано | Ранее отражение прошло для 300/300 модулей |
| Два frame-history изображения | Реализовано | Ранее логировались оба слота и длительные серии completion callbacks |
| Mipmaps compute | Реализован вызов DXVK dispatch | Требуется повторный runtime-log на актуальном HEAD |
| Alpha / Beta compute | Реализованы DXVK pass functions | Требуется runtime-подтверждение на актуальном HEAD |
| Gamma / Delta / Generate compute | Реализованы DXVK pass functions | Требуется runtime-подтверждение на актуальном HEAD |
| Generated-frame presentation | Подключён в D3D9 swapchain presentation path | Требуется проверка изображения, частоты кадров и стабильности на устройстве |
| Android APK build | Не подтверждён в рамках этого документа | APK пользователь собирает самостоятельно |

**Важное разграничение.** В более ранних runtime-логах, полученных до
последних изменений этой ветки, отражение SPIR-V и копирование history уже
работали, но сообщалось об отсутствии GPU pipelines/dispatch и
`generated_frames=0`. После этих логов в ветке появились DXVK shader adapter,
mipmaps, alpha/beta, gamma/delta/generate и generated presentation. Поэтому
старые логи не доказывают работу новой цепочки, а наличие новых вызовов в
исходниках ещё не доказывает их успешное исполнение на телефоне. Нужен новый
APK и новый runtime log именно с текущего HEAD.

## 3. Архитектура и карта прохождения кадра

### 3.1 Общий поток

```text
Skyrim (D3D9)
    |
    v
DXVK D3D9 swapchain / present
    |
    +--> source frame image
    |       |
    |       +--> DXVK copyImage -> alternating history slot 0/1
    |                               |
    |                               v
    |                         Mipmaps (shader 255)
    |                               |
    |                               v
    |                         Alpha (290..293)
    |                               |
    |                               v
    |                         Beta (298..302)
    |                               |
    |                         shared flow state
    |                               |
    |               +---------------+----------------+
    |               | one pass per intermediate time |
    |               v                                |
    |         Gamma / Delta                           |
    |               |                                |
    |               v                                |
    |         Generate (shader 256)                   |
    |               |                                |
    |               v                                |
    +--------> offscreen generated target             |
                    |                                 |
                    v                                 |
             DXVK blitter -> acquired WSI image       |
                    |                                 |
                    v                                 |
                 Present                              |
```

Порядок на уровне LSFG: **mipmaps → alpha → beta → gamma/delta → generate**.
Mipmaps/alpha/beta — общая подготовка для пары реальных кадров. Gamma/delta
и generate выполняются для каждого запрошенного промежуточного timestamp.

Копирование реального игрового кадра в один из двух history slots через
`DxvkContext::copyImage()` — это захват входных кадров для алгоритма. Это не
отдельный LSFG shader pass `hist-copy`; такой дополнительный этап в
compute-цепочку не вводится. Все compute-команды должны записываться в
активный DXVK command list. Прямой независимый `vkQueueSubmit` из present-hook
не используется.

### 3.2 Стадии LSFG 3.1

| Этап | Shader ID / число стадий | Назначение |
|---|---|---|
| Mipmaps | 255 | Построение 7-уровневой luma/flow pyramid; reference tile 64×64 |
| Alpha | 290–293 | Четыре coarse-flow стадии на каждом из 7 уровней |
| Beta | 298–302 | Пять стадий уточнения потока |
| Gamma | 280, 282–285 | Пять стадий motion/confidence на уровень и generation slot |
| Delta | 280, 286–289, 281, 294–297 | Уточнение occlusion/warp detail на уровнях 4–6; стадии interleave с Gamma |
| Generate | 256 | Запись интерполированного изображения в storage-capable target |

Reference topology, восстановленная по WinNative:
- 7 pyramid levels;
- 3 history slots для recurrence-dependent pass state;
- 2 изображения входной пары кадров;
- Delta включается на уровнях 4, 5 и 6;
- flow — R8_UNORM; motion/detail — RGBA16F; входные изображения в reference —
  RGBA8 UNORM;
- timestamp промежуточного кадра: `(index + 1) / (generationCount + 1)`.

Эти сведения описывают recovered chain и служат технической спецификацией.
Они не означают, что все значения constant buffers, runtime shader variants
и image layouts уже независимо подтверждены на целевом Android GPU.

### 3.3 DXVK интеграция

Важные точки исходников:

| Файл | Ответственность |
|---|---|
| `android/app/jni/lsfg/lsfg_host.cpp` | Конфигурация, bootstrap, извлечение/трансляция, регистрация hooks |
| `lsfg_extract.cpp` | Извлечение PE RT_RCDATA из пользовательской Lossless.dll |
| `lsfg_translate.cpp` | DXBC → SPIR-V и подготовка shader cache |
| `lsfg_step3d.cpp` | Vulkan history images, CPU-side SPIR-V reflection и metadata export |
| `lsfg_step3e.cpp` | Подключение к настоящему present-image hook |
| `lsfg_step3g.cpp` | Два history slots, DXVK wrappers/views, frame copy, mipmaps dispatch, общий вызов Alpha/Beta и вход в generation |
| `lsfg_dxvk_shader.cpp/.h` | Создание compute shader/pipeline и проверка descriptor/push-constant интерфейса |
| `lsfg_dxvk_shared.cpp/.h` | Ресурсы и dispatch общих Alpha/Beta pass |
| `lsfg_dxvk_generation.cpp/.h` | Gamma/Delta, Generate, generation target и UBO |
| `lsfg_present.cpp` | Регистрация Vulkan context и present-side orchestration |
| `lsfg_log.cpp` | Запись LSFG runtime log |
| `android/app/jni/dxvk/src/d3d9/d3d9_swapchain.cpp` | DXVK swapchain: дополнительные generated presents |
| `android/app/jni/dxvk/src/dxvk/dxvk_swapchain_blitter.cpp` | DXVK blit/presentation primitives |

### 3.4 History и синхронизация

- История состоит из двух чередующихся изображений.
- Для каждого source frame DXVK записывает image copy и compute commands в
  активный `DxvkContext`.
- Completion callback привязан к DXVK command list и срабатывает после fence.
  Он подтверждает завершение command list, но сам по себе не подтверждает
  корректность пикселей.
- При изменении extent/формата/device generation история и связанные ресурсы
  сбрасываются; generation warm-up начинается заново.
- Generation допускается только после warm-up и готовности общих pass resources.
- Если generation pass не удаётся записать, цикл дополнительных generated
  presents прекращается после неудачного dispatch; это не должно создавать
  бесконечную серию пустых кадров.

## 4. Конфигурация LSFG

Файл конфигурации читается из:

`<папка Skyrim>/port/lsfg.conf`

Поддерживаемые ключи, читаемые `lsfg_load_config()`:

| Ключ | Значение / поведение |
|---|---|
| `enabled` | Включает LSFG при ненулевом значении |
| `multiplier` | Ограничивается диапазоном 1–4; число generated frames равно multiplier − 1, максимум 3 |
| `flow_scale` | Качество 0.1–1.0; задаёт базовый масштаб flow pyramid |
| `performance` | Режим: 0 = Performance (0.75 × quality), 1 = Stability (1.0 × quality), 2 = Low (0.5 × quality) |
| `dll_path` | Путь к копии выбранной пользователем `Lossless.dll` во внутреннем хранилище приложения |
| `shader_cache` | Каталог извлечённых RCDATA/SPIR-V в `noBackupFilesDir/lsfg/cache` |
| `state_cache` | Каталог DXVK pipeline state cache в `noBackupFilesDir/lsfg/dxvk` |

Пример:

```ini
enabled=1
multiplier=2
flow_scale=0.70
performance=1
dll_path=/data/user/0/<package>/files/lsfg/Lossless.dll
shader_cache=/data/user/0/<package>/no_backup/lsfg/cache
state_cache=/data/user/0/<package>/no_backup/lsfg/dxvk
```

Пути в примере иллюстративные: лаунчер записывает реальные абсолютные пути
для текущей установки. Если конфигурация отсутствует, LSFG не включается.
При `multiplier=1` генерируемых кадров нет.

### 4.1 Настройки в Android Launcher

- **Enable LSFG frame generation** включает/отключает host bootstrap.
- **Choose Lossless.dll** открывает системный SAF picker. Выбранный файл
  проверяется по MZ-заголовку и копируется в
  `getFilesDir()/lsfg/Lossless.dll`; Windows DLL не исполняется на Android.
- **Generated frames**: x2/x3/x4 соответствуют `multiplier=2/3/4`.
- **Generation quality**: SeekBar 0.1–1.0 с шагом 0.1; значение записывается
  в `flow_scale`.
- **Generation mode**: Performance использует 75% выбранного flow scale,
  Stability — 100%, Low — 50%. Это меняет разрешение flow compute, а не
  количество generated frames.
- RCDATA/SPIR-V cache находится в приватном `noBackupFilesDir/lsfg/cache`;
  DXVK pipeline cache — в `noBackupFilesDir/lsfg/dxvk`.
- При замене Lossless.dll shader cache очищается, чтобы не использовать
  ресурсы от предыдущего DLL. Временные history/generation images создаются
  в GPU memory и не сохраняются как файлы.
- Конфигурация `port/lsfg.conf` и диагностический `port/lsfg.log` остаются
  рядом с игрой, чтобы их можно было приложить к runtime-отчёту.

## 5. Shader archive и reflection

Аудит пользовательского архива, записанный в
[`LSFG_SHADER_AUDIT.md`](LSFG_SHADER_AUDIT.md), установил:

- `Lossless.dll` — PE32+ Windows x86-64, не Android native library;
- обнаружено 300 shader modules, IDs 101–400;
- IDs 101–302: 202 DXBC-derived SPIR-V modules;
- IDs 303–400: 98 уже готовых SPIR-V ресурсов;
- эти две семьи используют разные descriptor binding conventions — их нельзя
  смешивать при создании pipeline layouts.

CPU reflection анализирует entry point, workgroup size и descriptors.
Расширенный TSV экспортирует доступные declarations image types, buffer
members/offsets и array strides. Это статические сведения SPIR-V, а не
автоматическое определение реальных VkImageView formats/usages или значений,
записанных host в constant buffers.

Для анализа вариантов есть
[`tools/compare_lsfg_spirv.py`](../tools/compare_lsfg_spirv.py). Пример:

```sh
python3 tools/compare_lsfg_spirv.py /path/to/port/lsfg_spirv
```

По умолчанию сравниваются candidate pairs base/+49/+98. Совпадение SHA-256
доказывает только идентичность бинарных файлов; различающиеся файлы могут
быть функционально эквивалентны, а одинаковые интерфейсы не доказывают
runtime selection.

## 6. История работ в snap-patch-3

Ветка создана от `snap-patch-2`. Ниже — ключевые подтверждённые commits
текущей ветки, а не полный список каждого промежуточного изменения:

| Commit | Работа |
|---|---|
| `c8d58b6` | DXVK compute shader/resource adapter |
| `d46013c` | Mipmaps dispatch через DXVK context |
| `ded8a0b` | Исправления storage-image reflection и frame index |
| `949a335` | Отказ от compute shaders с неподдержанными push constants |
| `2ccfee5` | Alpha/Beta pass dispatch |
| `21ab1b7` | Передача shared flow outputs в generation |
| `4740dc7` | Gamma/Delta/Generate passes |
| `bcc6aacf` | Generated-frame presentation в DXVK swapchain |
| `6d85863` | Регистрация DXVK compute shaders в pipeline cache |
| `31b5c4c` | Ограничение image view usage flags |
| `7e0f574` | Callback registration для frame copy и generation |
| `91bc549` | Сброс warm-up при изменении backing images |
| `82f779e` | Generation gated by fresh completed shared frame |
| `009ddaa` | Логирование записанных generate dispatches |
| `df8f683` | Present generated output по полному WSI extent |
| `91d82db` | Отклонение generation targets с несовпадающим history extent |
| `1da46f6` | Остановка generation burst после неудачного dispatch |
| `820bed6` | Flush compute→fragment barriers перед sampling generated target |
| `acd029f` | Нормализация SPIR-V Binding decorations и DXVK resource slots |
| `90ae4a9` | HUD: отображение активного Vulkan ICD в DXVK HUD |
| `732407e` | Передача статуса custom Vulkan ICD в HUD |
| `0317b57` | Исправление объявления string comparison для driver HUD |
| `98ac295` | Документация статуса Vulkan driver HUD |

Текущий branch HEAD на момент документа: `98ac2957fede8c805375a86ecbcba91c79074091`.

Последнее исправление устраняет несоответствие между binding-номерами исходных
LSFG shader modules (DXBC-translated: 800/816/832/1088; embedded SPIR-V: 0/16/32/48)
и компактными слотами, используемыми DXVK binder. Адаптер теперь перенумеровывает
binding decorations в SPIR-V и соответствующие `DxvkResourceSlot` одинаково.
Это исправляет интерфейс дескрипторов на уровне кода, но ещё не подтверждает
успешную компиляцию APK или корректные GPU-пиксели на устройстве.

## FPS HUD для проверки LSFG

В Android-ветке FPS HUD включён по умолчанию через fallback
`dxvk.hud = fps`. Поэтому он должен отображаться без отдельного
`dxvk.conf`. Его можно явно включить через `dxvk.hud = 1` или
`DXVK_HUD=1` в окружении процесса DXVK. Переменная окружения Termux
не влияет на уже запущенное Android-приложение.

- **Game FPS** — число вызовов исходного D3D9 `PresentImage` за секунду.
- **FG submit FPS** — число успешно сформированных generated images,
  переданных в DXVK `SubmitPresent`, за секунду.
- **Output submit FPS** — сумма Game FPS и FG submit FPS.

Счётчики обновляются раз в 500 мс. FG и Output обозначают именно
поставленные в очередь кадры, а не подтверждённые фотон-время/scanout:
это полезная оценка работы FG, но не замена измерению фактической частоты
обновления дисплея. Для объективного сравнения следует сравнивать Game FPS,
FG submit FPS и Output submit FPS в одной и той же игровой сцене.

## 7. План работ / roadmap

### Этап A — сборка и базовая интеграция
- [x] Bootstrap/config и загрузка пользовательской DLL как источника данных.
- [x] RCDATA extraction и DXBC → SPIR-V cache.
- [x] Vulkan context/present hooks.
- [x] DXVK-managed frame history и completion callbacks.

### Этап B — compute chain
- [x] DXVK compute shader adapter.
- [x] Mipmaps dispatch.
- [x] Alpha/Beta shared passes.
- [x] Gamma/Delta generation passes.
- [x] Generate shader и offscreen generation target.
- [x] Generated presentation callback и WSI blit path.
- [ ] Подтвердить успешное создание всех нужных pipelines на реальном Android GPU.
- [ ] Подтвердить каждый этап по runtime log и отсутствию ошибок Vulkan/descriptor validation.

### Этап C — функциональная проверка
- [ ] Собрать APK из текущего HEAD (сборка выполняется владельцем проекта).
- [ ] Проверить LSFG выключен/включен и multiplier 2.
- [ ] Проверить multiplier 3/4 после подтверждения режима 2.
- [ ] Проверить корректность картинки: отсутствие чёрных кадров, tearing,
  неверного extent, сильного ghosting и corruption.
- [ ] Проверить frame pacing и фактическое число presents на каждый source frame.
- [ ] Проверить resize, сворачивание/возврат, повторный запуск и длительную сессию.
- [ ] Сверить output с reference WinNative по pass order и shader variant.
- [ ] Только после этого объявлять LSFG рабочим.

### Этап D — стабилизация
- [ ] Устранить ошибки сборки, если они проявятся на текущем HEAD.
- [ ] Проверить ресурсы и синхронизацию при смене swapchain/device generation.
- [ ] Оценить стоимость compute passes и подобрать flow_scale/performance.
- [ ] Обновить документацию фактическими runtime результатами.

## 8. Сборка APK

APK собирается владельцем проекта вручную. Документ не утверждает, что APK
собран или что текущий HEAD уже прошёл Android build.

Основная инструкция и требования к SDK/NDK находятся в корневом
[`README.md`](../README.md). Команда для Linux/macOS из каталога `android`:

```sh
./gradlew assembleDebug -PndkVersion=29.0.14206865
```

Windows:

```bat
gradlew.bat assembleDebug "-PndkVersion=29.0.14206865"
```

Ожидаемый путь debug APK:
`android/app/build/outputs/apk/debug/app-debug.apk`.

Если сборка падает, для диагностики нужен полный участок вывода от первой строки
`error:` или `FAILED:` до конца соответствующей ошибки, включая linker
`undefined reference` / `undefined symbol`, если это линковка.

## 9. Тестирование и какие логи присылать

Основной LSFG лог:

`<папка Skyrim>/port/lsfg.log`

Он открывается в append-режиме, поэтому перед новым тестом лучше сохранить
предыдущий файл или очистить его при остановленном приложении. Дополнительный
общий лог порта:

`<папка Skyrim>/port/log.txt`

`port/log.txt` нужен, если игра не запускается, падает, зависает или не доходит
до present. Для обычной проверки LSFG в первую очередь нужен полный
`port/lsfg.log`.

В актуальном логе следует искать следующие группы сообщений (точный набор
зависит от того, какие этапы достигнуты):

- bootstrap: `LSFG host init`, `Lossless.dll found`,
  `extract OK`, `translate OK`;
- Vulkan/present: `Vk context registered`, `present hook LIVE`,
  `present-image hook installed`;
- mipmaps: `step3h: mipmaps DXVK resources ready`,
  `step3h: mipmaps dispatch`;
- shared passes: `step3i: alpha/beta DXVK resources initialized`;
- generation: `step3j: gamma/delta/generate DXVK resources ready`,
  `step3j: generate command recorded`;
- history lifetime: `GPU command-list completion` и
  `GPU command-list completion failed`;
- ошибки: строки уровня `[E]`, сообщения `failed`, `mismatch`,
  `unsupported`, а также любые Vulkan/driver errors в logcat.

Важно: `command recorded` означает, что команда добавлена в DXVK command list.
Это ещё не доказывает успешное исполнение GPU, корректный результат пикселей
или видимое увеличение FPS. Для подтверждения нужны лог без ошибок, реальный
визуальный результат и измерение фактических presents/frame pacing.

Рекомендуемый сценарий первого теста:
1. Установить APK, собранный из текущего HEAD.
2. Убедиться, что игра запускается с LSFG выключенным.
3. Включить LSFG с `multiplier=2`, начать обычную игровую сцену и дать ей
   поработать несколько минут.
4. Сохранить полный `port/lsfg.log`; если был crash/hang — также
   `port/log.txt` и соответствующий logcat.
5. Не обрезать начало сессии: важны bootstrap, создание ресурсов, первые
   dispatches и возможная ошибка первого generated frame.

## 10. Ограничения и инженерные правила

- Не считать CPU reflection доказательством корректного GPU pipeline.
- Не считать fence completion доказательством правильности содержимого image.
- Не считать запись dispatch command доказательством успешного GPU execution.
- Не подменять optical-flow generation простым temporal blend.
- Не смешивать descriptor conventions DXBC-derived и original SPIR-V families.
- Не угадывать UBO layout, image formats, descriptor bindings или shader variants
  там, где они не подтверждены.
- Не отправлять независимый `vkQueueSubmit` из present hook; compute и output
  должны соблюдать DXVK command recording, synchronization и resource lifetime.
- Не записывать compute output непосредственно в WSI image без подтверждённой
  storage usage; используется отдельный offscreen target и DXVK blit.
- Не коммитить `Lossless.dll`, извлечённые проприетарные blobs, runtime cache,
  пользовательские игровые файлы или APK.
- Reference WinNative используется как технический ориентир. Восстановление
  графа не означает копирования GPL-кода. Перед переносом исходного кода нужно
  отдельно проверить лицензионную совместимость и сохранить notices.

## 11. Документы по теме

- [Android runtime tuning map](ANDROID_RUNTIME_TUNING_MAP.md) — experimental settings, synchronous pipeline compilation, thread/Surface diagnostics, deferred game-thread affinity and worker back-off work.
- [XAudio2 starvation fix](XAUDIO2_STARVATION_FIX.md) — buffer-boundary callback refill, bounded xWMA decode wait and next runtime test.
- [LSFG implementation notes](LSFG_IMPLEMENTATION.md) — подробная история и
  архитектурные заметки реализации.
- [LSFG chain recovery](LSFG_CHAIN_RECOVERY.md) — recovered LSFG 3.1 pass graph,
  resource topology и интеграционные gates.
- [Shader archive audit](LSFG_SHADER_AUDIT.md) — состав архива и descriptor families.
- [Shader variant map](LSFG_VARIANT_MAP.md) — candidate ID mapping base/+49/+98.
- [Vulkan integration](LSFG_VK_INTEGRATION.md) — Vulkan/DXVK integration notes.
- [Extraction](LSFG_EXTRACT.md) — PE resource extraction.
- [Translation](LSFG_TRANSLATE.md) — DXBC → SPIR-V.
- [Present hook](LSFG_STEP3_PRESENT.md) — present path notes.
- [Recovery patch](step3g_presentimage.patch) — historical patch artifact.
- [Root README](../README.md) — исходная сборка и установка Skyrim Android.

---

Последнее обновление: 2026-11-01. При изменении фактического состояния ветки
этот документ и таблица статуса должны обновляться по новым commits и runtime
логам; не переносить старые диагностические выводы на более новый HEAD без
повторной проверки.


## Актуализация LSFG — 1 ноября 2026

Последний просмотр исходников выполнен на ветке `snap-patch-3`, HEAD
`98ac2957fede8c805375a86ecbcba91c79074091`. Это обновление фиксирует
состояние кода и следующий этап диагностики; нового runtime-лога от APK,
собранного с этого HEAD, в ходе этой проверки не получено.

### Что подтверждено исходниками

- `lsfg_step3g.cpp` чередует два history slots (`g_pp & 1`) и копирует
  source image в backing VkImage через `DxvkContext::copyImage()`.
- После копирования вызывается shader 255 (mipmaps), затем
  `dispatchLsfgSharedPasses()` для общей Alpha/Beta подготовки.
- Generate разрешается после warm-up (не менее трёх кадров с успешной общей
  подготовкой), при наличии обоих history views и готовности shared resources.
- `lsfg_step3g_generate_impl()` получает отдельный generation target и вызывает
  `dispatchLsfgGeneration()` с frame index, generation count/index и flow scale.
- Completion callbacks регистрируются на DXVK command list. Они подтверждают
  статус завершения command list, но не читают и не проверяют пиксели.
- В коде этой цепочки не добавлен самостоятельный `vkQueueSubmit`; compute
  dispatches записываются через переданный `DxvkContext`.

### Открытая проблема качества

В последнем пользовательском описании статичная сцена выглядит чёткой, но
быстрое движение камеры/объектов вызывает размытие и искажения, в том числе
при x4. Это пользовательское наблюдение, а не результат воспроизведения в
текущей сессии.

Следующий аудит должен идти от данных, а не от новых счётчиков:
1. проверить соответствие SPIR-V descriptor bindings реальным DXVK slots;
2. сверить image formats, extents и mip-level размеры каждого pass;
3. проверить Alpha history и Beta flow outputs, их временную последовательность;
4. сверить Gamma/Delta входы, UBO layouts/values и timestamp каждого output;
5. проверить target reuse/lifetime и producer-consumer barriers на реальном
   DXVK command list.

Не добавлять барьеры вслепую: сначала проверить существующие DXVK hazard
tracking и точные read/write зависимости. Не менять WinNative pass order без
подтверждённого расхождения.

### Текущие ограничения подтверждения

В этом просмотре проверены исходники, но не выполнялись Android NDK build,
APK packaging, установка или GPU runtime test. Наличие вызовов dispatch в
исходниках не доказывает успешное создание всех pipeline, корректность
дескрипторов, успешное исполнение GPU или качество generated pixels.

Для следующего runtime review необходимы:
- полный `port/lsfg.log` с APK, собранного из актуального HEAD;
- `port/log.txt` и logcat, если есть crash/hang/ошибка запуска;
- тест сначала на x2, затем на x4, с коротким быстрым поворотом камеры.


## Runtime LSFG enable/disable — 1 November 2026

Latest source changes on `snap-patch-3` add an ON/OFF slider to the in-game
LSFG settings panel (opened from the top-left `☰` menu). OFF sets the effective
multiplier to `1x`, disables the runtime present/generation path and requests
zero generated frames. ON restores the selected 2x/3x/4x multiplier without
restarting the game.

A follow-up backend correction keeps the LSFG host initialized when the saved
multiplier is `1`. The config retains `enabled=1` to mean LSFG is configured;
`multiplier=1` means runtime generation is OFF. This is necessary so the
in-game panel remains available and can turn LSFG back ON after a launch with
OFF saved. OFF is a bypass, not a resource unload: shader resources and DXVK
hooks remain initialized.

Implementation/documentation commits:
- `96c6870` — runtime generation gate and live settings persistence.
- `d263a7d` — in-game ON/OFF slider.
- `29f910f`, `17ec3ce` — preserve host initialization and saved OFF state.
- `d8f29d6` — detailed settings semantics and limitations.

The Java-only CI status and native arm64 build status must be checked separately.
No APK build or device test is claimed by this documentation update. See
[In-game LSFG settings](LSFG_IN_GAME_SETTINGS.md) for behaviour and test notes.


## Актуализация snap-patch-5 — производительность DXVK / LSFG (2 ноября 2026)

Проверены последние пользовательские артефакты из `Logs/`:

- `dxvk_lsfg_gpu.csv`: 23 407 GPU stage samples, все со статусом `ok`,
  2 130 групп кадров.
- Сумма измеренных GPU pass на одну цепочку generation: среднее 7,196 мс,
  медиана 6,178 мс, p95 10,475 мс, максимум 11,509 мс.
- Самый дорогой участок — Gamma/Delta level 6: 2,082 мс в среднем
  (около 29% суммы). Alpha — 1,232 мс, Mipmaps — 0,987 мс,
  Generate — 0,930 мс, Beta — 0,874 мс.
- `dxvk_perf_on.csv` содержит только LSFG ON. Это CPU-side PresentImage
  timing, а не полное время кадра. Сопоставимого LSFG OFF прогона нет;
  прирост FPS/потерю производительности по этой серии вычислить нельзя.
- В тестовой конфигурации: multiplier=2, flow_scale=1.00,
  performance=0 (Performance mode); фактический flow extent — 1836x810
  при source extent 2448x1080.

### Первая оптимизация

GPU timestamp profiler переведён в режим opt-in. В обычной игре он выключен,
чтобы не создавать на каждом pass query objects, timestamp commands,
completion callbacks и отдельные операции записи CSV. Для диагностического
прогона включить переменную окружения:

```sh
DXVK_LSFG_GPU_PROFILE=1
```

Изменение не меняет алгоритм LSFG, shaders, dispatch order или синхронизацию.
Это удаляет накладные расходы самого профилировщика из обычного игрового
пути.

### Следующие задачи

1. Аудит shader/resource dependencies Gamma/Delta L6 — главный измеренный
   GPU bottleneck. Не пропускать и не объединять стадии без доказательства
   эквивалентности WinNative reference.
2. Проверить стоимость Alpha/Mipmaps и shared pass resources.
3. Получить парные DXVK benchmark логи LSFG OFF/ON на одинаковой сцене.
4. Проверить frame pacing, длительную нагрузку и визуальное качество x2/x4.

Статус проверки: изменения исходников и документации подготовлены; NDK build,
APK и device retest ещё не выполнены. Успешные GPU timestamps не доказывают
качество сгенерированных пикселей, а текущий ON-only benchmark не доказывает
прирост FPS.


## Результаты нового теста LSFG ON/OFF — 2 ноября 2026

Получены парные файлы `Logs/dxvk_perf_off.csv` и
`Logs/dxvk_perf_on.csv`. Разрешение обоих прогонов — 2448x1080, VSync=1.

| Метрика CPU PresentImage | OFF | ON |
|---|---:|---:|
| Количество окон | 21 | 15 |
| Среднее оконных средних | 5,137 мс | 9,918 мс |
| Медиана оконных средних | 6,536 мс | 12,001 мс |
| Максимальный max_cpu_present_ms | 17,554 мс | 838,957 мс |

Это не полное время кадра и не FPS. Прогоны разной длины, а в ON есть
изолированные большие пики; поэтому цифры показывают дополнительную
нагрузку/столлы CPU-side PresentImage, но пока не дают точного A/B результата.
В runtime log подтверждены LSFG ON, multiplier 2, DXVK command-list path,
300/300 успешных SPIR-V reflection entries и completion counter до 1800.
Поздняя отдельная сессия в том же логе имеет enabled=0. `Logs/log.txt`
пустой. GPU stage CSV отсутствует, так как профилировщик выключен по
умолчанию.

Следующий тест: повторить OFF/ON на одной сцене и одинаковой длительности,
не включая GPU profiler в обычном benchmark. Отдельно выполнить короткий
ON-прогон с `DXVK_LSFG_GPU_PROFILE=1` для измерения pass timings.


## LSFG GPU profiling from launcher settings

In `snap-patch-5`, GPU stage timing can be enabled without manually setting
process environment variables. Open the launcher, enable **LSFG GPU stage
profiling (CSV)** under **Experimental runtime tuning**, then start Skyrim
with LSFG enabled. After a clean exit, collect
`<Skyrim folder>/port/dxvk_lsfg_gpu.csv`. The option is OFF by default because
timestamp queries and CSV writes add diagnostic overhead. The launcher writes
the selected state to `port/lsfg_gpu_profile.conf`.


## План комплексной оптимизации snap-patch-5 — 2 ноября 2026

Добавлен [единый план оптимизации](OPTIMIZATION_PLAN_SNAP_PATCH_5.md), охватывающий:
- аудио: музыку, голоса и эффекты; XAudio2 refill, xWMA decode и starvation;
- DXVK: локальные просадки FPS, pipeline compilation, PresentImage stalls и раздельные CPU/GPU метрики;
- LSFG: визуальные искажения при резких движениях, temporal history, flow и pass/resource audit;
- CPU scheduling: эксперимент с big.LITTLE affinity и приоритетами потоков без жёсткой привязки к номерам ядер;
- тестовую дисциплину и изоляцию логов между сессиями.

Ручное закрытие приложения с последовательностью `quit requested` / FORTIFY mutex / signal 6 в текущем тесте не считается игровой ошибкой. Возвращаться к teardown следует только при воспроизводимом самопроизвольном завершении.

### Последняя пара LSFG ON/OFF

В последней паре тестов медиана CPU Present составила 0,86 мс при OFF и 10,96 мс при ON. Медиана лучше отражает типичное окно при наличии редких больших всплесков, но это не full frame time и не FPS. В отдельной игровой локации пользователь наблюдает меньшую просадку Game FPS при включённом LSFG; этот сценарий включён в план как обязательный воспроизводимый тест, но причинная связь ещё не установлена.

Файл `dxvk_lsfg_gpu.csv` из последней загрузки не относится к OFF-запуску: при OFF генерация не выполнялась, поэтому CSV является оставшимся результатом предыдущей ON-сессии. В плане выделено исправление изоляции/очистки CSV и маркировки каждой сессии.

План задаёт этапы и критерии проверки, но не означает, что перечисленные оптимизации уже внесены в исходники или подтверждены на устройстве. Следующие изменения должны выполняться отдельными commits и проверяться APK на устройстве.


### Реализация оптимизации — 2 ноября 2026

Первое изменение в исходниках `snap-patch-5`: LSFG GPU profiler CSV теперь сбрасывается на каждый запуск до проверки LSFG ON/OFF и получает явный session marker. Это предотвращает использование старых ON GPU samples как результата OFF теста. Подробности и тестовая процедура: [LSFG GPU profiler](LSFG_GPU_PROFILER.md) и [план оптимизации](OPTIMIZATION_PLAN_SNAP_PATCH_5.md).

Изменение source-level; native APK build и device validation ещё ожидаются. Аудио, DXVK rendering и LSFG algorithm пока не менялись.




## Актуальное состояние snap-patch-5 — 3 ноября 2026

Текущая рабочая ветка проекта — [`snap-patch-5`](https://github.com/Snap888/Skyrim_Android/tree/snap-patch-5), HEAD:

`2acde0c27b8246a45eab12b29fc4d41dc6cefc8b`

Последнее изменение — [per-dispatch GPU profiling для Gamma/Delta](https://github.com/Snap888/Skyrim_Android/commit/2acde0c27b8246a45eab12b29fc4d41dc6cefc8b). Профилировщик теперь сохраняет как агрегированные строки `GammaDelta_L0–L6`, так и индивидуальные интервалы `Gamma_L<level>_S<stage>` и `Delta_L<level>_S<stage>`. Это позволит локализовать дорогие dispatch внутри ранее самого затратного уровня L6.

Профилирование включается в launcher через **LSFG GPU stage profiling (CSV)**; вручную задавать переменные окружения не требуется. Для следующего теста нужен APK из актуального HEAD и свежие `dxvk_lsfg_gpu.csv`, `dxvk_perf_on.csv`, `lsfg.log`.

**Статус:** source commit опубликован и проверен; Android NDK build, APK и device runtime validation ещё не подтверждены. Изменение не меняет алгоритм LSFG или порядок GPU passes. Подробности: [GPU profiler](LSFG_GPU_PROFILER.md), [план оптимизации](OPTIMIZATION_PLAN_SNAP_PATCH_5.md), [статус реализации](LSFG_SNAP_PATCH_5_STATUS.md).


## Новый runtime-тест LSFG x2 — 3 ноября 2026, 00:58

Проверен новый набор артефактов из `Logs/` на ветке `snap-patch-5`, upload HEAD `2358e474eaf4c2374b459b8eca38386e5138d974`.

Конфигурация: LSFG ON, x2, flow_scale=1.00, Performance mode, source extent 2448x1080, effective flow extent 1836x810 (0.75); драйвер Turnip Gen8 V38.

### Подтверждено в `lsfg.log`

- 300 RCDATA извлечены; 300/300 SPIR-V переведены без ошибок.
- Reflection: 300/300 модулей успешно, ошибок reflection нет.
- Активен реальный WSI present-image hook и DXVK command-list compute path.
- Созданы два history wrapper slots; copy completion дошёл до #9000.
- Mipmaps, Alpha/Beta и Gamma/Delta/Generate resources инициализированы.
- Generate command записывался до source_frame=9000; present counter дошёл до 18000.

Это подтверждает устойчивую активность command-recording/copy-completion пути в течение сессии, но не доказывает корректность каждого generated pixel и визуальное качество кадра.

### CPU-side PresentImage

`dxvk_perf_on.csv`: 76 окон, frame 120–9120, VSync=1. Среднее оконных средних — 7,112 мс; медиана — 6,555 мс; P95 — 11,501 мс. Максимальный `max_cpu_present_ms` — 824,513 мс (единичный stall). Среднее первых 10 окон — 6,665 мс, последних 10 — 10,880 мс: в конце capture виден рост CPU Present cost, причина пока не локализована.

Requested/generated averages в CSV находятся между 0,983333 и 1,0, что согласуется с x2 режимом (примерно один generated frame на source frame). CPU PresentImage не является полной длительностью кадра или FPS.

### Ограничения анализа

В текущей загрузке нет `dxvk_perf_off.csv`, поэтому это не A/B тест. Большие файлы `dxvk_lsfg_gpu.csv` (25,7 МБ) и `log.txt` (1,43 МБ) присутствуют в Git tree, но их содержимое не удалось прочитать через GitHub file connector. Поэтому не публикуются новые GPU stage timings и выводы по звуку/xWMA. Подробный разбор и следующий тест — в [статусе snap-patch-5](LSFG_SNAP_PATCH_5_STATUS.md).


## Состояние ветки `frame_gen` — 3 ноября 2026

Работа по SnapFrame ведётся отдельно от `snap-patch-5`. Ветка содержит
самостоятельный модуль `android/app/jni/snapframe/`; LSFG остаётся отдельной
подсистемой. Текущий прототип захватывает реальные кадры, строит half-res
luma, оценивает двунаправленное coarse motion и формирует forward/backward
consistency mask. Он **не реконструирует и не показывает промежуточные кадры**.

### Перенос аудио из snap-patch-5

В `frame_gen` перенесены изменения `runtime/xaudio2.c`, совпадающие с
реализацией `snap-patch-5`:

- плавный 5 ms gain ramp на source voice при временном starvation;
- -6 dB master-bus headroom перед soft limiter с сохранением pre-gain peak
  telemetry;
- запуск SDL audio device в paused state и старт после prefill 120 ms;
- повышение steady-state queue target до 120 ms;
- логирование фактического SDL device format, частоты, каналов и buffer size;
- учёт ошибок `SDL_QueueAudio` и расширенный output timing log.

Владелец проекта сообщил о чистом звуке на устройстве на сборке
`snap-patch-5`. Это подтверждает combined audio result той ветки, но не
является отдельной проверкой перенесённого кода в `frame_gen`. После
переноса требуется локальная сборка и прослушивание. Подробности и ограничения:
[AUDIO/XAudio2 status](XAUDIO2_STARVATION_FIX.md).

### SnapFrame — следующий инженерный этап

Последний доступный диагностический прогон `frame_gen` завершил первые три
фазы по 1,800 кадров; четвёртая фаза была неполной, полного
`TEST_SUITE_COMPLETE` нет. В flow CSV присутствуют неfinite-free samples,
но заметна высокая доля displacement vectors на границе search window.
GPU timestamp CSV показывает сильную асимметрию между одинаковыми forward и
backward kernels; эти времена пока нельзя считать достоверной стоимостью
отдельных dispatch. Файл пространственного dump присутствует в Git tree (7,866,563 байта), то есть не пуст. Однако его полный текст не удалось извлечь через GitHub connector из-за размера; содержимое и строки требуют отдельного чтения/анализа.

Поэтому следующий этап — не включение генерации в Present, а обязательная
валидация: исправить/проверить readback vector dump, проверить Vulkan timestamp
query semantics, добавить controlled zero-motion/known-translation тесты и
сопоставить GPU output с ground truth. Только после прохождения этих ворот
начинать warp/reconstruction shader и отдельную presentation design.

**Ограничения процесса:** APK не собирался, GitHub Actions не запускались.
Сборку и тест на устройстве выполняет владелец проекта локально.


### SnapFrame source milestone — variable motion-grid density

На `frame_gen` реализовано применение `flow_scale_percent` к размерам
motion grid, dispatch dimensions и координатам forward/backward consistency.
При 50% ожидаемая сетка для 2448x1080 — 77x34 вместо 153x68 при 100%.
Это уменьшает число flow cells примерно в четыре раза, но снижает
пространственную детализацию. Реализация пока source-only: shader compilation,
NDK build и проверка на Adreno не выполнялись. Перед warp/reconstruction
необходимо проверить нулевое движение, известные смещения, направление
векторов и поведение граничных блоков.


### frame_gen — обновление SnapFrame по логам 2026-11-03

Последняя загрузка использует `flow_scale=50%`, motion grid 77x34.
Завершены `static_scene` и `slow_camera_pan`; фаза вращения дошла примерно
до 1500/1800 кадров. Процесс завершился через `ExitProcess(0)`, поэтому
оснований считать это аварийным завершением нет. Фазы 4–7 не запускались.

Пространственные CSV содержат восемь полных сеток. На панорамировании и
вращении многие векторы упирались в прежний предел +/-4 texel, а
forward/backward consistency была низкой. Для кадра 4801, близкого к
неподвижной сцене, средние confidence и consistency составили примерно
0.987 и 0.988.

В `block_match_flow.comp` добавлен coarse-to-fine поиск: 9x9 coarse
кандидатов с шагом 2 texel в диапазоне +/-8 и последующая 3x3 refinement.
Эффективный диапазон — +/-9 при 89 проверяемых позициях. Порог диагностики
граничного насыщения синхронизирован с новым диапазоном.

Это пока изменение исходников. GLSL/SPIR-V compilation, APK build и
проверка на устройстве не выполнялись. До реализации warp/reconstruction
необходимо повторить те же сцены и подтвердить улучшение граничных
попаданий, confidence и consistency; затем выполнить синтетическую
проверку известных смещений.
