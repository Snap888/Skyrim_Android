# Prebuilt FFmpeg (arm64-v8a, WMAV2/WMAPRO only)

**Required layout:**

```
ffmpeg/
  include/
    libavcodec/avcodec.h
    libavutil/...
  lib/
    arm64-v8a/
      libavcodec.so   # must EXPORT avcodec_* symbols
      libavutil.so
```

## Broken build (old artifact)

If `.so` was built with `-fvisibility=hidden`, `nm -D libavcodec.so` shows **0** `T` symbols
and the APK link fails with `undefined reference to avcodec_*`.

Check:

```bash
nm -D lib/arm64-v8a/libavcodec.so | grep ' T ' | head
# must list avcodec_find_decoder, avcodec_open2, ...
```

## Get a good package

1. Actions → **Build FFmpeg Android arm64 (WMAV2)** → Run workflow  
   (script now rejects empty exports)
2. Download `ffmpeg-android-arm64-wmav2.tar.xz`
3. Replace tree:

```bash
cd android/app/jni/ffmpeg
rm -rf include lib
tar -xJf ~/Downloads/ffmpeg-android-arm64-wmav2.tar.xz
nm -D lib/arm64-v8a/libavcodec.so | grep avcodec_find_decoder
```

4. Rebuild APK: `./gradlew assembleDebug -Pffmpeg=true`

## Temporary: build APK without FFmpeg

```bash
./gradlew assembleDebug -Pffmpeg=false
```

Or remove/rename `lib/arm64-v8a/*.so` so CMake auto-disables FFmpeg.
