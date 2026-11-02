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
