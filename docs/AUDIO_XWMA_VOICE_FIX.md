# Skyrim Android — исправление голоса и музыки (xWMA / XAudio2)

Документ описывает путь интеграции FFmpeg-декодера xWMA, правки runtime и **итоги тестов** на устройстве (Adreno / SDL).

**Ветка:** `snap-patch`  
**Основные файлы:**

| Файл | Назначение |
|------|------------|
| `runtime/xaudio2.c` | XAudio2 shim (SDL output, SubmitSourceBuffer, форматы) |
| `runtime/xwma_ffmpeg.c` | Декод WAVE_FORMAT_WMAUDIO2/3 (0x0161 / 0x0162) через FFmpeg |
| `runtime/kernel32.c` | BSA open cache, правка пути `DATA/DATA` |
| `android/app/jni/ffmpeg/` | Prebuilt `libavcodec` / `libavutil` (wmav2 + wmapro) |
| `docs/FFMPEG_XWMA.md` | Сборка FFmpeg и CMake/Gradle |

---

## 1. Контекст проблемы

Skyrim (LE) отдаёт **голоса и музыку** в XAudio2 как **xWMA** (сжатый WMA в `SubmitSourceBuffer` + `XAUDIO2_BUFFER_WMA`).

На Android:

- Нет нативного XAudio2.
- Runtime эмулирует XAudio2 поверх **SDL2**.
- Без декодера xWMA → **тишина**.
- С «наивным» FFmpeg → часто **треск / шум** вместо речи.

`Voices.bsa` / `VoicesExtra.bsa` / `Sounds.bsa` открываются нормально.  
Лифтер/`gen/` **не** перечисляет `.fuz` — это data, не code. Голоса доходят до XAudio2 (в logcat есть `source format xWMA`).

---

## 2. Архитектура решения

```
TESV (gen/)  →  XAudio2 CreateSourceVoice / SubmitSourceBuffer
                    ↓
              runtime/xaudio2.c
                    ↓ tag 0x0161/0x0162
              runtime/xwma_ffmpeg.c  →  libavcodec (WMAV2/WMAPRO)
                    ↓ int16 PCM
              микшер XAudio2 → SDL QueueAudio (S16 stereo)
```

### Сборка

```bash
# FFmpeg arm64 (см. docs/FFMPEG_XWMA.md и workflow build-ffmpeg-android.yml)
# → android/app/jni/ffmpeg/{include,lib/arm64-v8a}

cd android
./gradlew assembleDebug -Pffmpeg=true
```

CMake включает `LB_HAS_FFMPEG`, если найдены заголовки/библиотеки.

**Важно:** `runtime/xaudio2.c` на git иногда превращался в заглушку (`#error`, ~212 байт).  
Рабочий файл **~36–40 KB**. Перед сборкой:

```bash
wc -c runtime/xaudio2.c   # не 212
grep -c '#error' runtime/xaudio2.c   # 0
```

### CI (GitHub Actions)

Workflow `Build & Publish APK` требует каталог **`gen/`** (не в git).  
При запуске указать **`gen_url`** — прямую ссылку на `gen.tar.xz` / `.zip`.  
Без `gen_url` шаг `Restore / download gen/` падает.

---

## 3. Хронология правок

### 3.1. Базовый путь FFmpeg

- Декод в `xwma_decode()` при `SubmitSourceBuffer` для `FMT_XWMA`.
- Формат из `WAVEFORMATEX` (tag, channels, rate, `nBlockAlign`, `nAvgBytesPerSec`, optional extradata).
- Таблица пакетов: `XAUDIO2_BUFFER_WMA.pDecodedPacketCumulativeBytes` + `PacketCount`.

### 3.2. Разблокировка микшера

Тяжёлый FFmpeg **не** должен держаться под `g_lock` микшера (underrun → треск).  
Decode выполняется вне лока; в очередь кладётся уже PCM.

### 3.3. SDL output

| Было | Стало |
|------|--------|
| `AUDIO_F32SYS` | **`AUDIO_S16SYS`** (стабильнее на Android) |
| samples 1024 | 2048 |
| PASS ~10–15 ms | ~20 ms |
| жёсткий clip | soft headroom ~0.85 |

Лог при успехе:

```text
xaudio2: SDL audio ok freq=48000 fmt=S16 ch=2 samples=2048
```

### 3.4. Путь `DATA/DATA`

Игра иногда запрашивает `C:\Skyrim\DATA\DATA\SOUND\...`.  
В `rt_host_path` добавлено сжатие `Data/Data` → `Data`.  
На голоса из BSA влияет слабо (основной путь — архив).

### 3.5. Стратегия decode (главный источник треска)

**Проблема:** `WAVEFORMATEX.nBlockAlign` (часто **2230** или **1487**) **не совпадает** с реальными размерами xWMA-пакетов (46…1594).

```text
[wmav2] Input packet size too small (820 < 2230)
[wmav2] overflow (...) in spectral RLE
→ PCM = шум → «сплошной треск», голоса не слышны
```

**Итоговое поведение `xwma_ffmpeg.c`:**

1. **effective `block_align`** — минимум размера пакета из таблицы (≥ 32), а не слепо fmt BA.
2. Порядок попыток:
   - **whole** — один буфер целиком;
   - **packets** — каждый пакет таблицы с effective BA (пропуск `< 32`);
   - **sliced** — нарезка по effective BA.
3. **`duration_ok`:**
   - короткие клипы (`< 16 KB`): достаточно ≥ 1024 сэмплов (кадр WMA ~2048);
   - большие потоки: не меньше **~25%** ожидаемой длительности по `avg_bps`, иначе **reject** (тишина лучше шума).

---

## 4. Итоги тестов на устройстве

Тесты по logcat (`adb logcat -s skyrim-xaudio2` и общий лог с `xwma_ffmpeg`).

### 4.1. Что подтверждено

| Наблюдение | Вывод |
|------------|--------|
| `CreateFileA ... Voices.bsa -> fd …` | BSA на месте, не «забыты в декомпиляторе» |
| `source format xWMA tag=0161 … decoder FFmpeg` | Голоса доходят до XAudio2 |
| `via whole` на клипах 3–15 KB → 2048…14336 frames | Короткий decode отрабатывает |
| `block_align is not set` / `avcodec_open2 EINVAL` | BA **обязан** быть при `open` |
| `Input packet size too small` + `overflow` на 2ch music | Неверный BA / packet feed → **шум** |
| `merged-pkts` / naive packets при BA=2230 | Даёт сотни тысяч «сэмплов» шума, маскирует голоса |
| `size=2974 → 2048 frames` reject при пороге 50 ms | Ложные FAILED на валидных коротких репликах |
| `xaudio2.c` = 212 байт `#error` | Полная тишина до замены файла |
| SDL F32 vs S16 | На части устройств F32 давал нестабильность |
| `pthread_mutex … destroyed` на quit | Отдельный crash при выходе, не причина треска в игре |

### 4.2. Типичные строки «хорошо»

```text
xaudio2: build: ... decoder=FFmpeg WMAV2
xaudio2: SDL audio ok ... fmt=S16 ...
xaudio2: source format xWMA tag=0161 1 ch 44100 ...
xaudio2: xWMA buffer: 5948 bytes -> 6144 frames (1 ch, 44100 Hz) via whole tag=0161
```

### 4.3. Типичные строки «плохо» (музыка / шум)

```text
[wmav2] Input packet size too small (46 < 2230)
[wmav2] overflow (.... > 1864) in spectral RLE
xwma_ffmpeg: merged-pkts sent=0 samples=466944 size=642240
```

Или недодекод:

```text
size≈1 MB → frames≈51200   # ~1 s вместо минут → должен reject
```

### 4.4. Текущий статус (после серии патчей)

| Цель | Статус |
|------|--------|
| FFmpeg связан, путь xWMA жив | **Да** |
| Короткие голоса decode (log `via whole`) | **Да** |
| Нет ложного reject на 2048-sample клипах | **Да** (ослаблен duration для small size) |
| Музыка без треска | **Частично / нет** — WMA всё ещё чувствителен к BA и packet layout; при плохом decode лучше silence (reject), но качество 2ch xWMA **не доведено до «как на PC»** |
| Субъективно «голоса чисто, без треска» | **Не достигнуто** стабильно — пользователь: «голосов нет, сплошной треск» даже после effective BA |
| Crash mutex на quit | **Не чинился** в этой ветке работ |

**Вывод:** пайплайн «файл → XAudio2 → FFmpeg → SDL» работает; **качество WMA-decode** (особенно длинные 2ch буферы) остаётся узким местом. Голоса в логе часто OK, но субъективно перекрываются/портятся шумом от параллельных стримов или всё ещё дефектным PCM.

---

## 5. Ограничения и следующие шаги

### Ограничения

- FFmpeg **WMAV2** плохо переносит несовпадение `block_align` и реальных пакетов xWMA.
- `extra=0` почти всегда — нет codec extradata из WAVEFORMATEX.
- Нет отдельного demuxer’а **xWMA** (как в libavformat) — только raw packet feed.
- Тяжёлый decode на UI/submit-потоке всё ещё может давать краткие глитчи под нагрузкой (CPU profile ~200%+ одного ядра).

### Возможные следующие шаги

1. **Демux xWMA через libavformat** (кастомный probe + packetize) вместо ручной таблицы.
2. Сравнение с рабочим `libmain.so` из другого APK (RE) — иные параметры open/feed.
3. Отложенный decode (worker queue), чтобы не блокировать submit.
4. Временно **глушить 2ch music xWMA** (`channels==2 && size > N → silent`), оставить 1ch voices — проверка гипотезы «шум музыки маскирует голоса».
5. Фикс lifecycle: не брать `g_lock` после destroy engine (quit crash).

### Быстрая изоляция на тесте

```bash
# В игре / ini по возможности убавить music volume
# LSFG выключить (CPU)
adb logcat -s skyrim-xaudio2 | grep -E 'via |FAILED|effective|SDL audio'
```

Если в логе только `via whole` на 1ch и **нет** `Input packet size too small`, а треск остаётся — копать микшер/SDL/routing, не packet feed.

---

## 6. Чеклист перед сборкой

- [ ] `runtime/xaudio2.c` ≥ ~35 KB, без `#error`
- [ ] `runtime/xwma_ffmpeg.c` с `effective_block_align` / `duration_ok`
- [ ] `android/app/jni/ffmpeg/lib/arm64-v8a/libavcodec.so` (+ avutil)
- [ ] `-Pffmpeg=true` / CMake: `xWMA: FFmpeg enabled`
- [ ] CI: задан `gen_url` при отсутствии `gen/` на runner
- [ ] На устройстве: `Data/Skyrim - Voices.bsa`, `VoicesExtra.bsa`, `Sounds.bsa`

---

## 7. Ключевые коммиты (ориентиры на snap-patch)

История на ветке менялась; ориентиры по смыслу:

- FFmpeg path + short whole-buffer for voices  
- Always set `block_align` (fix EINVAL)  
- Accept 2048-sample voice frames  
- Never feed undersized packets / merge policy  
- **effective BA from packet table + reject under-decoded music**

Актуальный tip:

```text
https://github.com/Snap888/Skyrim_Android/commits/snap-patch/runtime/xwma_ffmpeg.c
https://github.com/Snap888/Skyrim_Android/commits/snap-patch/runtime/xaudio2.c
```

---

## 8. Краткий итог одной фразой

**Голоса технически декодируются FFmpeg’ом и доходят до микшера; устойчивого чистого звука без треска (особенно с музыкой xWMA) добиться не удалось — главная причина: несовместимость типичного `nBlockAlign` Skyrim xWMA с ожиданиями FFmpeg WMAV2 и повреждённый PCM на длинных 2ch буферах.**
