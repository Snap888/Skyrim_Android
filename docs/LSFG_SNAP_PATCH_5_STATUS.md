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
