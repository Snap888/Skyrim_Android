# xWMA voices: FFmpeg prebuild + integration

## Overview

1. **Workflow** `.github/workflows/build-ffmpeg-android.yml`  
   Builds a minimal FFmpeg (`wmav2` + `wmapro` only) for **Android arm64-v8a** and uploads:
   - `ffmpeg-android-arm64-wmav2` — `tar.xz`
   - `ffmpeg-android-arm64-wmav2-tree` — unpacked `include/` + `lib/arm64-v8a/`

2. **Unpack** into `android/app/jni/ffmpeg/` (see README there).

3. **CMake** (`android/app/jni/game/CMakeLists.txt`) auto-enables `LB_HAS_FFMPEG` if libs exist, compiles `runtime/xwma_ffmpeg.c`, links `libavcodec`/`libavutil`.

4. **Gradle** packages `jni/ffmpeg/lib/arm64-v8a/*.so` into the APK.

5. **Runtime** `xaudio2.c` calls `xwma_decode()` for WAVE_FORMAT tags `0x0161`/`0x0162`.

## Quick path

```bash
# 1) Run workflow on GitHub, download artifact

# 2) Unpack
cd android/app/jni/ffmpeg
tar -xJf /path/to/ffmpeg-android-arm64-wmav2.tar.xz

# 3) Ensure runtime files are on the branch
#    runtime/xaudio2.c  (xWMA path + logs)
#    runtime/xwma_ffmpeg.c

# 4) Build APK
cd android
./gradlew assembleDebug -Pffmpeg=true
```

## Log expectations

```
xaudio2: build: xWMA path=yes decoder=FFmpeg WMAV2
xaudio2: source format xWMA tag=0161 ... (decoder FFmpeg)
xaudio2: xWMA decoded tag=0161 ... -> N frames
```

logcat: `adb logcat -s skyrim-xaudio2`
