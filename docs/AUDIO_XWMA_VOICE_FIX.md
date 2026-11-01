# xWMA voice isolation experiment

## Branch and scope

- Working branch: `snap-patch-4`
- Experiment: mute large stereo xWMA buffers before FFmpeg decode.
- `gen/` is intentionally untouched.
- This is a diagnostic isolation change, not a final xWMA decoder fix.

## Why

The current xWMA path in `runtime/xwma_ffmpeg.c` feeds the submitted payload as one packet, then retries using `block_align` chunks. It currently does not use the XAUDIO2_BUFFER_WMA cumulative packet offsets (`cum_bytes` / `packet_count`). Existing reports include WMAV2 packet-size and spectral-RLE errors on long stereo streams.

The requested first experiment is to determine whether large stereo streams (presumed music) are the source of continuous noise while preserving short/mono dialogue clips.

## Implemented test

In `runtime/xaudio2.c`, before the FFmpeg decode call:

- If source channels = 2 and submitted buffer size > 48 KiB, the buffer is left silent.
- All other xWMA buffers continue through the existing decoder.
- Muted buffers produce a rate-limited log line:
  `xaudio2: xWMA MUTED stereo-large ...`

The experiment is enabled by default with `XWMA_MUTE_LARGE_STEREO=1`. To disable it for a comparison build, compile with `-DXWMA_MUTE_LARGE_STEREO=0`.

This threshold is intentionally a coarse diagnostic heuristic. Some stereo voice assets could also be muted; use the log and in-game test to evaluate that possibility.

## Device test

Build with FFmpeg enabled:

```bash
cd android
./gradlew assembleDebug -Pffmpeg=true
```

Capture relevant logs:

```bash
adb logcat -s skyrim-xaudio2
```

Compare:

1. Whether `xWMA MUTED stereo-large` appears during music playback.
2. Whether short/mono voice buffers still report successful xWMA decode.
3. Whether continuous crackling disappears when the large stereo buffers are muted.
4. Whether NPC dialogue becomes intelligible.

Interpretation:

- Crackling disappears and dialogue is clear: large stereo xWMA streams are strongly implicated; proceed with packet-layout/demux investigation.
- Crackling remains while no large stereo buffers are active: investigate decoded mono/short buffers, the software mixer, SDL output and device routing.
- Dialogue is also missing: inspect the actual voice channel count and buffer sizes; the 2ch/48 KiB heuristic may be too broad.

## Decoder correction in progress

The FFmpeg packet submission path now handles `AVERROR(EAGAIN)` correctly:
it drains pending decoded frames and retries the same encoded packet instead of
counting the packet as accepted and dropping it. The previous behavior could
silently lose WMA packets and shorten decoded dialogue.

The large-stereo mute experiment now encloses the entire decode/failure-check
path. Muted buffers therefore no longer emit a misleading `xWMA decode FAILED`
message.

These changes are source-level fixes; they still require an Android build and
on-device playback/log comparison. Packet-table semantics and exact packet
boundaries remain under investigation.

## Not yet resolved

- Correct xWMA packetization using cumulative packet byte offsets.
- Whether libavformat's xWMA demuxer can be integrated or its packet handling replicated.
- Decoder overflow/noise rejection and asynchronous decode.
- SDL/mixer routing validation on the target device.
- Engine shutdown mutex lifetime issue.

No claim of audible success can be made until tested on-device. Keep `runtime/xaudio2.c` as the full implementation (not a stub); do not modify `gen/`.


## Collect logs from the game/port folder

The native runtime already redirects stderr to `<game>/port/log.txt`. This contains
the XAudio2 and xWMA decoder messages (including `xWMA MUTED`, decoder pass
results, FFmpeg packet errors and fatal runtime messages).

A helper is provided at `port/collect_xwma_logs.sh`. Copy it into the installed
game's `port/` directory next to `log.txt`, then run it from Termux:

```sh
cd "/storage/emulated/0/Skyrim/port"
sh collect_xwma_logs.sh
```

It creates one timestamped `xwma-diagnostics-YYYYMMDD-HHMMSS.txt` file in
that same directory. The report combines the complete runtime `log.txt`
with filtered Android logcat entries for XAudio2, xWMA/FFmpeg, SDL/audio and
crash diagnostics. The example path should be replaced with the actual game
folder selected in the launcher.

Run the game first, reproduce the issue (include both NPC dialogue and music),
exit the game, then run the collector. Send the generated diagnostics file.
