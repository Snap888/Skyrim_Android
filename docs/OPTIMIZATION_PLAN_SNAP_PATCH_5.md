# План оптимизации Skyrim Android — snap-patch-5

- Репозиторий: `Snap888/Skyrim_Android`
- Рабочая ветка: `snap-patch-5`
- Дата плана: 2026-11-02
- Назначение: единый план следующего цикла runtime-оптимизации DXVK, LSFG, аудио и CPU scheduling.
- APK собирает и тестирует владелец проекта вручную. Документ не утверждает, что запланированные изменения уже реализованы или проверены на устройстве.

## 1. Приоритеты и трактовка текущих наблюдений

| Приоритет | Направление | Наблюдение | Цель |
|---|---|---|---|
| P1 | Аудио | В логах повторяются признаки underrun/empty queue; проблема наблюдалась и при LSFG OFF | Устранить хрипы музыки, голосов и эффектов |
| P1 | DXVK | В отдельных игровых местах наблюдаются сильные просадки FPS; CPU Present зависит от режима LSFG | Найти источник CPU/GPU stalls и стабилизировать frame time |
| P1 | LSFG | При быстрых движениях камеры видны переменные по силе искажения | Улучшить temporal consistency и качество motion reconstruction |
| P2 | CPU scheduling | Предложено распределить DXVK и LSFG по производительным ядрам | Проверить влияние affinity/scheduling без ухудшения общей производительности |
| P3 | Shutdown | FORTIFY mutex / signal 6 появляется после явного закрытия приложения | Не считать текущий случай дефектом игры; вернуться только при самопроизвольном падении |

Порядок P1 не означает, что все подсистемы нужно менять одновременно. Для каждого эксперимента — отдельный небольшой diff, отдельный APK и сравнение с baseline.

## 2. Завершение игры: исключение ожидаемого закрытия

Последовательность `quit requested` → `FORTIFY: pthread_mutex_lock called on a destroyed mutex` → `CRASH: signal 6` в текущем наборе логов соответствует ручному закрытию приложения пользователем.

Решение:
- не регистрировать этот эпизод как игровую ошибку или регрессию;
- не выделять отдельную задачу на исправление mutex teardown на основании только этого лога;
- при самопроизвольном падении во время игры собирать новый logcat и полный runtime log и анализировать отдельно.

## 3. Аудио: музыка, голоса, эффекты

### Исходные признаки

В runtime-логах встречались большие значения `empty_queue`, отдельные ненулевые `pending_decode` и периоды низкого заполнения очереди. Это указывает на риск starvation, но сами счётчики не позволяют однозначно определить, какой voice/category виноват.

В предыдущей работе уже описана граница refill XAudio2 и параллельные xWMA decode workers. Новые изменения должны опираться на актуальный код и свежий runtime trace, а не повторять уже выполненные правки.

### План диагностики и оптимизации

1. **Разнести аудиокатегории.** Добавить/проверить диагностические метки для music, voice/dialogue и SFX: voice id/type, codec, queued bytes/frames, pending decode, refill reason, underrun count и время decode/refill. Не логировать каждый sample; использовать агрегаты за интервал.
2. **Проверить очередь каждого voice.** Убедиться, что медленный decode музыки не блокирует голоса и эффекты, что публикация готовых блоков сохраняет порядок, а callbacks/refill не теряются.
3. **Проверить xWMA decode.** Измерить p50/p95/max времени decode, размер входного блока, длительность полученного PCM и ожидание worker. Проверить поведение при нескольких одновременных потоках и ограниченном числе workers.
4. **Проверить XAudio2 refill lifecycle.** Границы буфера, повторное заполнение, callback lifetime, остановку/перезапуск voice, loop/play-region, освобождение очереди. Исключить гонки и ожидание decode на критическом аудиопотоке.
5. **Настроить буферизацию по результатам.** Сравнить несколько ограниченных размеров очереди/порогов low-watermark. Увеличивать буфер только если trace показывает кратковременный scheduling/decode gap; не скрывать постоянную нехватку производительности большой задержкой.
6. **Проверить при совместной нагрузке.** Отдельные сцены с музыкой, диалогом, большим числом SFX, затем LSFG OFF/ON. Проверять отсутствие underrun и задержку старта/переключения звука.

### Критерии приёмки

- Нет слышимых разрывов музыки, речи и эффектов в повторяемом сценарии.
- Нет устойчивого роста underrun/empty queue counters.
- Очередь не опустошается при обычных кратковременных пиках decode/scheduling.
- Не появляется чрезмерная задержка звука, нарушение порядка voice или зависание при остановке игры.
- CPU overhead аудиопотока и worker pool не ухудшает frame pacing.

## 4. DXVK: просадки FPS и пики Present

### Что показывают текущие тесты

Для последней пары тестов:
- CPU Present median при LSFG OFF: **0,86 мс**.
- CPU Present median при LSFG ON: **10,96 мс**.
- Средние: OFF **1,64 мс**, ON **16,64 мс**.

Медиана полезнее среднего для оценки типичного окна, поскольку отдельные всплески заметно влияют на среднее. Однако CPU Present — не полный frame time, не GPU execution time и не FPS. Низкая медиана не исключает редкие stalls и не объясняет локальную просадку в конкретной игровой сцене.

Наблюдение владельца проекта: в определённом месте игры FPS сильно проседает, а при LSFG ON график Game FPS показывает меньшую просадку. Это важный сценарий для воспроизведения, но пока не доказательство ускорения DXVK: Game FPS, generated/output FPS и CPU Present измеряют разные вещи.

### План

1. Зафиксировать воспроизводимую save/location/camera path, где возникает просадка. Сравнивать OFF и ON при одинаковом разрешении, VSync, настройках, длительности и фоновой нагрузке.
2. Разделить метрики: Game FPS (source presents), FG submit/output rate, CPU Present p50/p95/p99/max, GPU frame/pass time, pipeline compile duration, queue wait и пропуски/повторы кадров.
3. **Pipeline compilation:** определить, какие вызовы компиляции выполняются синхронно на критическом пути. Проверить cache hit/miss, повторную компиляцию и поведение при первом посещении локации. Исследовать безопасную асинхронную компиляцию/предварительный прогрев только после анализа потокобезопасности и порядка использования pipeline.
4. **Present path:** разложить время PresentImage на flush/command recording, acquire, synchronization/wait, submit и generated presentation. Не оптимизировать по одному суммарному таймеру.
5. **GPU/driver:** сопоставить CPU stalls с GPU busy/queue timing и LSFG GPU timestamps. Проверить bandwidth, render-target/texture traffic, overdraw и возможный thermal throttling в проблемной сцене.
6. **Кэш и повторное использование:** проверить pipeline cache persistence, descriptor/resource churn и частые аллокации. Не менять размеры кэшей наугад.
7. Повторить A/B после каждой оптимизации. Отдельно фиксировать cold run (первое посещение/компиляция) и warm run (повторная сцена).

### Критерии приёмки

- Уменьшение p95/p99 и максимальных stalls в повторяемой сцене.
- Меньше заметных просадок source Game FPS и стабильнее frame pacing.
- Отсутствие новых графических ошибок, deadlocks и регрессии LSFG OFF.
- Результаты ON/OFF сопоставимы по одинаковому workload; generated frames не засчитываются как дополнительные simulation frames.

## 5. LSFG: качество при резких движениях

### Симптом

При резких поворотах камеры и быстром движении объектов появляются визуальные искажения. Их выраженность меняется во времени: в некоторых моментах они уменьшаются, в других усиливаются. Это может быть связано с содержимым motion/flow, временной историей, occlusion/disocclusion, несовпадающими timestamps или ресурсами/синхронизацией. Причина пока не установлена.

### План проверки

1. Зафиксировать один тестовый save и несколько коротких camera paths: статичная сцена, медленный pan, быстрый pan, движение объекта поперёк камеры, тонкие геометрические детали/частицы и резкая смена направления.
2. Начать с x2. x3/x4 проверять только после базовой оценки x2; не менять multiplier посреди одного benchmark.
3. Сопоставить кадры source A, source B и generated output. Сохранять одинаковые временные точки и сравнивать артефакты по типу: ghosting, warped edges, tearing, blur, disocclusion holes, duplicated silhouettes.
4. Проверить соответствие входных history frames, их порядок и timestamps каждого generated frame. Проверить warm-up/reset при изменении extent, flow scale, multiplier и пересоздании ресурсов.
5. Аудировать Alpha/Beta flow outputs и Gamma/Delta motion/occlusion inputs: descriptors, UBO values, image formats/extents/mip levels, producer-consumer dependencies и barriers. Сверять topology и dispatch order с WinNative reference; не переставлять и не пропускать pass без доказательства.
6. Проверить lifetime и reuse generation targets/history slots: исключить чтение stale data и перезапись ресурса до завершения потребителя.
7. Исследовать влияние flow_scale и performance mode контролируемыми шагами. Сначала измерить качество и GPU cost каждого значения, а не выбирать режим только по FPS.
8. Только после локализации причины менять алгоритм/ресурсную схему. Не маскировать артефакты простым blending и не добавлять CPU readback/ожидания на каждый кадр.

### Критерии приёмки

- При быстром pan меньше ghosting/warping и стабильное качество от кадра к кадру.
- Нет stale history после reset/resize/смены настроек.
- Нет black/corrupt output, ошибок Vulkan и нарушений frame pacing.
- x2 проходит базовый тест до расширения на x3/x4.

## 6. CPU scheduling: DXVK, LSFG и производительные ядра

### Гипотеза

На устройстве с big.LITTLE CPU-кластерами можно проверить, даст ли размещение критичных CPU-потоков DXVK/LSFG на производительных ядрах меньшие CPU-side stalls. Это экспериментальная гипотеза, а не готовое решение.

Важно:
- GPU compute LSFG выполняется GPU; CPU affinity влияет на потоки, которые записывают команды, управляют ресурсами и обслуживают callbacks, но не «переносит GPU-шейдеры на супер-ядра».
- DXVK и LSFG используют общие контексты/command lists и синхронизацию. Жёсткое разделение на разные ядра или независимые очереди может добавить межпоточные ожидания и ухудшить latency.
- Android scheduler может менять affinity/cpuset по thermal/power policy; приложению могут быть недоступны некоторые CPU. Нельзя предполагать конкретные номера big cores.

### Безопасный экспериментальный план

1. Получить реальную топологию CPU на устройстве: доступные CPU, capacity/frequency policy, affinity текущих потоков и ограничения Android. Не хардкодить CPU numbers.
2. По trace определить критичные потоки: game/render thread, DXVK submission/present, LSFG command recording/callback, audio mixer и decode workers. Не назначать affinity только по названию потока.
3. Снять baseline без affinity changes при LSFG OFF и ON.
4. Экспериментировать по одному параметру:
   - default Android scheduling;
   - мягкая подсказка/приоритет для критичного render-submit thread, если поддерживается;
   - ограниченная affinity только для конкретного подтверждённого CPU-bound thread group;
   - отдельный тест размещения decode workers на efficiency cores, чтобы освободить big cores для render path.
5. Не закреплять одновременно все DXVK, LSFG, audio и worker threads на big cores. Не менять affinity audio callback так, чтобы он конкурировал с render thread.
6. Сравнить p95/p99 CPU Present, source FPS, audio underruns, CPU utilization, частоты/температуру и устойчивость длительной сессии.
7. Оставить affinity opt-in и с безопасным fallback на системный scheduler. По умолчанию не менять поведение до подтверждения выгоды.

### Критерии приёмки

- Улучшение frame pacing в повторяемом тесте без ухудшения аудио и LSFG.
- Нет роста thermal throttling, энергопотребления или нестабильности.
- Нет предположений о фиксированных номерах ядер; корректная работа на устройствах с другой топологией.

## 7. Диагностика и тестовая дисциплина

1. Для каждого прогона записывать branch/commit, APK build, device/Android, GPU driver, resolution, VSync, game settings, LSFG multiplier/flow_scale/performance, location/save, длительность и thermal state.
2. OFF/ON тесты выполнять отдельными чистыми сессиями на одинаковой сцене. Не переключать multiplier в середине измерения.
3. Разделять обычный performance benchmark и GPU timestamp profiling: профилировщик LSFG opt-in и может вносить диагностические накладные расходы.
4. Исправить lifecycle `dxvk_lsfg_gpu.csv`: гарантировать новый файл/очистку перед каждой сессией, в том числе LSFG OFF; записывать run/session ID и config snapshot. Если LSFG OFF, файл должен быть пустым с заголовком или явно содержать статус `not_run`, а не оставлять CSV от предыдущего ON запуска.
5. Не интерпретировать старый `dxvk_lsfg_gpu.csv` как данные текущего OFF теста. В последней паре OFF-запуск имел `lsfg.enabled=0`, requested/generated=0, поэтому сохранённый GPU CSV относился к предыдущей ON-сессии.
6. Для каждого теста сохранять `lsfg.log`, `log.txt`, соответствующие CSV и logcat при сбое. Для аудио — агрегированные counters и trace; для качества LSFG — source/generated screenshots или короткие видеозаписи, если доступны.
7. После каждого изменения — отдельный commit, Android NDK/APK build status отдельно от runtime validation. Не объявлять исправление готовым по одному source-level diff.

## 8. Последовательность выполнения

| Этап | Работа | Выход |
|---|---|---|
| 0 | Зафиксировать baseline и исправить изоляцию диагностических файлов | Чистые воспроизводимые OFF/ON артефакты |
| 1 | Аудио: трассировка voice/refill/decode и проверка уже добавленных workers | Локализованный источник starvation и тестируемая коррекция |
| 2 | DXVK: проблемная сцена, pipeline compile и Present breakdown | Понимание CPU/GPU bottleneck |
| 3 | LSFG: capture source/generated и аудит temporal/resource correctness | Локализованный тип визуального искажения |
| 4 | CPU scheduling A/B | Решение, полезно ли affinity на этом устройстве |
| 5 | Общий regression pass | Звук + DXVK + LSFG совместно, длительная сессия |

## 9. Что считать завершением плана

Работа считается подтверждённой только после:
- отдельного source commit;
- успешной сборки APK владельцем проекта или подтверждённого CI native build;
- повторяемого теста на устройстве;
- сравнения с baseline по релевантным метрикам;
- отсутствия регрессии в остальных подсистемах.

См. также: [DXVK performance A/B test](DXVK_PERFORMANCE_AB_TEST.md), [XAudio2 starvation fix](XAUDIO2_STARVATION_FIX.md), [LSFG GPU profiler](LSFG_GPU_PROFILER.md), [Android runtime tuning map](ANDROID_RUNTIME_TUNING_MAP.md).


## Текущий прогресс реализации — 2 ноября 2026

### Этап 0: изоляция LSFG profiler CSV — реализовано в исходниках

Выполнено:
- CSV теперь открывается с `truncate` при инициализации LSFG host, до проверки runtime LSFG ON/OFF.
- Каждый запуск получает свежий заголовок и строку `SESSION` со статусом `session_started`, `lsfg_disabled` или `profiling_disabled`.
- При смене output path файл также начинается заново, а не дописывает данные другой сессии.
- Запись stage rows по-прежнему opt-in и выполняется только при реальном выполнении соответствующих GPU dispatches.

Это устраняет известное смешивание старого ON CSV с новым OFF запуском. Изменение диагностическое и не ускоряет рендеринг само по себе.

**Статус проверки:** source changes committed; native NDK build и тест на устройстве ещё не выполнены. После сборки сначала проверить, что OFF запуск создаёт CSV только с SESSION marker и не содержит старых stage rows; ON с включённым profiling должен создавать свежие stage rows.

### Следующий этап: аудио

Не вносить ещё одну общую правку XAudio2 на основании только `empty_queue`: последний лог не показал refill-gap и `slow_mix=0`. Сначала нужен воспроизводимый аудиотест и раздельная диагностика xWMA startup latency для музыки, voice и SFX. Большие xWMA decode latency (порядка сотен миллисекунд) — кандидат на исследование, но streaming/partial decode пока не менять без проверки packet ordering, dpds и play-region semantics.


## Обновление после per-dispatch GPU instrumentation — 3 ноября 2026

### Последний source commit

Рабочая ветка: `snap-patch-5`  
HEAD на момент обновления: `2acde0c27b8246a45eab12b29fc4d41dc6cefc8b`  
Commit: [Add per-dispatch LSFG Gamma Delta GPU timings](https://github.com/Snap888/Skyrim_Android/commit/2acde0c27b8246a45eab12b29fc4d41dc6cefc8b)

### Что изменено

В `android/app/jni/lsfg/lsfg_dxvk_generation.cpp` добавлены отдельные GPU timestamp intervals вокруг каждого Gamma и Delta compute dispatch:

- `Gamma_L<level>_S<stage>`
- `Delta_L<level>_S<stage>`

Сохраняются агрегированные `GammaDelta_L0` … `GammaDelta_L6` для сравнения суммы индивидуальных dispatch с прежними измерениями. Level соответствует внешнему уровню generation pyramid; stage — индексу shader dispatch в Gamma/Delta массиве. На L0–L3 выполняется по пять Gamma dispatches; на L4–L6 — пять Gamma и десять Delta dispatches на уровень.

Изменение только диагностическое и opt-in через настройку launcher **LSFG GPU stage profiling (CSV)**. Порядок dispatch, шейдеры, ресурсы, barriers и логика generation не менялись. Подробности формата — в [LSFG GPU profiler](LSFG_GPU_PROFILER.md).

### Следующий тест

1. Собрать APK из указанного HEAD и включить **LSFG GPU stage profiling (CSV)**.
2. Оставить LSFG включённым на x2 и не менять multiplier/flow scale/performance mode во время прогона.
3. Прогреть игру, затем несколько минут воспроизводить одну обычную сцену.
4. Штатно закрыть игру и прислать `port/dxvk_lsfg_gpu.csv`, `dxvk_perf_on.csv` и `lsfg.log`.

В новом CSV проверить, какие конкретно Gamma/Delta dispatch формируют прежний дорогой агрегат L6, и сверить сумму per-dispatch intervals с `GammaDelta_L6`. До получения нового CSV нельзя объявлять конкретный shader stage причиной bottleneck.

**Проверка:** commit и source diff опубликованы; Android NDK build, APK packaging и device runtime test ещё не подтверждены. Следующий этап остаётся измерительным — алгоритмические изменения не вносить до анализа новых timestamps.


## Device capture review — 2026-11-03 00:58

The latest ON-only x2 capture confirms the LSFG command path remains active
through source frame 9000 / present counter 18000. All 300 shader resources
were extracted and translated; SPIR-V reflection passed 300/300. DXVK history
copy completion reached #9000. This is runtime command-path evidence, not
pixel-quality certification.

CPU-side PresentImage benchmark (76 windows, frames 120–9120):
- mean of window averages: 7.112 ms;
- median: 6.555 ms; P95: 11.501 ms;
- isolated maximum `max_cpu_present_ms`: 824.513 ms;
- first ten window averages mean: 6.665 ms; last ten: 10.880 ms.

The late-run increase needs a controlled sustained-load reproduction. Do not
assume thermal throttling or attribute the isolated stall to LSFG GPU compute
without synchronized CPU/GPU traces. The run reports requested/generated
averages close to 1.0, consistent with x2.

The upload contains no OFF benchmark. The GPU CSV (25.7 MB) and runtime
`log.txt` (1.43 MB) exist in Git but their contents were not retrievable via
the current file-content interface, so GPU per-dispatch and audio conclusions
remain open.

### Updated immediate priorities

1. Capture a matched OFF/ON CPU benchmark pair, with GPU profiling disabled
   in both passes and a fixed scene/configuration.
2. Capture a separate short ON-only GPU-profiled run and make the per-dispatch
   Gamma/Delta CSV readable/split into manageable chunks; compare individual
   stages with aggregate `GammaDelta_L6`.
3. Provide a readable runtime `log.txt` (or filtered audio diagnostics)
   before changing XAudio2, xWMA decode scheduling or mixer behaviour.
4. If the late-run Present increase reproduces, correlate per-window CPU
   Present timing with device temperature/clocks, source FPS, scene workload,
   and DXVK thread CPU placement.
5. Keep visual x2 validation separate: inspect fast camera pans, ghosting,
   warped edges, trails and generated-frame pacing.

No optimization is approved solely from this capture. No APK build, measured
FPS gain, audio fix or visual-quality result is claimed.
