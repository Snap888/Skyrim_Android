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

## Not yet resolved

- Correct xWMA packetization using cumulative packet byte offsets.
- Whether libavformat's xWMA demuxer can be integrated or its packet handling replicated.
- Decoder overflow/noise rejection and asynchronous decode.
- SDL/mixer routing validation on the target device.
- Engine shutdown mutex lifetime issue.

No claim of audible success can be made until tested on-device. Keep `runtime/xaudio2.c` as the full implementation (not a stub); do not modify `gen/`.
