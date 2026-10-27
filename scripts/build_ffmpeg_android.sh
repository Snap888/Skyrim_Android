#!/usr/bin/env bash
# Minimal FFmpeg for Android arm64-v8a: only WMAV2/WMAPRO decoders (Skyrim xWMA voices).
set -euo pipefail

NDK=""
API=28
REF="n6.1.1"
OUT=""
JOBS="$(nproc 2>/dev/null || echo 4)"

usage() {
  echo "Usage: $0 --ndk PATH --out PATH [--api 28] [--ref n6.1.1]"
  exit 1
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --ndk) NDK="$2"; shift 2 ;;
    --api) API="$2"; shift 2 ;;
    --ref) REF="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    *) usage ;;
  esac
done

[[ -n "$NDK" && -n "$OUT" ]] || usage
[[ -d "$NDK" ]] || { echo "NDK not found: $NDK"; exit 1; }

HOST_TAG="linux-x86_64"
TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/$HOST_TAG"
CC="$TOOLCHAIN/bin/aarch64-linux-android${API}-clang"
CXX="$TOOLCHAIN/bin/aarch64-linux-android${API}-clang++"
AR="$TOOLCHAIN/bin/llvm-ar"
RANLIB="$TOOLCHAIN/bin/llvm-ranlib"
STRIP="$TOOLCHAIN/bin/llvm-strip"
NM="$TOOLCHAIN/bin/llvm-nm"

for t in "$CC" "$AR" "$RANLIB"; do
  [[ -x "$t" ]] || { echo "missing tool: $t"; exit 1; }
done

WORK="${WORKDIR:-$PWD/ffmpeg-build-tmp}"
mkdir -p "$WORK"
cd "$WORK"

if [[ ! -d ffmpeg/.git ]]; then
  rm -rf ffmpeg
  git clone --depth 1 --branch "$REF" https://github.com/FFmpeg/FFmpeg.git ffmpeg \
    || git clone --depth 1 https://github.com/FFmpeg/FFmpeg.git ffmpeg
  if [[ ! -d ffmpeg/.git ]]; then exit 1; fi
  (cd ffmpeg && git fetch --depth 1 origin "refs/tags/$REF:refs/tags/$REF" 2>/dev/null && git checkout "$REF") || true
fi
cd ffmpeg

# 16 KB page-size friendly for modern Android
EXTRA_LDFLAGS="-Wl,-z,max-page-size=16384"

# IMPORTANT: do NOT use -fvisibility=hidden — it strips all public av* exports
# from the .so and the app link fails with undefined reference to avcodec_*.
./configure \
  --prefix="$OUT" \
  --target-os=android \
  --arch=aarch64 \
  --cpu=armv8-a \
  --enable-cross-compile \
  --cc="$CC" \
  --cxx="$CXX" \
  --ar="$AR" \
  --ranlib="$RANLIB" \
  --strip="$STRIP" \
  --nm="$NM" \
  --sysroot="$TOOLCHAIN/sysroot" \
  --enable-shared \
  --disable-static \
  --disable-doc \
  --disable-programs \
  --disable-avdevice \
  --disable-swscale \
  --disable-swresample \
  --disable-postproc \
  --disable-network \
  --disable-everything \
  --enable-decoder=wmav2 \
  --enable-decoder=wmapro \
  --enable-parser=wma \
  --enable-protocol=file \
  --extra-cflags="-O2 -fPIC -DANDROID" \
  --extra-ldflags="$EXTRA_LDFLAGS" \
  --disable-debug \
  --disable-autodetect

make -j"$JOBS"
make install

for so in "$OUT"/lib/*.so; do
  [[ -f "$so" ]] || continue
  "$STRIP" --strip-unneeded "$so" || true
done

# Sanity: public symbols must be exported
echo "=== export check ==="
for so in "$OUT"/lib/libavcodec.so "$OUT"/lib/libavutil.so; do
  [[ -f "$so" ]] || continue
  n=$("$NM" -D "$so" 2>/dev/null | grep -c ' T ' || true)
  echo "$so defined_symbols=$n"
  if [[ "${n:-0}" -lt 10 ]]; then
    echo "ERROR: $so exports too few symbols (visibility/build bug)"
    exit 1
  fi
done
"$NM" -D "$OUT"/lib/libavcodec.so 2>/dev/null | grep -E 'avcodec_find_decoder|avcodec_open2|avcodec_send_packet' || {
  echo "ERROR: required avcodec_* symbols missing"
  exit 1
}

echo "Installed to $OUT"
ls -lah "$OUT/lib"
