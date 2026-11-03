# Android ARM64 build

## Toolchain

The repository currently uses:
- Android ABI: `arm64-v8a`;
- Android platform: 28;
- CMake: 3.22.1;
- C++17;
- NDK: 29.0.14206865 in the existing Android CI workflow;
- Gradle JDK: 17.

## Build

From the repository root, after generated game sources are available:

```sh
cd android
./gradlew assembleDebug -PndkVersion=29.0.14206865 --no-daemon --stacktrace
```

The APK output is `android/app/build/outputs/apk/debug/app-debug.apk`.

## Snap AI / SnapFrame native dependencies

The JNI CMake project fetches `Snap888/Snap_AI_Engine` at pinned commit `f60acfcb7b09722d5927215957c46045b11f7897`. Upstream tests/examples and its optional legacy DXVK target are disabled in the app build. The game runtime links `SnapAI::Engine` and `SnapFrame::Engine`.

This currently requires Git/network access during CMake configure unless the FetchContent source is already cached. The portable libraries do not depend on DXVK. The current legacy SnapFrame GPU host still does.

## Validation note

The branch's Android CI push workflow currently compiles Java. A full APK build is a separate workflow-dispatch job and requires the generated `gen/` sources or a `gen_url` archive. Do not claim an APK build until that job has run successfully.
