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
