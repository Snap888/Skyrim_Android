# XAudio2 buffer starvation fix — snap-patch-5

## Log analysis

The previous runtime log showed repeated `pending_decode` and `empty_queue`
starvation reports. Source inspection found that the counters are accumulated
per requested source frame, not per underrun event. Large values therefore
represent missing source frames over the reporting interval; they are not a
count of discrete glitches.

The main refill timing issue was in `runtime/xaudio2.c`: source-buffer end
events were collected while the full 10 ms mix block ran, and game callbacks
were invoked only after the entire block had been mixed. If a short voice
buffer ended partway through that block, its `OnBufferEnd` callback could not
submit the next buffer until the block had already rendered the remaining
frames as silence.

## Changes

- Dispatch queued voice callbacks at the exact `OnBufferEnd` boundary.
- Release `g_lock` while invoking callbacks, preserving the existing
  re-entrant callback contract. The completed buffer is retired before
  `OnBufferEnd` runs, so the callback can safely submit its replacement.
- Keep other voice events ordered in the same event queue; pending events are
  dispatched together when a buffer boundary is reached or at pass end.
- When an xWMA buffer is queued but its asynchronous decode is not yet ready,
  allow a bounded wait of at most 4 ms per 10 ms engine pass. The condition
  wait releases `g_lock`, allowing the decoder worker to publish PCM.
- Rename the internal starvation counters to make their frame-count semantics
  explicit and label the log line `source starvation frames`.
- Add per-source-voice refill timing: timestamp `OnBufferEnd`, then measure
  the interval until the next `SubmitSourceBuffer` on that voice.
- Log measured refill gaps of at least 2 ms (and EOS-associated resubmissions),
  with per-voice totals and thresholds at 5 ms / 10 ms.
- Keep normal one-shot completion separate: if a buffer ends and no replacement
  is submitted, no refill gap is recorded and no underrun is inferred.

## Test

Build the APK from the current `snap-patch-5` HEAD and repeat the same gameplay
scene and audio conditions used for the previous log.

Keep these runtime settings unchanged for comparison:

```ini
sync_pipeline_compile=1
dxvk_thread_diagnostics=1
android_surface_diagnostics=1
runtime_diagnostics=1
cpu_affinity=not_implemented_game_threads
worker_spin_wait=not_implemented_game_workers
```

The last two values are placeholders and do not enable game-thread affinity or
worker spin-wait behaviour.

After the test, attach the complete `port/log.txt` from the new run. The key
comparison is whether `empty_queue` / `pending_decode` frame counts and
audible gaps decrease, and whether new `output timing` slow-mix reports appear.
Also include logcat if the app crashes during exit.

## Validation status

Source changes are committed to `snap-patch-5`. No Android APK build, install,
or device playback test was performed as part of this change. The fix is not
considered runtime-verified until the new log and listening test are reviewed.

## Shutdown crash follow-up (2026-11-02)

The latest device log reproduces `FORTIFY: pthread_mutex_lock called on a
destroyed mutex` immediately after `quit requested`. The SDL quit handler
previously called `exit(0)` while the detached XAudio2 mixer thread could
still call `SDL_GetQueuedAudioSize` / `SDL_QueueAudio` on the SDL audio device.
That allowed process-exit teardown to destroy SDL audio synchronization while
the mixer thread was still using it.

The shutdown path now:
- keeps the XAudio2 mixer thread joinable and signals it to stop;
- closes its SDL audio device from the mixer thread, after leaving the mix loop;
- joins the mixer before the SDL quit handler calls `exit(0)`;
- signals and joins the xWMA decode worker so it cannot continue publishing
  decoded buffers during process teardown.

This is a targeted lifecycle fix based on the observed quit-time race. It still
requires an APK rebuild and device retest; the current log is from the build
before this change.

Latest test diagnostics: eight callback-to-submit intervals were recorded
(11–65 ms), all on buffers marked EOS and with an empty queue before submit.
These measurements need game-side callback semantics to determine whether
they represent a real continuous-stream gap or normal EOS/voice reuse. Two
xWMA worker jobs took 235 ms and 183 ms; the other recorded jobs took 1–6 ms.

## Latest device log review — 2026-11-02

The newest `Logs/log.txt` does not reproduce the earlier callback-refill
gaps:

- No `refill gap` messages were recorded.
- The output timing summary reports `slow_mix=0`; the SDL queue was low in
  one interval only.
- 88 source-starvation summaries were emitted, but `pending_decode` was
  nonzero only during the first large xWMA decode. The `empty_queue` counter
  measures source frames without queued data and can include normal one-shot
  voice completion; it is not a count of audible glitches.
- The first large stereo xWMA buffer represents about 235.6 seconds of PCM and
  took 282 ms on the worker. Later sampled jobs generally took 1–4 ms.

This log does not justify changing mixer scheduling or callback dispatch.
The outstanding audio optimization candidate is large-buffer xWMA startup
latency. Any streaming/partial-decode redesign must preserve packet ordering,
dpds accounting, loop/play-region semantics and voice lifetime. First verify
whether the 282 ms decode corresponds to an audible startup gap in a repeatable
test.


## 2026-11-02 — Parallel xWMA decode workers

The refreshed runtime log shows xWMA worker latencies of 237 ms for one
1.86 MB stereo stream (235.6 s decoded audio), 28 ms and 53 ms for two
additional stereo streams, plus several later 24–28 ms jobs. The mixer also
reports nonzero `pending_decode` frames. In contrast, the output timing report
has `slow_mix=0`, and no explicit refill-gap events are present.

The xWMA decode queue previously had one worker, so a long music decode could
hold up independent voice jobs behind it. The queue now starts up to two
workers, with a one-worker fallback if thread creation is constrained. Each
worker owns a separate FFmpeg decoder context per job. Decoded buffers are
published into their already-reserved per-voice queue slots; playback still
consumes only the head slot, preserving submission order, play regions,
looping and callback semantics. Shutdown signals and joins every started
worker before voices or SDL are torn down.

This is intended to reduce head-of-line blocking between independent voices;
it does not make a single large WMA stream decode faster. Device testing is
required to verify that parallel CPU use does not worsen frame pacing or
audio scheduling.


## 2026-11-03 — Smooth starvation transitions

The latest device log and source review identified a harsh transition in `mix_source`: when `next_frame` cannot yet read a reserved xWMA buffer (or the source queue is empty), it supplies zero samples immediately. When decoded PCM becomes available, playback can jump abruptly from silence back to the signal. This is a plausible click/pop mechanism, although the log alone cannot prove that every audible artifact came from this path.

The mixer now applies a short 5 ms linear gain ramp per source voice: it fades toward zero while source data is unavailable and fades back to unity after a valid source frame is read. This changes only rendered gain; it does not advance, rewind, reorder, or discard source frames, and it leaves buffer callbacks, play regions, loops, and xWMA decode scheduling unchanged. The ramp also applies to ordinary end-of-queue silence, avoiding an abrupt tail when a source stops.

This is an audio-continuity mitigation, not a decoder throughput fix. Validate with a fresh APK and the same scene; compare audible clicks, `pending_decode`, worker latency, `empty_queue`, and output timing. No device playback test has been performed for this change.


## 2026-11-03 — Master-bus headroom and device-format telemetry

The audio path review found that all XAudio2 source voices are accumulated into the master bus before a nonlinear soft limiter. Earlier sampled diagnostics showed pre-limiter peaks as high as 1.86, so multi-voice sums can drive the limiter frequently. This is a plausible source of audible distortion across music, effects and dialogue, but it is not proof that the limiter is the only cause.

The master output now receives a fixed -6 dB headroom gain before the existing soft limiter. The pre-gain `mix peak` measurement is intentionally retained so logs remain comparable; the `soft_limited` counter now reflects samples that still exceed the limiter knee after headroom. SDL's obtained frequency, format, channel count and buffer size are also logged at device startup to verify the actual output path.

This is a targeted A/B mitigation and diagnostic build, not a device-verified final fix. It reduces output level by approximately 6 dB; check whether crackling decreases without unacceptable volume loss. If artifacts remain with `soft_limited=0`, prioritize output queue/device scheduling and sample-boundary discontinuities instead of further reducing gain.


## 2026-11-03 — SDL output queue prefill and underrun resilience

The `log (7).txt` run differs from the previous device log: `output timing`
reports `queue_low=7–10` and `min_queue=0B` repeatedly, even while
`slow_mix=0` and `max_mix` is 0–2 ms. This points away from mixer CPU overload
and toward the SDL output queue being empty/near-empty during playback. The
obtained device format is correct (48 kHz, F32 stereo).

The SDL device is now left paused immediately after opening. The engine queues
120 ms of mixed audio before unpausing it, avoiding playback from an empty
queue while the mixer thread is starting. The steady-state queue target is
also raised from 80 ms to 120 ms to provide more margin against Android thread
scheduling jitter. `SDL_QueueAudio` failures are now logged, and the once-per-
second output timing summary includes the target queue size and enqueue-failure
count.

This is a targeted output-underrun mitigation based on repeated zero-depth
queue measurements. It adds up to roughly 40 ms of output latency versus the
previous target, plus the initial prefill delay. It has not yet been built into
an APK or verified by device playback. On the next run, confirm that
`SDL playback started after queue prefill` appears once, `queue_low` falls to
zero (or near zero), and crackles improve. If `queue_low` remains high, the
next step is to inspect SDL/Android device queue behavior rather than further
increasing the buffer blindly.


The first `log (7).txt` peak values (0.912 / 0.950) were measured after the
existing -6 dB gain because the counter was placed after the gain operation,
despite the earlier comment saying it was pre-gain. Those values therefore
correspond to approximate pre-gain peaks of 1.82 / 1.90. The counter has now
been moved before the gain so the next log is comparable with pre-gain
measurements; `soft_limited` continues to count samples that exceed the limiter
knee after the -6 dB gain.
