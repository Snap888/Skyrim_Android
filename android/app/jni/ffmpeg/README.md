# Prebuilt FFmpeg (arm64-v8a, WMAV2/WMAPRO only)

Unpack the GitHub Actions artifact here:

```
ffmpeg/
  include/
    libavcodec/
    libavutil/
  lib/
    arm64-v8a/
      libavcodec.so
      libavutil.so
  BUILD_INFO.txt   (optional)
```

## How to get the libs

1. Actions → **Build FFmpeg Android arm64 (WMAV2)** → Run workflow
2. Download artifact `ffmpeg-android-arm64-wmav2` (tar.xz) or `ffmpeg-android-arm64-wmav2-tree`
3. Unpack into this directory:

```bash
cd android/app/jni/ffmpeg
tar -xJf ~/Downloads/ffmpeg-android-arm64-wmav2.tar.xz
```

4. Rebuild the APK — CMake enables `LB_HAS_FFMPEG` automatically when `.so` files exist.

Force on/off:

```bash
./gradlew assembleDebug -Pffmpeg=true
./gradlew assembleDebug -Pffmpeg=false
```
