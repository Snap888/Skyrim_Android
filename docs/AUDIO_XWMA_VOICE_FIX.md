# xWMA voice and music decoding

## Branch and scope

- Working branch: `snap-patch-4`
- Main change: decode xWMA as fixed-size compressed packets using the XAudio2 packet count and WAVEFORMATEX block alignment.
- `gen/` is intentionally untouched.
- The decoder change is source-level and still requires Android playback validation.

## Why

The previous xWMA path in `runtime/xwma_ffmpeg.c` first fed the entire submitted payload as one FFmpeg packet. Its permissive success threshold could accept a short partial decode and skip packet-by-packet processing. The latest implementation uses `PacketCount` and `block_align` to submit each compressed packet separately. `pDecodedPacketCumulativeBytes` is decoded PCM byte accounting, not compressed packet offsets.

The large-stereo mute switch remains available only as an optional A/B diagnostic; it is disabled by default so music is not silenced.

## Implemented test

In `runtime/xaudio2.c`, before the FFmpeg decode call:

- If source channels = 2 and submitted buffer size > 48 KiB, the buffer is left silent.
- All other xWMA buffers continue through the existing decoder.
- Muted buffers produce a rate-limited log line:
  `xaudio2: xWMA MUTED stereo-large ...`

To enable the isolation experiment for a comparison build, compile with `-DXWMA_MUTE_LARGE_STEREO=1`. Normal playback is the default (`0`).

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

## Decoder corrections

The FFmpeg packet submission path now handles `AVERROR(EAGAIN)` correctly:
it drains pending decoded frames and retries the same encoded packet instead of
counting the packet as accepted and dropping it. The previous behavior could
silently lose WMA packets and shorten decoded dialogue.

The large-stereo mute experiment now encloses the entire decode/failure-check
path. Muted buffers therefore no longer emit a misleading `xWMA decode FAILED`
message. The experiment is disabled by default again, so stereo music is no
longer intentionally silenced; define `XWMA_MUTE_LARGE_STEREO=1` only for an
A/B diagnostic build.

These changes are source-level fixes; they still require an Android build and
on-device playback/log comparison. Packet-table semantics were checked against
the Microsoft contract and FFmpeg's xWMA demuxer packet model.

## Not yet resolved

- Verify on-device decoded duration against the `dpds` cumulative PCM byte count.
- Investigate any remaining WMAPRO/WMAV2 decode errors with real `.xwm`/`.xwma`/`.fuz` samples.
- Decoder overflow/noise rejection and performance measurement on-device.
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

## Fixed-packet decoder

The decoder no longer tries the entire compressed data buffer as one FFmpeg
packet and no longer accepts an arbitrary 0.5-second partial result as success.
It now:

- derives each packet size from `AudioBytes / PacketCount`, verifies the
  XAudio2-required exact divisibility, and checks the result against `block_align`;
- submits every fixed-size compressed packet separately while retaining the
  FFmpeg decoder state between packets;
- flushes the decoder after the final packet;
- validates the decoded-packet cumulative table as monotonic decoded-byte
  accounting, without treating it as compressed offsets;
- logs packet count, packet size, decoded PCM frames, `dpds` duration, effective extradata size/flags, and normalized bitrate.

This follows the Microsoft XAUDIO2_BUFFER_WMA contract and FFmpeg's xWMA
demuxer packet model. A mismatched packet count or block alignment is reported
as a layout error rather than silently decoding a partial stream.

Android compilation and on-device playback are still required to confirm the
result with the Skyrim voice and music assets.


The XAudio2 voice callback path was also corrected: `OnVoiceProcessingPassStart`
now receives `BytesRequired = 0` for xWMA voices, as required by the API because
compressed xWMA packets have no meaningful per-frame byte count. The previous
generic PCM-style calculation incorrectly multiplied output frames by the
compressed packet alignment and could request a nonsensical amount of input data.


## Mixer lock and decode scheduling

Large xWMA buffers can contain thousands of compressed packets. Decoding
inside SubmitSourceBuffer was unsafe even after releasing the mixer mutex:
XAudio2 can invoke SubmitSourceBuffer from an OnBufferEnd callback on the engine
thread itself. A long decode could therefore block the engine thread until the
SDL audio queue drained, causing crackle or gaps in long dialogue.

The current implementation uses a dedicated xWMA decoder worker:

- SubmitSourceBuffer copies the compressed payload and dpds table into host-owned
  memory, reserves its exact queue position, enqueues a job, and returns without
  waiting for FFmpeg.
- The worker decodes outside the mixer/engine thread and publishes PCM into the
  reserved slot under the mixer mutex.
- The mixer will not consume a reserved slot until it is ready, preserving
  per-voice submission order and callback order.
- Flush and voice destruction defer queue/voice cleanup while jobs are in flight.
- Unstarted head buffers are also marked flushed; only a buffer that has
  actually started playing is retained by FlushSourceBuffers.
- Worker latency (queue wait plus decode time) is logged for early samples,
  every 50th sample, and any sample taking at least 20 ms.

This removes FFmpeg decode time from the engine callback path. It does not by
itself prove that the worker always decodes faster than real time; long-dialogue
playback and worker-latency logs still require validation on the Android device.

## Real FUZ sample analysis (DialogueGe__0003526D_1.fuz)

A supplied vanilla voice sample was inspected byte-for-byte.

- FUZ container: version 1, 3,141-byte LIP payload, 7,510-byte XWMA RIFF payload.
- RIFF audio: WMAV2 (`0x0161`), mono, 44,100 Hz, 32,000 bit/s, block alignment 1,487.
- Compressed data: 7,435 bytes = exactly 5 packets × 1,487 bytes.
- `dpds`: five cumulative decoded-byte values: 16,384; 28,672; 40,960; 61,440; 77,824.
- FFmpeg reference decode: 38,912 mono frames / 77,824 PCM bytes = 0.882358 seconds.
- The final decoded PCM byte count exactly matches the final `dpds` value.

The reference decode exposed an important missing initialization detail in the
custom XAudio2 path. Bethesda xWMA RIFF streams normally have no WMA codec
extradata, but FFmpeg's xWMA demuxer synthesizes it before opening WMAV2:
six zeroed bytes with byte 4 set to 31. For WMAPRO it synthesizes 18 bytes,
setting byte 0 to the coded bit depth and byte 14 to 224. The custom decoder
previously passed an empty extradata block straight to libavcodec. This can
cause invalid WMAV2 bit-reservoir/block flags and incomplete or failed
decoding even when packet boundaries are correct.

The custom decoder now mirrors FFmpeg's xWMA demuxer initialization, including
its WMAV2 bitrate normalization rules, WMAPRO fallback extradata, and bit-depth
propagation. Runtime logs now expose the effective extradata size/flags and
normalized bitrate so the Android run can confirm the fallback is active. Packet splitting now follows the XAudio2 submission contract: `AudioBytes`
must divide evenly by `PacketCount`, and each resulting packet must match
`block_align`. (FFmpeg's RIFF demuxer can read a trailing short file block,
but that does not relax XAudio2's `SubmitSourceBuffer` contract.) It also uses refcounted `AVPacket` buffers and rejects decoded PCM whose
byte count differs from the final cumulative `dpds` value.

The supplied sample's 7,435-byte data chunk divides exactly into five
1,487-byte packets, matching both `PacketCount` and `block_align`. It was decoded
successfully by the system FFmpeg reference decoder; the Android in-process implementation has not yet been built or run
against this sample. The sample therefore establishes the expected packet
layout and PCM length, but does not by itself prove the Android fix is working.


## Music crackle: master mix headroom

The software mixer previously hard-clamped the summed master output to [-1, 1].
When several sources (especially stereo music plus dialogue/effects) exceed
full scale, this creates abrupt waveform clipping that can sound like crackle
or harsh distortion. The master now uses a continuous soft-knee limiter above
0.90 full scale, with a bounded output, and reports the raw peak, limited
sample count, and non-finite sample count once per second when relevant.

The next Android test should check whether crackle is reduced and inspect
`xaudio2: mix peak=...` lines. If the peak remains below 0.90 and there are no
invalid samples, the crackle is more likely caused by source decode, queue
underruns, or device scheduling than master clipping.
