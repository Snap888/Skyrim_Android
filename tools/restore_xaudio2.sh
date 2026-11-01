#!/usr/bin/env bash
# Restore runtime/xaudio2.c and apply unlock-decode fix for crackling audio.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

GOOD_REF=${1:-631b5ab1cce8}
echo "Restoring runtime/xaudio2.c from $GOOD_REF"
git show "$GOOD_REF:runtime/xaudio2.c" > runtime/xaudio2.c

python3 - <<'PY'
from pathlib import Path
text = Path('runtime/xaudio2.c').read_text()
start = text.find('SHIM(xs_SubmitSourceBuffer)')
end = text.find('SHIM(xs_FlushSourceBuffers)', start)
if start < 0 or end < 0:
    raise SystemExit('SubmitSourceBuffer block not found')
block = r'''SHIM(xs_SubmitSourceBuffer) {
    Voice *v = V;
    uint32_t b = ARG(1);
    uint32_t bwma = ARG(2);

    uint32_t flags = M32(b);
    uint32_t bytes = M32(b + 4);
    uint32_t data = M32(b + 8);
    uint32_t play_begin = M32(b + 12);
    uint32_t play_len = M32(b + 16);
    uint32_t loop_begin = M32(b + 20);
    uint32_t loop_len = M32(b + 24);
    uint32_t loop_count = M32(b + 28);
    uint32_t ctx = M32(b + 32);

    int16_t *decoded = NULL;
    uint32_t nframes = 0;
    int is_xwma = (v->fmt == FMT_XWMA);
    int is_adpcm = (v->fmt == FMT_ADPCM && v->block_align && v->spb);

    if (is_adpcm) {
        uint32_t nb = bytes / (uint32_t)v->block_align;
        nframes = nb * (uint32_t)v->spb;
        decoded = malloc(sizeof(int16_t) * (size_t)nframes * (size_t)v->in_ch + 2);
        if (decoded) {
            for (uint32_t i = 0; i < nb; i++)
                adpcm_block(v, MEM + data + i * (uint32_t)v->block_align,
                            decoded + (size_t)i * (size_t)v->spb * (size_t)v->in_ch);
        } else nframes = 0;
    } else if (is_xwma) {
        uint32_t packet_count = bwma ? M32(bwma + 4) : 0;
        uint32_t *packet_info = bwma && packet_count ? (uint32_t *)(MEM + M32(bwma)) : NULL;
#ifdef LB_HAS_FFMPEG
        /* Decode WITHOUT g_lock — FFmpeg on large buffers otherwise stalls the
         * mixer thread and causes crackling / underruns. */
        decoded = xwma_decode(MEM + data, bytes, v->in_ch, v->rate,
                              v->block_align, v->xwma_avg_bps, v->xwma_tag,
                              packet_info, packet_count,
                              v->xwma_extra, v->xwma_extra_size,
                              &nframes);
        if (!decoded || !nframes) {
            XA_LOG("xaudio2: xWMA decode FAILED tag=%04x bytes=%u packets=%u",
                   v->xwma_tag, bytes, packet_count);
            free(decoded); decoded = NULL; nframes = 0;
        } else {
            static unsigned s_ok;
            if (++s_ok <= 15 || s_ok % 40 == 0)
                XA_LOG("xaudio2: xWMA decoded tag=%04x %d ch %d Hz bytes=%u -> %u frames pkt=%u",
                       v->xwma_tag, v->in_ch, v->rate, bytes, nframes, packet_count);
        }
#else
        static unsigned s_sil;
        if (++s_sil <= 20 || s_sil % 50 == 0)
            XA_LOG("xaudio2: xWMA SILENT tag=%04x bytes=%u (need FFmpeg)", v->xwma_tag, bytes);
        (void)packet_info;
#endif
    } else if (v->fmt != FMT_NONE) {
        nframes = v->bpf ? bytes / (uint32_t)v->bpf : 0;
    }

    pthread_mutex_lock(&g_lock);
    if (v->qn >= MAX_QUEUE) {
        pthread_mutex_unlock(&g_lock);
        free(decoded);
        RET(0x88960001u, 3);
    }
    XBuf *x = &v->q[(v->qhead + v->qn) % MAX_QUEUE];
    memset(x, 0, sizeof *x);
    x->flags = flags; x->bytes = bytes; x->data = data;
    x->play_begin = play_begin; x->play_len = play_len;
    x->loop_begin = loop_begin; x->loop_len = loop_len;
    x->loop_count = loop_count; x->ctx = ctx;
    x->decoded = decoded; x->nframes = nframes;
    if (is_adpcm) g_xa_adpcm_submits++;
    else if (is_xwma) g_xa_xwma_submits++;
    else if (v->fmt != FMT_NONE) g_xa_pcm_submits++;

    if (x->decoded && x->nframes) {
        uint32_t pb = x->play_begin, pl = x->play_len;
        if (pl == 0 || pb >= x->nframes || (pl && pb + pl > x->nframes * 2)) {
            x->pos = 0; x->end = x->nframes;
        } else {
            x->pos = pb;
            x->end = pl ? pb + pl : x->nframes;
            if (x->end > x->nframes) x->end = x->nframes;
        }
    } else {
        x->pos = x->play_begin < x->nframes ? x->play_begin : x->nframes;
        x->end = x->play_len ? x->play_begin + x->play_len : x->nframes;
        if (x->end > x->nframes) x->end = x->nframes;
    }
    if (!x->loop_len && x->loop_count)
        x->loop_len = x->end > x->loop_begin ? x->end - x->loop_begin : 0;
    x->loops_left = x->loop_count == 255 ? 255 : (int)x->loop_count;
    v->qn++;
    pthread_mutex_unlock(&g_lock);
    RET(0, 3);
}

'''
text = text[:start] + block + text[end:]
text = text.replace('#define PASS_FRAMES_MS 10', '#define PASS_FRAMES_MS 15', 1)
text = text.replace('(Uint32)(4 * frames * 2 * sizeof(float))', '(Uint32)(6 * frames * 2 * sizeof(float))', 1)
Path('runtime/xaudio2.c').write_text(text)
print('patched', len(text), 'unlock', 'WITHOUT g_lock' in text)
PY

echo "Done. Commit runtime/xaudio2.c and rebuild APK with -Pffmpeg=true"
