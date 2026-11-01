#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
cat tools/xaudio2.b64.* | base64 -d > runtime/xaudio2.c
BYTES=$(wc -c < runtime/xaudio2.c)
echo "wrote runtime/xaudio2.c ($BYTES bytes)"
grep -q "WITHOUT g_lock" runtime/xaudio2.c
grep -q "xaudio2_create" runtime/xaudio2.c
if grep -q '#error' runtime/xaudio2.c; then echo "still has #error"; exit 1; fi
echo "OK: unlock fix present, no #error"
