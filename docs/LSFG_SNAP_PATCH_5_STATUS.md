# LSFG implementation status — snap-patch-5

## Branch

- Repository: [Snap888/Skyrim_Android](https://github.com/Snap888/Skyrim_Android)
- Working branch: [snap-patch-5](https://github.com/Snap888/Skyrim_Android/tree/snap-patch-5)
- Base: `snap-patch-4`
- Base commit: `c2a1077d631e552896b19da42e405579138e7a2b`
- Current HEAD before this runtime-log update: `ce0dce236e8e37518187ecca4d87b2fb4a462be1`

## Changes

1. `15c70a5` — PE resource parsing now checks complete byte ranges against raw section data and the resource directory. This removes unchecked RVA + size arithmetic before reading resource directories and payloads.
2. `c41de0b` and `25aff4d` — RCDATA blobs are written to temporary files and atomically renamed. The manifest is updated only after a complete blob has been published.
3. `85eef73` — when the effective flow scale changes, temporal warm-up is reset. Newly allocated mipmap and Alpha/Beta resources must receive three successful shared-pass frames before generation resumes.
4. `7a27a16` / `332abe2` — the implementation plan and progress are documented in `LSFG_SNAP_PATCH_5_PLAN.md`.

## Source review notes

- The existing GPU path is retained: source history copy → mipmaps 255 → Alpha 290–293 → Beta 298–302 → Gamma/Delta → Generate 256.
- Compute work remains on the active DXVK command list. No separate Vulkan submit path, CPU readback, or speculative hist-copy pass was added.
- The WinNative source-level pass correspondence recorded in the prior status document remains the reference for the remaining descriptor/resource audit.
- The flow-scale warm-up fix addresses a real temporal-state issue: rebuilding flow/Alpha/Beta resources without resetting the outer warm-up counter could allow Generate to read fresh, not-yet-populated temporal resources.

## Verification

- [x] Branch created from exact `snap-patch-4` HEAD.
- [x] Branch diff checked: six commits ahead, zero commits behind `snap-patch-4` at the time of comparison.
- [x] PE bounds and extraction publication changes reviewed at source level.
- [x] Flow-scale reset path reviewed at source level.
- [ ] Android NDK native build.
- [ ] APK packaging.
- [ ] Vulkan runtime pipeline/descriptor validation.
- [ ] Device visual test at x2, then x4.

No APK build or device test is claimed. The next required runtime artifact is the complete `port/lsfg.log`; include `port/log.txt` and Android logcat if startup, crash, hang, or Vulkan errors occur.


## Runtime performance review and first optimization — 2026-11-02

Latest uploaded device artifacts reviewed from `Logs/`:

- `dxvk_lsfg_gpu.csv`: 23,407 stage rows, all status `ok`, covering 2,130
  frame groups. Recorded stage sum: avg 7.196 ms, median 6.178 ms, p95
  10.475 ms, max 11.509 ms.
- Highest measured stage: `GammaDelta_L6`, avg 2.082 ms; next are Alpha
  (1.232 ms), Mipmaps (0.987 ms), Generate (0.930 ms), and Beta (0.874 ms).
- `dxvk_perf_on.csv` contains only the LSFG-ON pass. It reports CPU-side
  PresentImage windows, not full frame time. No matching LSFG-OFF pass is
  present, so no quantitative ON/OFF performance conclusion can be made.
- The current test config uses multiplier 2, flow_scale 1.00 and
  performance 0 (Performance mode); the runtime log reports a 0.75 flow
  extent scale (1836x810 from 2448x1080).
- The profiler CSV's successful timestamps show that the instrumented GPU
  stages completed for the sampled command lists. They do not validate
  generated pixel quality or prove sustained device stability.

First optimization: GPU timestamp profiling is now disabled by default and
can be enabled with `DXVK_LSFG_GPU_PROFILE=1`. This removes diagnostic query
objects, timestamp writes, completion callbacks and per-stage CSV file writes
from normal gameplay. It does not alter LSFG shaders or pass order.

Next optimization target is GammaDelta L6, but no compute stage has been
removed, fused or reordered: its shader dependencies and reference WinNative
topology must be audited before algorithmic changes. A separate LSFG-OFF
benchmark is required for a valid DXVK A/B comparison.

Verification pending: Android NDK build, APK packaging and device retest of
the profiler-disabled path. No FPS improvement or stability fix is claimed
until measured on device.


## Runtime test after profiler opt-in change — 2026-11-02

New user-uploaded artifacts from `Logs/`:

- `dxvk_perf_off.csv`: 21 windows, source frame markers 120–2520.
- `dxvk_perf_on.csv`: 15 windows, source frame markers 120–1800.
- Both runs report source extent 2448x1080 and VSync=1.
- OFF reports `avg_lsfg_requested=0` and `avg_lsfg_generated=0`; ON reports
  both values as 1 in every sampled window.
- Arithmetic mean of the per-window `avg_cpu_present_ms` values: OFF
  5.137 ms, ON 9.918 ms. Median of those window averages: OFF 6.536 ms,
  ON 12.001 ms.
- Maximum per-window `max_cpu_present_ms`: OFF 17.554 ms, ON 838.957 ms.
  The ON run also contains large isolated maxima (for example 468.271 ms and
  838.957 ms); these should be treated as outliers/stalls requiring
  investigation, not as representative frame time.
- `lsfg.log` confirms an ON session (`enabled=1`, multiplier 2), 300/300
  SPIR-V reflection entries successful, active DXVK command-list path, and
  command-list completion counter reaching 1800. A later session in the same
  file starts with `enabled=0`; do not conflate it with the ON run.
- `Logs/log.txt` is empty in this upload.
- No `dxvk_lsfg_gpu.csv` is present. Since GPU profiling is opt-in, this run
  does not provide updated per-stage GPU timings.

Interpretation: ON has a higher CPU-side PresentImage window average in this
sample, but the two CSVs have different sample counts/durations and the metric
is not full frame time or FPS. The isolated ON maxima are not enough to
diagnose their cause. This is evidence of additional CPU-side cost/stalls in
the ON run, not a measured FPS delta or proof that a specific GPU pass is the
cause.

Next test: repeat a controlled ON/OFF pair in the same scene and duration,
with the profiler disabled for the ordinary CPU benchmark. Then do a separate
short ON-only diagnostic run with `DXVK_LSFG_GPU_PROFILE=1` to collect the
GPU-stage CSV. Keep the two measurement modes separate.


## Launcher-controlled LSFG GPU profiling — 2026-11-02

The launcher now exposes an opt-in **LSFG GPU stage profiling (CSV)** checkbox.
When enabled, it sets `DXVK_LSFG_GPU_PROFILE=1` before starting Skyrim,
resets `port/dxvk_lsfg_gpu.csv`, and supplies its absolute path through
`DXVK_LSFG_GPU_PROFILE_LOG`. The profiler prioritizes this explicit path.
When disabled, both variables are unset. The selected state and output path
are recorded in `port/lsfg_gpu_profile.conf`. Profiling remains OFF by default.

## Latest device log review — 2026-11-02

The newest `Logs/` upload contains a valid GPU profiler CSV (33,467 rows,
all `ok`). GammaDelta L6 is the largest measured stage at 1.955 ms average
and 3.030 ms P95. The full Gamma/Delta/Generate chain averages 3.851 ms;
Mipmaps/Alpha/Beta average 2.955 ms per source-frame group. The run records
3,247 generated chains over 2,497 source-frame groups, and its measured stage
sum is approximately 7.963 ms per source frame. These are GPU timestamp
intervals, not full frame time or FPS.

The same session changed multiplier x2 -> x4 -> x2. Its ON CPU PresentImage
CSV also ran with GPU profiling enabled, so it is not a controlled OFF/ON
comparison. Do not infer a production LSFG regression from that comparison.

The GPU profiler CSV writer has been changed to keep its output file open and
flush every 60 rows, avoiding a filesystem open/probe/close for each stage.
This is an instrumentation optimization; it does not alter rendering or the
generation algorithm. Native build and device validation remain pending.

### Sound review

The latest `Logs/log.txt` contains no `refill gap` events and reports
`slow_mix=0`; the SDL output queue was low in one reporting interval only.
There are 88 per-second source-starvation summaries, but these counters count
missing source frames, not discrete audible glitches. Only the initial large
xWMA decode shows a substantial delay (282 ms); later sampled worker jobs are
typically 1–4 ms. No mixer rewrite is justified by this log alone. The
remaining audio investigation is focused on startup/large-buffer xWMA decode
and on reproducing any audible crackle with a matching log.

### Next steps

1. Build and test the profiler-writer change.
2. Run a clean LSFG ON/OFF pair with GPU profiling disabled and identical
   multiplier, scene, duration, resolution and VSync.
3. Run a separate short ON-only GPU profile at x2; inspect L6 before changing
   shader math or WinNative pass order.
4. Keep audio changes evidence-driven; collect logcat and note the exact
   audible event if crackles persist.

## WinNative paired-pass order correction — 2026-11-02

Source comparison against `WinNative/app/src/main/cpp/winlator/vk/lsfg/lsfg_chain.cpp`
found that the DXVK generation loop previously dispatched all five Gamma
stages before the ten Delta stages on L4–L6. WinNative interleaves paired
stages: Gamma 0, Delta 0, Gamma 1, Delta 1, through Gamma 4, Delta 4, then
Delta 5–9.

Commit `77b76cf` changes `dispatchLsfgGeneration()` to match that exact
dispatch order. Levels without Delta continue to dispatch their five Gamma
stages in order. DXVK remains responsible for image-hazard tracking and
barriers; no direct Vulkan queue submission or CPU wait was introduced.

This corrects a source-level pass-order mismatch. It is not yet a measured
performance improvement, and it does not complete the descriptor/temporal
producer-consumer audit. Native build, Vulkan validation and device image
quality remain pending.

## Alpha command-recording optimization — 2026-11-02

Commit `ceb481b` moves the Alpha sampler and compute-shader binds outside
the per-level loop. The shared Alpha path already iterates stage-first and
then dispatches all seven pyramid levels, matching WinNative's
`DispatchShared()` structure. DXVK's `bindShader()` marks the compute
pipeline dirty unconditionally, so rebinding the same Alpha shader for every
level forced unnecessary pipeline-state recommits.

The shader and resource bindings are unchanged; per-level image views are
still rebound before each dispatch, and DXVK continues to track hazards.
This is a CPU-side command-recording optimization. Its impact must be measured
in a profiler-disabled ON/OFF test; no FPS gain is claimed yet.


## 2026-11-02 — DXVK pipeline-state binding optimization

The refreshed GPU profile contains 18,354 successful timestamp rows over source
frames 0–1669. Stage averages (ms): Mipmaps 0.993, Alpha 1.234, Beta 0.871,
GammaDelta L6 2.085 (P95 3.031), Generate 0.906. GammaDelta L6 remains the
largest individual group; its timer includes the five Gamma and ten Delta
dispatches for the full-resolution flow level.

The ON/OFF CPU Present CSVs are not a controlled A/B comparison (different
sample counts and run conditions), so they are not used to claim a precise
regression or gain. The ON run reports one generated frame per source frame,
consistent with multiplier x2.

DXVK `DxvkContext::bindShader` previously dirtied the pipeline, pipeline
state and descriptor resources on every call, even when the exact same shader
was already bound. It now returns early for an unchanged shader object. This
preserves all dirty flags when a real shader change has already occurred and
does not bypass image hazard tracking. It reduces redundant CPU-side pipeline
invalidation for repeated state binds; GPU shader math and dispatch order are
unchanged. Device/build validation is still required.

Audio: after retrieving the full `Logs/log.txt` blob, the log shows xWMA
worker latencies of 237 ms, 28 ms and 53 ms for early stereo streams, plus
later 24–28 ms jobs. `pending_decode` frames are nonzero, while
`slow_mix=0` and no explicit refill-gap event appears. The separate audio
update adds up to two xWMA decode workers to reduce head-of-line blocking
between independent voices; per-voice queue order is preserved.


## Latest minimum-graphics runtime review — 2026-11-02

The newest Logs upload contains 203,905 successful GPU timestamp rows across
source frames 0–13,690. The run is not a single LSFG configuration: the log
shows runtime changes to flow scale (0.75, 0.30, 0.20, 0.50, 1.00, 0.75,
0.50, 0.20 and 0.40), multiplier (2x/3x/4x) and performance mode. Therefore
whole-file averages are descriptive only and must not be treated as an A/B
comparison with the previous high-graphics run.

Whole-file stage averages (ms): Mipmaps 0.430, Alpha 0.481, Beta 0.334,
GammaDelta L0 0.047, L1 0.044, L2 0.045, L3 0.047, L4 0.146, L5 0.274,
L6 0.799, Generate 0.755. GammaDelta L6 remains the most expensive
individual generation group. Its P95 is 1.738 ms and max is 10.535 ms.
These numbers mix multiple flow scales and generation multipliers; they are
not a direct comparison against the previous high-graphics run. The separate
ON/OFF CPU Present CSVs also have different sample counts and run windows.

Audio findings: the two initial large stereo xWMA jobs took 193 ms and
270 ms on the worker path, with pending-decode starvation summaries of 10,081
and 12,348 frames. A later large stereo job (submit #150) took 259 ms.
There are two slow-mix reports (14 ms and 22 ms) and two measured 49/54 ms
refill gaps classified as EOS. Empty-queue frame totals are not by themselves
audible underruns. The two-worker change does not eliminate the long decode
latency of a single large stream; startup buffering/voice readiness remains
an audio investigation target. No audible improvement is claimed from logs
alone.

### DXVK unchanged-resource bind fast path

Commit `5922955` adds early-outs to `DxvkContext::bindResourceView()` and
`bindResourceSampler()` when the exact same view/sampler objects are already
bound. A real binding change still clears `m_rcTracked` and marks compute and
graphics resources dirty. An unchanged binding keeps the existing command-list
resource tracking state, avoiding redundant descriptor invalidation and
resource re-tracking during the repeated LSFG dispatch sequence. This is a
CPU-side command-recording optimization; it does not change shader math,
GPU dispatch count, image barriers or the WinNative Gamma/Delta order. Device
build and performance validation remain pending.

### Next validation

1. Build the current branch and test audio startup, dialogue/music overlap,
   and crackle/dropout behavior; the large-stream decode delay is still visible.
2. For GPU comparison, capture separate fixed runs at one multiplier, one
   flow scale, one performance mode and one scene. Do not change LSFG settings
   during a CSV capture.
3. Keep profiler-disabled CPU ON/OFF runs separate from short GPU-profiled
   runs. Match duration and scene for ON/OFF.
4. No APK build, native build, device validation or measured performance gain
   is claimed for commit `5922955`.

## Latest runtime review and DXVK buffer-binding fast path — 2026-11-02

The newest uploaded run is on branch `snap-patch-5` at the time of review.
The GPU CSV contains 130,463 successful stage rows across 10,277 source-frame
groups. Stage timings vary by the actual input extent, so the following
full-resolution / reduced-resolution buckets are kept separate:

- Full-resolution `GammaDelta_L6` (2448 wide): 12,454 samples,
  average 1.419 ms, P95 2.277 ms, max 6.431 ms.
- Full-resolution `GammaDelta_L5`: average 0.436 ms.
- Full-resolution `Generate`: average 0.808 ms.
- `Alpha` at 1836 wide: average 1.079 ms; `Beta` at 1836 wide:
  average 0.766 ms.
- Additional Alpha/Beta samples exist at widths 1224, 734 and 489,
  reflecting runtime flow-scale changes. They are not pooled with 1836.

The LSFG session changes multiplier (1x–4x), flow scale (0.40 and 1.00)
and performance mode while recording. The GPU numbers above describe this
mixed session, not a fixed-mode comparison with prior uploads. The CPU
ON/OFF CSVs also differ substantially in sample count and runtime window;
they are not a valid A/B pair.

### Process exit and audio findings

`last_exit.txt` reports Android `EXCESSIVE_RESOURCE_USAGE` with
`EXCESSIVE CPU USAGE` (CPU 20 over roughly 18 minutes). The end of
`log.txt` records `quit requested`, followed by
`FORTIFY: pthread_mutex_lock called on a destroyed mutex` and SIGABRT.
This establishes a teardown-time mutex-lifetime failure after the quit
request, but the available native backtrace contains only `abort()`; it
does not identify the owning mutex or prove that XAudio2 caused it.
Do not treat this as a confirmed XAudio2 fix target until a symbolized
logcat/native tombstone identifies the caller.

The final CPU sample reports 228% aggregate across one-core-normalized
threads, including an SDLThread sample at 86%. The guest thread sample
shows a very high `InterlockedDecrement` call count, but sampling data alone
does not establish whether that is a hot spin loop or expected guest
synchronization. This needs a focused trace before changing scheduling or
spin-wait behavior.

The xWMA log now contains many short mono 44.1 kHz voice decodes, as well
as a 21.184-second stereo 32 kHz stream (voice #127, 56 packets). Repeated
`empty_queue` summaries with `pending_decode=0` show source queues empty
between buffers; they do not alone prove audible underruns. FFmpeg also
reports `Could not update timestamps for skipped samples` on multiple
WMA2 packets. Keep decoder timing, EOS and queue-empty counts distinct.

### DXVK identical buffer-slice binding optimization

Commit `d1b8fdd` adds an early return to
`DxvkContext::bindResourceBuffer()` when the exact buffer slice is already
bound and no image or buffer view is active in that slot. It uses
`DxvkBufferSlice::matches()`, which compares buffer identity, offset and
length. The view-type guard preserves transitions from image/buffer views;
a changed range or buffer continues through the existing dirty-state logic.

This avoids marking descriptor-binding state dirty for repeated identical
buffer binds (including the LSFG shared buffer bindings). It does not alter
descriptor contents, barriers, shader math, dispatch topology or pass order.
It is a CPU-side command-recording optimization; native build and device
performance validation are still required. No FPS or GPU-time gain is
claimed.

### Next validation

1. Build and test the current branch; watch for startup, shutdown and
   audio-device lifecycle errors.
2. For CPU performance, capture a fixed-scene LSFG ON/OFF pair with
   identical duration, multiplier, flow scale, performance mode, resolution
   and VSync; keep GPU profiling disabled for both.
3. Capture a separate short GPU-profiled run with one fixed LSFG configuration.
4. If the destroyed-mutex crash recurs, include Android logcat/native
   tombstone with symbols and the lines immediately before `quit requested`.

## Runtime review and CPU fast paths — 2026-11-02 (22:41 session)

### Current log interpretation

- The new `Logs/lsfg.log` session starts at 22:41:14 and runs through at least
  22:47:58. The user changed multiplier (1x–4x), flow setting (effective
  scales 0.20–0.75), and performance mode repeatedly during this capture.
  Treat it as a functional stress run, not a controlled performance A/B.
- Each effective flow-scale change recreates the flow-dependent mipmap and
  Alpha/Beta resources, recreates Gamma/Delta/Generate resources, and restarts
  temporal warm-up. Avoid changing flow scale during a timed comparison.
- The log records the DXVK command-list path, completed history copies,
  mipmaps dispatches, and Generate commands. At 4x it records three generation
  commands per source frame; at 3x, two; at 1x/2x, one. This confirms command
  recording activity, not visual quality or successful GPU completion for
  every generated frame.
- The current `Logs/last_exit.txt` reports Android
  `EXCESSIVE_RESOURCE_USAGE / EXCESSIVE CPU USAGE` at 21:11:35, with CPU 20
  over 300062 units during 1084075 ms. This timestamp precedes the 22:41 LSFG
  session and must not be attributed to that session without a matching
  process/logcat trace.
- The GitHub file-content endpoint returned empty content for the new
  `Logs/log.txt` and `Logs/dxvk_lsfg_gpu.csv` blobs (their blob IDs are
  present, but their contents were not available to this analysis). Do not
  infer new GPU timings or audio underrun counts from those files until they
  can be read.

### DXVK repeated vertex/index binding fast paths

- `bindIndexBuffer()` now returns when the exact buffer slice and index type
  are unchanged. A changed slice or index type still follows the original
  dirty-state path.
- `bindVertexBuffer()` now normalizes the stride first, then returns when the
  exact buffer slice and effective stride are unchanged. Buffer identity
  changes still invalidate the tracked binding; changed ranges/strides still
  mark the required state dirty.
- These are CPU command-recording optimizations only. They do not change
  descriptors, barriers, shaders, GPU dispatches, LSFG flow scale, or pass
  order.

### XAudio2 mixer routing fast paths

- `route()` now bypasses construction and traversal of a full channel matrix
  for the default equal-channel route (identity matrix), multiplying each
  channel by its existing voice/channel gain directly.
- Default mono-to-stereo routing now duplicates the mono sample directly.
- Custom output matrices and all other channel layouts continue through the
  original generic matrix path. The default routing coefficients and gain
  semantics are preserved.
- This reduces per-pass CPU work in common voice/submix routes; no measured
  audio latency or CPU reduction is claimed until a same-scene device test.
- The mixer now buckets active submix voices by processing stage using one
  registry scan and a compact prefix-offset array. This replaces the former
  64-stage × 1024-slot nested scan (up to 65,536 registry checks per pass)
  while preserving ascending voice-slot order within each processing stage.

### Validation status and next test

- Neither the Android APK nor device runtime was built/tested in this change.
- Run a fixed-scene test without changing multiplier, flow scale, performance
  mode, resolution, or VSync during capture. Use one short GPU-profiling run
  and a separate longer CPU/audio run.
- For audio validation, capture voice counts, xWMA worker latency, pending
  decode starvation, non-EOS refill gaps, SDL queue depth, and mixer-pass
  duration in the same log. A teardown tombstone with symbols is still needed
  if the destroyed-mutex abort recurs.


## Комплексный runtime review и план оптимизации — 2026-11-02

Последняя пара тестов LSFG ON/OFF:

| Метрика CPU Present | OFF | ON |
|---|---:|---:|
| Среднее оконных средних | 1,64 мс | 16,64 мс |
| Медиана оконных средних | 0,86 мс | 10,96 мс |
| Минимум | 0,09 мс | 3,34 мс |
| Максимум | 4,94 мс | 31,48 мс |

Медиана здесь лучше описывает типичное измеренное окно, так как среднее чувствительно к всплескам. Но CPU Present не является полным временем кадра и не равен FPS. Пользователь также отмечает конкретную игровую локацию с сильной просадкой Game FPS, которая по игровому графику меньше при LSFG ON. Требуется повторяемый тест в одной сцене с отдельными source FPS, output/FG rate, CPU Present и GPU timings.

### Уточнение по артефактам

- OFF подтверждён конфигурацией `lsfg.enabled=0`; requested/generated равны нулю.
- `dxvk_lsfg_gpu.csv` в последней загрузке не является результатом OFF: при выключенном LSFG GPU pass не исполнялись. Файл содержит данные предыдущей ON-сессии, вероятно из-за отсутствия очистки/маркировки сессии.
- Приоритетная диагностическая задача — очищать/создавать profiler CSV для каждой сессии, включая OFF, и добавлять session/run ID + config snapshot.

### Shutdown

Последовательность `quit requested` → FORTIFY destroyed mutex → signal 6 появилась после ручного закрытия приложения. По указанию владельца проекта не считать её ошибкой игры и не включать в текущие оптимизационные задачи. Анализировать заново только при самопроизвольном завершении.

### Новые направления работ

1. Аудио: музыка, голоса, эффекты; локализация XAudio2 queue starvation, xWMA decode/refill и проверка уже реализованного параллельного decode.
2. DXVK: воспроизведение локальной просадки FPS, разбор синхронной pipeline compilation и разложение Present path на отдельные стадии.
3. LSFG: визуальные артефакты при быстрых движениях; проверка source history, timestamps, flow/motion/occlusion, resource lifetime и pass dependencies.
4. CPU scheduling: измерить топологию и affinity реальных потоков; провести осторожный A/B с приоритетами/affinity. GPU compute не переносится на CPU cores; эксперимент касается CPU-side command recording/submission и worker scheduling.
5. Regression: после каждого изменения отдельная сборка и device A/B, не менять несколько подсистем одновременно.

Подробный порядок, ограничения, тестовая матрица и acceptance criteria находятся в [OPTIMIZATION_PLAN_SNAP_PATCH_5.md](OPTIMIZATION_PLAN_SNAP_PATCH_5.md).

Этот раздел фиксирует анализ и план. Он не означает, что новые оптимизации реализованы, APK собран или результаты подтверждены на устройстве.


## Реализация этапа 0 — изоляция LSFG profiler CSV (2026-11-02)

В `snap-patch-5` внесено source-level изменение:
- LSFG profiler CSV сбрасывается на каждом запуске до runtime enable gate;
- OFF/disabled session получает явный `SESSION` marker;
- stage callback использует общий сериализованный writer и начинает новый файл при смене пути;
- старые ON samples не должны сохраняться в следующем OFF CSV.

Это исправление достоверности диагностики, не оптимизация FPS. Требуется APK rebuild и проверка: OFF CSV должен содержать только заголовок + `SESSION,...,lsfg_disabled` (или `profiling_disabled`, если профилировщик выключен), а новый ON profiling run — новый набор stage samples.

Аудиокод пока не менялся: предыдущий лог не подтвердил refill gaps, `slow_mix=0`; сначала требуется повторяемый тест на слышимые разрывы и связь с долгим xWMA startup decode.


## Per-dispatch Gamma/Delta GPU profiling — 2026-11-03

Current branch HEAD: `2acde0c27b8246a45eab12b29fc4d41dc6cefc8b`.

Commit [2acde0c](https://github.com/Snap888/Skyrim_Android/commit/2acde0c27b8246a45eab12b29fc4d41dc6cefc8b) adds individual GPU timestamp intervals around every Gamma and Delta dispatch in `android/app/jni/lsfg/lsfg_dxvk_generation.cpp`.

New CSV stage names:

- `Gamma_L<level>_S<stage>`
- `Delta_L<level>_S<stage>`

The existing `GammaDelta_L0` … `GammaDelta_L6` aggregate rows remain enabled, allowing direct comparison of the per-dispatch sum with each level aggregate. Level follows the outer generation-pyramid loop. Stage is the shader-array index passed to the dispatch helper. L0–L3 contain five Gamma dispatches each; L4–L6 contain five Gamma and ten Delta dispatches each.

Instrumentation is opt-in through the launcher setting **LSFG GPU stage profiling (CSV)**. Dispatch order, shaders, resources, barriers and generated-frame logic are unchanged. No extra profiling is performed when the setting is OFF.

### Next device validation

Build the APK from this HEAD, enable the profiling checkbox, run LSFG ON at x2 with a fixed configuration, warm up the scene and capture several minutes. After a clean exit, provide:

- `port/dxvk_lsfg_gpu.csv` — per-dispatch and aggregate GPU timings;
- `dxvk_perf_on.csv` — CPU-side PresentImage windows from the same run;
- `lsfg.log` — runtime configuration and dispatch/completion diagnostics.

The next analysis will identify which Gamma/Delta dispatches dominate level 6 and compare their summed durations with `GammaDelta_L6`. Existing aggregate profile identifies L6 as the largest group, but does not identify its individual expensive shader dispatch.

### Verification state

- [x] Source changes committed to `snap-patch-5`.
- [x] Branch HEAD and commit contents verified on GitHub.
- [ ] Android NDK native build.
- [ ] APK packaging and installation.
- [ ] Device run with per-dispatch CSV.
- [ ] Runtime timing and visual validation.

No successful native build, APK or device validation is claimed.
