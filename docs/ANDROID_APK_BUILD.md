# Android APK build — Snap-AI-EngineFrame

Date: 2026-11-06

## Required source inputs

The repository checkout does not contain the generated Skyrim C translation units. The native game target collects them from `gen/*.c` (see `android/app/jni/game/CMakeLists.txt`). The Android CI native-host compile intentionally uses a dummy generated-code stub and does **not** build the complete game runtime or APK.

A full APK build therefore requires the generated game source directory from the same Skyrim runtime revision:

- Local build: place the generated C files in the repository-root `gen/` directory.
- GitHub Actions build: provide a publicly reachable archive URL through the Android CI workflow's `gen_url` input. Supported archive formats are ZIP, tar.gz, and tar.xz. The archive may contain a top-level `gen/` directory or the generated `.c` files directly.

Do not use generated C files from a different runtime revision: the function tables and runtime ABI must match this branch.

## GitHub Actions full APK build

1. Open Actions → Android CI → Run workflow.
2. Select branch `Snap-AI-EngineFrame`.
3. Set `build_apk=true`.
4. Set `gen_url` to the generated-source archive URL.
5. Run the workflow and wait for the **Full arm64 APK build** job.
6. Download artifact `Skyrim-Android-<commit>`; it contains the APK and `build-info.txt`.

The workflow builds `arm64-v8a` only, uses Android NDK `29.0.14206865`, CMake `3.22.1`, and produces a debug APK signed with the runner's Android debug key when no CI signing configuration is present. No signing secret is required for this debug build.

The workflow assigns a monotonically increasing run-derived versionCode and a run-number-based versionName. For repeatable in-place updates across separate GitHub-hosted runner builds, a stable signing identity is still required; ephemeral runner debug keys are not a stable update signature. Local debug builds use the developer machine's persistent debug keystore.

## Local build

From the repository root, after restoring `gen/*.c`:

```bash
cd android
chmod +x ./gradlew
./gradlew assembleDebug \
  -PndkVersion=29.0.14206865 \
  -PversionCode=23 \
  -PversionName=0.21 \
  --no-daemon --stacktrace --console=plain
```

Output:

```
android/app/build/outputs/apk/debug/app-debug.apk
```

The local fallback version values are defined in `android/app/build.gradle`; increase versionCode for each locally distributed update. Keep the same applicationId and signing key when installing an update over an existing installation.

## What CI currently proves

The push-triggered validation workflow checks Java compilation, portable Snap AI/SnapFrame ARM64 builds, portable API tests, analyzer tests, and the DXVK/SnapFrame host ARM64 compile. The complete APK job is deliberately manual and is skipped on ordinary pushes. A green validation run is not a full APK build.

The DXVK Native 2.6.2 candidate remains gated and is not included in the APK until its Android ARM64 runtime pair, ABI/linkage checks, SnapFrame adapter port, and device validation are complete. The existing Android DXVK runtime remains the packaged fallback.
