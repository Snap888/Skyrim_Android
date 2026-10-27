# snap-patch: xWMA voice fix (prep)

## Apply on top of main

```bash
git checkout snap-patch
# restore xaudio2.c from main if it was broken:
curl -sL https://raw.githubusercontent.com/Snap888/Skyrim_Android/main/runtime/xaudio2.c -o runtime/xaudio2.c
patch -p1 < patches/0001-xwma-and-stop-click.patch
git add runtime/xaudio2.c
git commit -m "snap-patch: xWMA path + Stop click fix"
```

## What this does
- Recognizes WAVE_FORMAT 0x0161/0x0162 as FMT_XWMA
- Stores xwma_avg_bps / xwma_tag
- Captures pBufferWMA seek table
- Logs xWMA submits (still silent until WMA decoder is linked)
- Resets primed/frac on xs_Stop (fixes click on resume)

## Next step
Link libavcodec (WMAV2) and decode into XBuf.decoded in SubmitSourceBuffer.
