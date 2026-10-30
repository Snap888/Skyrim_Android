# Skyrim Android — документация работ и карта LSFG

> Документ описывает состояние ветки `snap-patch-3` на commit
> [`1da46f68f1d8`](https://github.com/Snap888/Skyrim_Android/commit/1da46f68f1d8a68c4ce8ff0e1f4b826d5337f20a)
> (30 октября 2026). Это инженерный журнал и карта проекта, а не заявление
> о подтверждённой работоспособности frame generation на устройстве.

## 1. Назначение проекта

Ветка `snap-patch-3` развивает Android-порт оригинальной PC-версии
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
| `flow_scale` | Масштаб flow pyramid; если не задан или <= 0, используется 0.70 |
| `performance` | Читается из config; отдельные режимы производительности в этом README не заявляются |
| `dll_path` | Путь к пользовательской `Lossless.dll` |
| `shader_cache` | Каталог shader cache; по умолчанию `<game_dir>/port/lsfg_spirv` |

Пример:

```ini
enabled=1
multiplier=2
flow_scale=0.70
performance=1
dll_path=/storage/emulated/0/Skyrim/Lossless.dll
shader_cache=/storage/emulated/0/Skyrim/port/lsfg_spirv
```

Путь в примере нужно заменить на фактическую папку игры на устройстве.
Если конфигурация отсутствует, LSFG не включается. При `multiplier=1`
генерируемых кадров нет.

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

Текущий branch HEAD на момент документа: `1da46f68f1d8a68c4ce8ff0e1f4b826d5337f20a`.

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

Последнее обновление: 2026-10-30. При изменении фактического состояния ветки
этот документ и таблица статуса должны обновляться по новым commits и runtime
логам; не переносить старые диагностические выводы на более новый HEAD без
повторной проверки.
