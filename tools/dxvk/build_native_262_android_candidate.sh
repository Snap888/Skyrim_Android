#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 ]]; then
  echo "usage: $0 <patched-dxvk-source-dir> <build-dir> [repo-root]" >&2
  exit 2
fi

DXVK_SOURCE_DIR=$(cd "$1" && pwd)
BUILD_DIR=$2
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=${3:-$(cd "$SCRIPT_DIR/../.." && pwd)}
REPO_ROOT=$(cd "$REPO_ROOT" && pwd)
NDK_ROOT=${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}
CMAKE_BIN=${CMAKE_BIN:-cmake}
ABI=${ANDROID_ABI:-arm64-v8a}
PLATFORM=${ANDROID_PLATFORM:-android-28}

if [[ -z "$NDK_ROOT" || ! -f "$NDK_ROOT/build/cmake/android.toolchain.cmake" ]]; then
  echo "ANDROID_NDK_HOME (or ANDROID_NDK_ROOT) must point to a valid Android NDK" >&2
  exit 2
fi
if [[ "$ABI" != "arm64-v8a" ]]; then
  echo "DXVK Native 2.6.2 candidate is currently validated only for arm64-v8a (got $ABI)" >&2
  exit 2
fi
if [[ ! -f "$DXVK_SOURCE_DIR/src/wsi/sdl2/wsi_native_surface.cpp" ]]; then
  echo "DXVK source tree is missing the applied Android WSI overlay" >&2
  exit 2
fi

mkdir -p "$BUILD_DIR"
BUILD_DIR=$(cd "$BUILD_DIR" && pwd)
HARNESS="$BUILD_DIR/harness"
mkdir -p "$HARNESS"
cat > "$HARNESS/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.22)
project(dxvk_native_262_android_candidate LANGUAGES C CXX)

set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
add_subdirectory("${SNAP_REPO_ROOT}/android/app/jni/SDL2" sdl2)

set(SNAP_DXVK_NATIVE_262_SOURCE_DIR "${SNAP_DXVK_SOURCE_DIR}" CACHE PATH "" FORCE)
add_subdirectory("${SNAP_REPO_ROOT}/android/app/jni/dxvk_native_262" dxvk_native_262)
CMAKE

"$CMAKE_BIN" -S "$HARNESS" -B "$BUILD_DIR/build" \
  -DCMAKE_TOOLCHAIN_FILE="$NDK_ROOT/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI="$ABI" \
  -DANDROID_PLATFORM="$PLATFORM" \
  -DCMAKE_BUILD_TYPE=Release \
  -DSNAP_REPO_ROOT="$REPO_ROOT" \
  -DSNAP_DXVK_SOURCE_DIR="$DXVK_SOURCE_DIR"

"$CMAKE_BIN" --build "$BUILD_DIR/build" --target SDL2 dxvk_d3d9_262 --parallel "${BUILD_JOBS:-2}"

DXVK_SO="$BUILD_DIR/build/dxvk_native_262/libdxvk_d3d9_262.so"
if [[ ! -s "$DXVK_SO" ]]; then
  DXVK_SO=$(find "$BUILD_DIR/build" -type f -name 'libdxvk_d3d9_262.so' -print -quit)
fi
test -n "$DXVK_SO" && test -s "$DXVK_SO"

LLVM_BIN="$NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin"
READELF="$LLVM_BIN/llvm-readelf"
NM="$LLVM_BIN/llvm-nm"
test -x "$READELF"
test -x "$NM"

"$READELF" -h "$DXVK_SO" | grep -q 'AArch64'
"$READELF" -d "$DXVK_SO" | grep -Fq 'Library soname: [libdxvk_d3d9_262.so]'
"$NM" -D --defined-only "$DXVK_SO" | grep -Eq '[[:space:]]Direct3DCreate9$'
"$NM" -D --defined-only "$DXVK_SO" | grep -Eq '[[:space:]]Direct3DCreate9Ex$'

echo "DXVK_NATIVE_262_ANDROID_ARM64_BUILD_OK"
echo "DXVK_NATIVE_262_ANDROID_ARM64_SONAME_OK"
echo "DXVK_NATIVE_262_ANDROID_D3D9_EXPORTS_OK"
echo "DXVK_NATIVE_262_ANDROID_LIBRARY=$DXVK_SO"
