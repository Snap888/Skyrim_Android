#!/system/bin/sh
# Run from <Skyrim game folder>/port:
#   sh collect_xwma_logs.sh
# Creates xwma-diagnostics-YYYYMMDD-HHMMSS.txt beside log.txt.

set -u

PORT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
OUT="$PORT_DIR/xwma-diagnostics-$(date '+%Y%m%d-%H%M%S').txt"
GAME_LOG="$PORT_DIR/log.txt"

{
    echo "=== Skyrim Android xWMA diagnostics ==="
    echo "Captured: $(date 2>/dev/null || echo unknown)"
    echo "Port directory: $PORT_DIR"
    echo

    echo "=== PORT LOG: $GAME_LOG ==="
    if [ -f "$GAME_LOG" ]; then
        cat "$GAME_LOG"
    else
        echo "MISSING: port/log.txt (launch the game at least once first)"
    fi
    echo

    echo "=== ANDROID LOGCAT (audio, SDL and crash diagnostics) ==="
    if command -v logcat >/dev/null 2>&1; then
        # Keep only the tags/messages useful for xWMA, FFmpeg, SDL and crashes.
        logcat -d -v threadtime 2>&1 | grep -Ei \
            'skyrim-xaudio2|xwma_ffmpeg|wmav2|wmapro|ffmpeg|SDL|AudioTrack|AAudio|OpenSL|FATAL EXCEPTION|AndroidRuntime|DEBUG|signal [0-9]+|Fatal signal|backtrace:' \
            || true
    else
        echo "logcat command not available in this shell."
        echo "Run this script from Termux or an Android shell that provides logcat."
    fi
    echo
    echo "=== END ==="
} > "$OUT"

echo "Saved: $OUT"
echo "Share this single file for xWMA diagnosis."
