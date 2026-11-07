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

# DXVK's D3D9 version script hides every symbol not listed under global:.
# The game-side main_262/JNI bridge must resolve the Android surface registry
# and the version-matched SnapFrame callbacks from this exact DSO.
SYMBOL_MAP="$DXVK_SOURCE_DIR/src/d3d9/d3d9.sym"
python3 - "$SYMBOL_MAP" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text(encoding="utf-8")
required = (
    "dxvk_native_surface",
    "snapframe_dxvk262_record_source",
    "snapframe_dxvk262_release_window",
    "snapframe_dxvk262_release_device",
)
if text.count("  global:") != 1 or text.count("  local:") != 1:
    raise SystemExit("unexpected DXVK D3D9 symbol map structure")
missing = [name for name in required if f"    {name};" not in text]
if missing:
    insertion = "".join(f"    {name};\n" for name in missing)
    text = text.replace("  local:", insertion + "\n  local:", 1)
    path.write_text(text, encoding="utf-8")
PY

mkdir -p "$BUILD_DIR"
BUILD_DIR=$(cd "$BUILD_DIR" && pwd)
HARNESS="$BUILD_DIR/harness"
SHADER_HEADER="$BUILD_DIR/snapframe-generated/snapframe_shaders.h"
mkdir -p "$HARNESS" "$(dirname "$SHADER_HEADER")"

SPIRV_VAL=${SPIRV_VAL:-spirv-val}
GLSLANG_VALIDATOR=${GLSLANG_VALIDATOR:-glslangValidator}
command -v "$SPIRV_VAL" >/dev/null
command -v "$GLSLANG_VALIDATOR" >/dev/null
python3 "$REPO_ROOT/tools/compile_snapframe_shaders.py" \
  --glslang "$GLSLANG_VALIDATOR" \
  --spirv-val "$SPIRV_VAL" \
  --output "$SHADER_HEADER"
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

# Compile the version-matched SnapFrame host into the candidate DSO.
target_sources(dxvk_d3d9_262 PRIVATE
  "${SNAP_REPO_ROOT}/android/app/jni/snapframe/snapframe_host_262.cpp")
target_include_directories(dxvk_d3d9_262 PRIVATE
  "${SNAP_REPO_ROOT}/android/app/jni/snapframe"
  "${BUILD_SNAPFRAME_GENERATED_DIR}")
target_compile_definitions(dxvk_d3d9_262 PRIVATE SNAPFRAME_DXVK262_HOST=1)
CMAKE

"$CMAKE_BIN" -S "$HARNESS" -B "$BUILD_DIR/build" \
  -DCMAKE_TOOLCHAIN_FILE="$NDK_ROOT/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI="$ABI" \
  -DANDROID_PLATFORM="$PLATFORM" \
  -DCMAKE_BUILD_TYPE=Release \
  -DSNAP_REPO_ROOT="$REPO_ROOT" \
  -DSNAP_DXVK_SOURCE_DIR="$DXVK_SOURCE_DIR" \
  -DBUILD_SNAPFRAME_GENERATED_DIR="$(dirname "$SHADER_HEADER")"

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

# Capture tool output before matching. With pipefail, grep -q can close a pipe
# early and make llvm-readelf/llvm-nm report SIGPIPE (exit 74) even on success.
ELF_HEADER=$("$READELF" -h "$DXVK_SO")
ELF_DYNAMIC=$("$READELF" -d "$DXVK_SO")
ELF_EXPORTS=$("$NM" -D --defined-only "$DXVK_SO")
ELF_DEFINED=$("$NM" --defined-only "$DXVK_SO")

if ! grep -q 'AArch64' <<<"$ELF_HEADER"; then
  echo "DXVK 2.6.2 candidate is not an AArch64 ELF library" >&2
  printf '%s\\n' "$ELF_HEADER" >&2
  exit 1
fi
if ! grep -Fq 'Library soname: [libdxvk_d3d9_262.so]' <<<"$ELF_DYNAMIC"; then
  echo "DXVK 2.6.2 candidate has an unexpected SONAME" >&2
  printf '%s\\n' "$ELF_DYNAMIC" >&2
  exit 1
fi
if ! grep -Eq '[[:space:]]Direct3DCreate9$' <<<"$ELF_EXPORTS"; then
  echo "DXVK 2.6.2 candidate is missing Direct3DCreate9 export" >&2
  printf '%s\\n' "$ELF_EXPORTS" >&2
  exit 1
fi
if ! grep -Eq '[[:space:]]Direct3DCreate9Ex$' <<<"$ELF_EXPORTS"; then
  echo "DXVK 2.6.2 candidate is missing Direct3DCreate9Ex export" >&2
  printf '%s\\n' "$ELF_EXPORTS" >&2
  exit 1
fi
for symbol in dxvk_native_surface snapframe_dxvk262_record_source snapframe_dxvk262_release_window snapframe_dxvk262_release_device; do
  if ! grep -Eq "[[:space:]]${symbol}$" <<<"$ELF_EXPORTS"; then
    echo "DXVK 2.6.2 candidate is missing required dynamic export: ${symbol}" >&2
    printf '%s\\n' "$ELF_EXPORTS" >&2
    exit 1
  fi
done

echo "DXVK_NATIVE_262_SNAPFRAME_HOST_COMPILED_OK"
echo "DXVK_NATIVE_262_ANDROID_ARM64_BUILD_OK"
echo "DXVK_NATIVE_262_ANDROID_ARM64_SONAME_OK"
echo "DXVK_NATIVE_262_ANDROID_D3D9_EXPORTS_OK"
echo "DXVK_NATIVE_262_ANDROID_LIBRARY=$DXVK_SO"
