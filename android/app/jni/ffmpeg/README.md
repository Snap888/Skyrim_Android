# Prebuilt FFmpeg (arm64-v8a, WMAV2/WMAPRO only)

**Required layout:**

```
ffmpeg/
  include/
    libavcodec/avcodec.h   <-- required
    libavutil/...
  lib/
    arm64-v8a/
      libavcodec.so
      libavutil.so
```

CMake enables `LB_HAS_FFMPEG` only if **headers and .so** are both present.
If you only upload `.so` without `include/`, the build skips FFmpeg (no compile error).

## Get the full package

1. Actions → **Build FFmpeg Android arm64 (WMAV2)** → Run workflow
2. Download **`ffmpeg-android-arm64-wmav2`** (tar.xz) — contains include/ + lib/
3. Unpack here:

```bash
cd android/app/jni/ffmpeg
# clear incomplete tree first
rm -rf include lib
tar -xJf ~/Downloads/ffmpeg-android-arm64-wmav2.tar.xz
ls include/libavcodec/avcodec.h lib/arm64-v8a/libavcodec.so
```

Or:

```bash
./scripts/install_ffmpeg_artifact.sh ~/Downloads/ffmpeg-android-arm64-wmav2.tar.xz
```

4. Rebuild:

```bash
cd android && ./gradlew assembleDebug -Pffmpeg=true
```
