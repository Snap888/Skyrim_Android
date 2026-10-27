#!/usr/bin/env bash
# Install downloaded workflow artifact into android/app/jni/ffmpeg/
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$ROOT/android/app/jni/ffmpeg"
ARCHIVE="${1:-}"

if [[ -z "$ARCHIVE" ]]; then
  echo "Usage: $0 ffmpeg-android-arm64-wmav2.tar.xz"
  echo "   or: $0 /path/to/unpacked-tree-dir"
  exit 1
fi

mkdir -p "$DEST"
if [[ -d "$ARCHIVE" ]]; then
  cp -a "$ARCHIVE/include" "$DEST/" 2>/dev/null || true
  cp -a "$ARCHIVE/lib" "$DEST/" 2>/dev/null || true
  [[ -f "$ARCHIVE/BUILD_INFO.txt" ]] && cp "$ARCHIVE/BUILD_INFO.txt" "$DEST/"
else
  tar -xJf "$ARCHIVE" -C "$DEST"
fi

echo "Installed:"
find "$DEST" -type f | head -40
test -f "$DEST/lib/arm64-v8a/libavcodec.so" && echo "OK libavcodec.so"
test -f "$DEST/lib/arm64-v8a/libavutil.so" && echo "OK libavutil.so"
