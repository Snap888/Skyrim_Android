/* XAudio2 2.7 shim (the June 2010 SDK runtime LEGO Marvel was built with):
 * IXAudio2 and its voices are host COM objects, mixed in software on an
 * engine thread that runs the guest's voice and engine callbacks, and queued
 * to SDL. Effects (XAPOFX reverb/EQ) and filters are accepted and ignored.
 *
 * Callbacks run with g_lock released: the game submits buffers from inside
 * them and calls into XAudio2 from its own threads while holding its locks.
 * Pass-start callbacks run before mixing; OnBufferEnd is dispatched at the
 * exact buffer boundary so the game can refill before the rest of the pass. */
#include "hcom.h"
#include <SDL2/SDL.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <stdarg.h>
#ifdef __ANDROID__
#include <android/log.h>
#endif

/* xaudio2 diagnostics: always go to port/log.txt (stderr) and logcat on Android */
static void xa_log(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "%s\n", buf);
    fflush(stderr);
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "skyrim-xaudio2", "%s", buf);
#endif
}
#define XA_LOG(...) xa_log(__VA_ARGS__)

#define PASS_FRAMES_MS 10
/* Optional diagnostic isolation switch. Keep disabled by default so music is
 * decoded normally; define XWMA_MUTE_LARGE_STEREO=1 only for A/B diagnosis. */
#ifndef XWMA_MUTE_LARGE_STEREO
#define XWMA_MUTE_LARGE_STEREO 0
#endif
#define XWMA_MUSIC_MUTE_THRESHOLD (48u * 1024u)
#define MAX_VOICES 1024
#define MAX_QUEUE 64
#define MAX_CH 8
#define MAX_SENDS 8

enum { V_SOURCE, V_SUBMIX, V_MASTER };
enum { FMT_PCM16, FMT_PCM8, FMT_F32, FMT_PCM24, FMT_PCM32, FMT_ADPCM, FMT_XWMA, FMT_NONE };

typedef struct {
    uint32_t flags, bytes, data, play_begin, play_len, loop_begin, loop_len, loop_count, ctx;
    int16_t *decoded;   /* ADPCM/xWMA decoded PCM */
    uint32_t nframes;   /* frames in the whole buffer */
    uint32_t pos, end;  /* current frame, end of the play region */
    int loops_left;     /* 255 = forever */
    int started, ready, flushed;
} XBuf;

typedef struct Voice Voice;
struct Voice {
    int kind, dead;
    uint32_t guest, callback, flags;
    int in_ch, rate, stage;
    /* source format */
    int fmt, bpf, block_align, spb;
    int xwma_avg_bps;          /* nAvgBytesPerSec for xWMA */
    int xwma_bits;             /* wBitsPerSample for WMA codec setup */
    int xwma_tag;              /* 0x0161 or 0x0162 */
    uint8_t xwma_extra[64];    /* WAVEFORMATEX cbSize bytes after header */
    int xwma_extra_size;
    uint8_t adpcm_coef[64];
    int ncoef;
    float ratio, max_ratio;
    int running, decoding, destroy_pending;
    XBuf q[MAX_QUEUE];
    int qhead, qn;
    uint64_t played;
    /* Refill diagnostics: only a measured callback-to-submit interval is
     * evidence of a late refill. A buffer ending without a replacement is
     * not itself counted as starvation (one-shot sounds end normally). */
    uint64_t last_buffer_end_us;
    uint64_t refill_gap_total_us;
    uint32_t refill_gap_count;
    uint32_t refill_gap_over_5ms;
    uint32_t refill_gap_over_10ms;
    int refill_pending;
    int last_buffer_was_eos;
    double frac;
    float prev[MAX_CH], cur[MAX_CH];
    int primed;
    /* mixing */
    float volume, chvol[MAX_CH];
    int nsends;
    Voice *send[MAX_SENDS];
    float matrix[MAX_SENDS][MAX_CH * MAX_CH];
    int matrix_set[MAX_SENDS];
    float *acc;         /* input accumulation for submix/master: in_ch * frames */
};

typedef struct XDecodeJob {
    Voice *voice;
    int slot;
    uint8_t *data;
    uint32_t bytes;
    uint32_t *dpds;
    uint32_t packet_count;
    int channels, rate, block_align, avg_bps, tag, bits, extra_size;
    uint8_t extra[64];
    unsigned submit_no;
    uint64_t queued_us;
    struct XDecodeJob *next;
} XDecodeJob;

static HComClass k_xa, k_src, k_sub, k_mst, k_fx;
static pthread_mutex_t g_lock;
static Voice *g_voices[MAX_VOICES];
static Voice *g_master;
static uint32_t g_engine_cb[16];
static int g_nengine_cb;
static int g_rate = 48000, g_running, g_thread_started, g_pass_active;
static SDL_AudioDeviceID g_dev;
/* diagnostics */
static unsigned g_xa_src_voices, g_xa_xwma_voices, g_xa_xwma_submits, g_xa_pcm_submits, g_xa_adpcm_submits;
static uint64_t g_xa_mix_passes, g_xa_mix_clipped, g_xa_mix_invalid;
static uint64_t g_xa_starved_pending_frames, g_xa_starved_empty_frames;
static float g_xa_mix_peak;
static pthread_cond_t g_decode_cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_decode_ready_cond = PTHREAD_COND_INITIALIZER;
static XDecodeJob *g_decode_head, *g_decode_tail;
static int g_decode_thread_started;
static uint64_t g_pass_decode_wait_deadline_us;
static uint64_t now_us(void);

/* Called with g_lock held. A voice may be destroyed while an xWMA buffer is
 * decoding outside the mixer lock; decoding is the lifetime reference. */
static void free_voice_locked(Voice *v) {
    if (!v) return;
    for (int k = 0; k < v->qn; k++)
        free(v->q[(v->qhead + k) % MAX_QUEUE].decoded);
    free(v->acc);
    for (int i = 0; i < MAX_VOICES; i++)
        if (g_voices[i] == v) { g_voices[i] = NULL; break; }
    free(v);
}

/* Used only when the engine is stopped. Preserve submissions made after the
 * flush while removing buffers that FlushSourceBuffers had already marked. */
static void drop_flushed_buffers_locked(Voice *v) {
    XBuf keep[MAX_QUEUE];
    int count = 0;
    for (int k = 0; k < v->qn; k++) {
        XBuf *b = &v->q[(v->qhead + k) % MAX_QUEUE];
        if (b->flushed) free(b->decoded);
        else keep[count++] = *b;
    }
    if (count) memcpy(v->q, keep, (size_t)count * sizeof(XBuf));
    if (count < MAX_QUEUE)
        memset(v->q + count, 0, (size_t)(MAX_QUEUE - count) * sizeof(XBuf));
    v->qhead = 0;
    v->qn = count;
}

static const uint8_t CLSID_XAudio2[16] = {0x85, 0x86, 0x50, 0x5a, 0x54, 0xa2, 0xba, 0x4f, 0x9b, 0x82, 0x9a, 0x24, 0xb0, 0x03, 0x06, 0xaf};
/* XAudio2 2.6 (Skyrim): same interfaces as 2.7, different class ids */
static const uint8_t CLSID_XAudio2_26[16] = {0x49, 0x9b, 0xda, 0x3e, 0x85, 0x20, 0x8b, 0x49, 0x9b, 0xb2, 0x39, 0xa6, 0x77, 0x84, 0x93, 0xde};
static const uint8_t CLSID_XAudio2_26_Debug[16] = {0x94, 0x98, 0x19, 0x47, 0xc2, 0x7c, 0x4d, 0x44, 0x98, 0x73, 0xce, 0xd2, 0x56, 0x2c, 0xc6, 0x0e};
static const uint8_t CLSID_XAudio2_Debug[16] = {0x35, 0xea, 0x05, 0xdb, 0x29, 0x03, 0x4b, 0x4d, 0xa5, 0x3a, 0x6d, 0xea, 0xd0, 0x3d, 0x38, 0x52};

/* ---------------------------------------------------------------- format */
static int16_t adpcm_clamp(int v) { return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

static const int k_adpcm_adapt[16] = {230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230};

/* MS ADPCM: one block -> spb frames of in_ch samples. */
static void adpcm_block(Voice *v, const uint8_t *b, int16_t *out) {
    int ch = v->in_ch, spb = v->spb;
    int pred[2], delta[2], s1[2], s2[2];
    const int16_t *coef = (const int16_t *)v->adpcm_coef;
    for (int c = 0; c < ch; c++) pred[c] = b[c] < v->ncoef ? b[c] : 0;
    b += ch;
    for (int c = 0; c < ch; c++, b += 2) delta[c] = (int16_t)(b[0] | b[1] << 8);
    for (int c = 0; c < ch; c++, b += 2) s1[c] = (int16_t)(b[0] | b[1] << 8);
    for (int c = 0; c < ch; c++, b += 2) s2[c] = (int16_t)(b[0] | b[1] << 8);
    for (int c = 0; c < ch; c++) { out[c] = (int16_t)s2[c]; out[ch + c] = (int16_t)s1[c]; }
    int n = 2 * ch, total = spb * ch, hi = 1;
    while (n < total) {
        int c = n % ch;
        int nib = hi ? (*b >> 4) : (*b & 15);
        if (!hi) b++;
        hi = !hi;
        int sn = nib & 8 ? nib - 16 : nib;
        int p = (s1[c] * coef[2 * pred[c]] + s2[c] * coef[2 * pred[c] + 1]) / 256 + sn * delta[c];
        int16_t s = adpcm_clamp(p);
        s2[c] = s1[c];
        s1[c] = s;
        out[n++] = s;
        delta[c] = k_adpcm_adapt[nib] * delta[c] / 256;
        if (delta[c] < 16) delta[c] = 16;
    }
}


#ifdef LB_HAS_FFMPEG
/* Implemented in xwma_ffmpeg.c — decode xWMA/WMA payload to interleaved PCM16.
 * cum_bytes: XAUDIO2_BUFFER_WMA decoded PCM cumulative byte totals (dpds). */
int16_t *xwma_decode(const uint8_t *data, uint32_t size, int channels, int rate,
                     int block_align, int avg_bps, int tag, int bits_per_sample,
                     const uint32_t *cum_bytes, uint32_t packet_count,
                     const uint8_t *extradata, int extradata_size,
                     uint32_t *out_frames);
#endif
/* xWMA decoding runs on a dedicated worker, never on the XAudio2 engine
 * thread. SubmitSourceBuffer reserves its queue slot and copies the guest
 * payload before returning; the worker publishes PCM when decoding finishes. */
static void *xwma_decode_main(void *unused) {
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&g_lock);
        while (!g_decode_head)
            pthread_cond_wait(&g_decode_cond, &g_lock);
        XDecodeJob *job = g_decode_head;
        g_decode_head = job->next;
        if (!g_decode_head) g_decode_tail = NULL;
        pthread_mutex_unlock(&g_lock);

        int16_t *decoded = NULL;
        uint32_t nframes = 0;
        int mute_large_stereo = XWMA_MUTE_LARGE_STEREO &&
            job->channels == 2 && job->bytes > XWMA_MUSIC_MUTE_THRESHOLD;
        if (mute_large_stereo) {
            if (job->submit_no <= 20 || job->submit_no % 50 == 0)
                XA_LOG("xaudio2: xWMA MUTED stereo-large tag=%04x bytes=%u rate=%d packets=%u threshold=%u submit #%u",
                       job->tag, job->bytes, job->rate, job->packet_count,
                       XWMA_MUSIC_MUTE_THRESHOLD, job->submit_no);
        } else {
#ifdef LB_HAS_FFMPEG
            decoded = xwma_decode(job->data, job->bytes, job->channels, job->rate,
                                  job->block_align, job->avg_bps, job->tag, job->bits,
                                  job->dpds, job->packet_count,
                                  job->extra, job->extra_size, &nframes);
            if (!decoded || !nframes) {
                XA_LOG("xaudio2: xWMA decode FAILED tag=%04x bytes=%u packets=%u submit #%u",
                       job->tag, job->bytes, job->packet_count, job->submit_no);
                free(decoded);
                decoded = NULL;
                nframes = 0;
            } else if (job->submit_no <= 10 || job->submit_no % 50 == 0) {
                XA_LOG("xaudio2: xWMA decoded tag=%04x %d ch %d Hz bytes=%u -> %u frames submit #%u",
                       job->tag, job->channels, job->rate, job->bytes,
                       nframes, job->submit_no);
            }
#else
            if (job->submit_no <= 20 || job->submit_no % 50 == 0)
                XA_LOG("xaudio2: xWMA SILENT tag=%04x %d ch %d Hz avg_bps=%d bytes=%u packets=%u submit #%u (need FFmpeg)",
                       job->tag, job->channels, job->rate, job->avg_bps,
                       job->bytes, job->packet_count, job->submit_no);
#endif
        }

        uint64_t elapsed_ms = (now_us() - job->queued_us) / 1000;
        if (decoded && (job->submit_no <= 10 || job->submit_no % 50 == 0 || elapsed_ms >= 20))
            XA_LOG("xaudio2: xWMA worker latency=%llums submit #%u",
                   (unsigned long long)elapsed_ms, job->submit_no);
        else if (!decoded && elapsed_ms >= 20)
            XA_LOG("xaudio2: xWMA failed/empty after %llums submit #%u",
                   (unsigned long long)elapsed_ms, job->submit_no);

        free(job->data);
        free(job->dpds);

        pthread_mutex_lock(&g_lock);
        Voice *v = job->voice;
        XBuf *slot = &v->q[job->slot];
        slot->decoded = decoded;
        slot->nframes = nframes;
        slot->pos = slot->play_begin < slot->nframes ? slot->play_begin : slot->nframes;
        slot->end = slot->play_len ? slot->play_begin + slot->play_len : slot->nframes;
        if (slot->end > slot->nframes) slot->end = slot->nframes;
        if (!slot->loop_len && slot->loop_count)
            slot->loop_len = slot->end - slot->loop_begin;
        slot->loops_left = slot->loop_count == 255 ? 255 : (int)slot->loop_count;
        if (slot->flushed) {
            slot->pos = slot->end;
            slot->loops_left = 0;
        }
        slot->ready = 1;
        if (v->decoding > 0) v->decoding--;

        pthread_cond_broadcast(&g_decode_ready_cond);
        if (v->destroy_pending && !g_running && !g_pass_active && v->decoding == 0) {
            free_voice_locked(v);
        } else if (!v->dead && !g_running && !v->running && v->decoding == 0) {
            drop_flushed_buffers_locked(v);
        }
        pthread_mutex_unlock(&g_lock);
        free(job);
    }
    return NULL;
}

static int start_xwma_decode_worker(void) {
    if (g_decode_thread_started) return 1;
    pthread_t thread;
    if (pthread_create(&thread, NULL, xwma_decode_main, NULL) != 0) return 0;
    pthread_detach(thread);
    g_decode_thread_started = 1;
    return 1;
}

static void set_format(Voice *v, uint32_t wf) {
    uint16_t tag = M16(wf);
    int bits = M16(wf + 14);
    v->in_ch = M16(wf + 2);
    if (v->in_ch < 1) v->in_ch = 1;
    if (v->in_ch > MAX_CH) v->in_ch = MAX_CH;
    v->rate = (int)M32(wf + 4);
    v->block_align = M16(wf + 12);
    if (tag == 0xFFFE) {  /* WAVE_FORMAT_EXTENSIBLE: the subformat's first word is the tag */
        tag = M16(wf + 24);
    }
    v->fmt = FMT_NONE;
    if (tag == 1) v->fmt = bits == 8 ? FMT_PCM8 : bits == 24 ? FMT_PCM24 : bits == 32 ? FMT_PCM32 : FMT_PCM16;
    else if (tag == 3) v->fmt = FMT_F32;
    else if (tag == 2) {
        v->fmt = FMT_ADPCM;
        v->spb = M16(wf + 18);
        v->ncoef = M16(wf + 20);
        if (v->ncoef > 16) v->ncoef = 16;
        memcpy(v->adpcm_coef, MEM + wf + 22, (size_t)v->ncoef * 4);
    } else if (tag == 0x0161 || tag == 0x0162) {
        /* WAVE_FORMAT_WMAUDIO2 / WMAUDIO3 — Skyrim voices & music (xWMA) */
        v->fmt = FMT_XWMA;
        v->xwma_tag = tag;
        v->xwma_bits = bits;
        v->xwma_avg_bps = (int)M32(wf + 8);
        v->bpf = v->block_align ? v->block_align : 1;
        /* cbSize at offset 16; extradata follows the 18-byte WAVEFORMATEX */
        {
            int cb = (int)M16(wf + 16);
            if (cb < 0) cb = 0;
            if (cb > (int)sizeof(v->xwma_extra)) cb = (int)sizeof(v->xwma_extra);
            v->xwma_extra_size = cb;
            if (cb > 0) memcpy(v->xwma_extra, MEM + wf + 18, (size_t)cb);
            else memset(v->xwma_extra, 0, sizeof v->xwma_extra);
        }
        g_xa_xwma_voices++;
        XA_LOG("xaudio2: source format xWMA tag=%04x %d ch %d Hz avg_bps=%d block_align=%d extra=%d (voice #%u, decoder %s)",
               tag, v->in_ch, v->rate, v->xwma_avg_bps, v->block_align, v->xwma_extra_size, g_xa_xwma_voices,
#ifdef LB_HAS_FFMPEG
               "FFmpeg"
#else
               "NOT LINKED — silent"
#endif
        );
    }
    if (v->fmt == FMT_NONE) {
        static unsigned s_silent;
        if (++s_silent % 20 == 1)
            XA_LOG("xaudio2: unsupported source format tag %04x (%d ch, %d Hz) plays silent (%u so far)",
                   tag, v->in_ch, v->rate, s_silent);
    } else if (v->fmt != FMT_XWMA) {
        static unsigned s_ok;
        if (++s_ok <= 8 || s_ok % 100 == 0)
            XA_LOG("xaudio2: source format tag=%04x fmt=%d %d ch %d Hz bits=%d",
                   tag, v->fmt, v->in_ch, v->rate, bits);
    }
    v->bpf = v->fmt == FMT_ADPCM ? 2 * v->in_ch : v->block_align ? v->block_align : v->in_ch * bits / 8;
}

static void read_frame(Voice *v, XBuf *b, uint32_t f, float *out) {
    int ch = v->in_ch;
    if (b->decoded) { for (int c = 0; c < ch; c++) out[c] = b->decoded[f * ch + c] / 32768.0f; return; }
    uint32_t a = b->data + f * (uint32_t)v->bpf;
    switch (v->fmt) {
    case FMT_PCM16: for (int c = 0; c < ch; c++) out[c] = (int16_t)M16(a + 2 * c) / 32768.0f; break;
    case FMT_PCM8: for (int c = 0; c < ch; c++) out[c] = ((float)M8(a + c) - 128.0f) / 128.0f; break;
    case FMT_F32: for (int c = 0; c < ch; c++) out[c] = MF32(a + 4 * c); break;
    case FMT_PCM24:
        for (int c = 0; c < ch; c++) {
            int32_t s = (int32_t)(M8(a + 3 * c) | M8(a + 3 * c + 1) << 8 | M8(a + 3 * c + 2) << 16) << 8;
            out[c] = s / 2147483648.0f;
        }
        break;
    case FMT_PCM32: for (int c = 0; c < ch; c++) out[c] = (int32_t)M32(a + 4 * c) / 2147483648.0f; break;
    default: for (int c = 0; c < ch; c++) out[c] = 0; break;
    }
}

/* ---------------------------------------------------------------- events */
typedef struct { uint32_t cb; int method; uint32_t arg; Voice *v; } Event;
#define MAX_EVENTS 8192
static Event g_ev[MAX_EVENTS];
static int g_nev;

static void push_event(Voice *v, int method, uint32_t arg) {
    if (!v->callback || g_nev >= MAX_EVENTS) return;
    g_ev[g_nev++] = (Event){v->callback, method, arg, v};
}

/* Run queued voice callbacks at the exact buffer boundary. The engine lock
 * must be released because game callbacks commonly submit the next buffer. */
static Event g_dispatch_ev[MAX_EVENTS];
static void dispatch_events_locked(CPU *c) {
    int count = g_nev;
    if (!count) return;
    memcpy(g_dispatch_ev, g_ev, sizeof(Event) * (size_t)count);
    g_nev = 0;
    pthread_mutex_unlock(&g_lock);
    for (int i = 0; i < count; i++) {
        Event *e = &g_dispatch_ev[i];
        if (e->v->dead) continue;
        uint32_t fn = M32(M32(e->cb) + 4 * (uint32_t)e->method);
        uint32_t args[2] = { e->cb, e->arg };
        rt_invoke(c, fn, e->method == 2 ? 1 : 2, args);
    }
    pthread_mutex_lock(&g_lock);
}

/* Give an asynchronous xWMA decode a small part of the current output
 * period to finish. The condition wait releases g_lock for the decoder. */
static int wait_for_decode_locked(void) {
    uint64_t now = now_us();
    if (!g_pass_decode_wait_deadline_us || now >= g_pass_decode_wait_deadline_us)
        return 0;
    uint64_t left = g_pass_decode_wait_deadline_us - now;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += (time_t)(left / 1000000u);
    ts.tv_nsec += (long)((left % 1000000u) * 1000u);
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_cond_timedwait(&g_decode_ready_cond, &g_lock, &ts);
    return 1;
}

/* Next source frame, advancing through the queue. Returns 0 when starved. */
static int next_frame(Voice *v, float *out, CPU *c) {
    while (v->qn) {
        XBuf *b = &v->q[v->qhead];
        /* xWMA decoding happens outside g_lock; never consume a reserved slot
         * until its decoded PCM is published by SubmitSourceBuffer. */
        if (!b->ready) {
            if (wait_for_decode_locked()) continue;
            g_xa_starved_pending_frames++;
            for (int ch = 0; ch < v->in_ch; ch++) out[ch] = 0;
            return 0;
        }
        if (!b->started) {
            b->started = 1;
            push_event(v, 3, b->ctx);  /* OnBufferStart */
        }
        if (b->pos < b->end) {
            read_frame(v, b, b->pos++, out);
            v->played++;
            if (b->pos >= b->end && b->loops_left && b->loop_len) {
                if (b->loops_left != 255) b->loops_left--;
                b->pos = b->loop_begin;
                b->end = b->loop_begin + b->loop_len;
                if (b->end > b->nframes) b->end = b->nframes;
                push_event(v, 5, b->ctx);  /* OnLoopEnd */
            }
            return 1;
        }
        /* Retire the completed buffer before invoking OnBufferEnd: the game
         * callback can now safely enqueue its replacement in this same pass. */
        push_event(v, 4, b->ctx);  /* OnBufferEnd */
        /* Start a per-voice refill timer before dispatching the callback.
         * The callback may immediately SubmitSourceBuffer; that path records
         * the actual elapsed time. EOS is retained for log classification. */
        v->last_buffer_end_us = now_us();
        v->refill_pending = 1;
        v->last_buffer_was_eos = !!(b->flags & 0x40);
        if (b->flags & 0x40) push_event(v, 2, 0);  /* XAUDIO2_END_OF_STREAM -> OnStreamEnd */
        free(b->decoded);
        v->qhead = (v->qhead + 1) % MAX_QUEUE;
        v->qn--;
        dispatch_events_locked(c);
        if (v->dead) {
            for (int ch = 0; ch < v->in_ch; ch++) out[ch] = 0;
            return 0;
        }
    }
    g_xa_starved_empty_frames++;
    for (int c = 0; c < v->in_ch; c++) out[c] = 0;
    return 0;
}

/* ---------------------------------------------------------------- mixing */
static void default_matrix(int sc, int dc, float *m) {
    memset(m, 0, sizeof(float) * MAX_CH * MAX_CH);
    for (int d = 0; d < dc; d++)
        for (int s = 0; s < sc; s++) {
            float g = 0;
            if (sc == 1) g = 1.0f;
            else if (sc == dc) g = s == d ? 1.0f : 0.0f;
            else if (dc == 2) g = s == 0 ? (d == 0) : s == 1 ? (d == 1) : (s == 2 || s == 3) ? 0.707f : (s % 2 == d) ? 0.707f : 0.0f;
            else if (dc == 1) g = 1.0f / (float)sc;
            else g = s % dc == d ? 1.0f : 0.0f;
            m[d * sc + s] = g;
        }
}

static void route(Voice *v, const float *in, int frames) {
    Voice *dests[MAX_SENDS];
    int n = v->nsends;
    for (int i = 0; i < n; i++) dests[i] = v->send[i];
    if (!n && g_master && v != g_master) { dests[0] = g_master; n = 1; }
    for (int i = 0; i < n; i++) {
        Voice *d = dests[i];
        if (!d || d->dead || !d->acc) continue;
        float m[MAX_CH * MAX_CH];
        if (i < v->nsends && v->matrix_set[i]) memcpy(m, v->matrix[i], sizeof m);
        else default_matrix(v->in_ch, d->in_ch, m);
        for (int s = 0; s < v->in_ch; s++) {
            float g = v->volume * v->chvol[s];
            for (int dc = 0; dc < d->in_ch; dc++) m[dc * v->in_ch + s] *= g;
        }
        for (int f = 0; f < frames; f++)
            for (int dc = 0; dc < d->in_ch; dc++) {
                float sum = 0;
                for (int s = 0; s < v->in_ch; s++) sum += m[dc * v->in_ch + s] * in[f * v->in_ch + s];
                d->acc[f * d->in_ch + dc] += sum;
            }
    }
}

static void mix_source(Voice *v, int frames, float *tmp, CPU *c) {
    float step = v->ratio * (float)v->rate / (float)g_rate;
    int ch = v->in_ch;
    if (!v->primed) {
        next_frame(v, v->cur, c);
        memcpy(v->prev, v->cur, sizeof v->prev);
        v->primed = 1;
        v->frac = 0;
    }
    for (int f = 0; f < frames; f++) {
        for (int c = 0; c < ch; c++) tmp[f * ch + c] = v->prev[c] + (v->cur[c] - v->prev[c]) * (float)v->frac;
        v->frac += step;
        while (v->frac >= 1.0) {
            memcpy(v->prev, v->cur, sizeof v->prev);
            next_frame(v, v->cur, c);
            v->frac -= 1.0;
        }
    }
    route(v, tmp, frames);
}

static void engine_pass(CPU *c, int frames, float *out) {
    static float tmp[MAX_CH * 48000 / 10];
    pthread_mutex_lock(&g_lock);
    g_pass_active = 1;
    pthread_mutex_unlock(&g_lock);
    uint32_t args[3];
    /* engine and voice pass-start callbacks, unlocked */
    for (int i = 0; i < g_nengine_cb; i++) {
        args[0] = g_engine_cb[i];
        rt_invoke(c, M32(M32(g_engine_cb[i])), 1, args);
    }
    uint32_t cbs[MAX_VOICES], req[MAX_VOICES];
    int ncb = 0;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_VOICES; i++) {
        Voice *v = g_voices[i];
        if (!v || v->dead || v->kind != V_SOURCE || !v->callback || !v->running) continue;
        /* bytes the voice needs beyond what is queued for this pass */
        uint64_t need = (uint64_t)(frames * v->ratio * (float)v->rate / (float)g_rate) + 2, have = 0;
        for (int k = 0; k < v->qn; k++) { XBuf *b = &v->q[(v->qhead + k) % MAX_QUEUE]; have += b->end - b->pos; }
        cbs[ncb] = v->callback;
        /* XAudio2 explicitly reports BytesRequired=0 for xWMA/XMA:
         * compressed packets do not have a meaningful per-frame byte count. */
        if (v->fmt == FMT_XWMA) {
            req[ncb++] = 0;
        } else {
            req[ncb++] = have >= need ? 0 :
                (uint32_t)((need - have) * (uint64_t)
                    (v->fmt == FMT_ADPCM
                        ? v->block_align / (v->spb ? v->spb : 1) + 1
                        : v->bpf));
        }
    }
    pthread_mutex_unlock(&g_lock);
    for (int i = 0; i < ncb; i++) {
        args[0] = cbs[i];
        args[1] = req[i];
        rt_invoke(c, M32(M32(cbs[i])), 2, args);  /* OnVoiceProcessingPassStart */
    }

    pthread_mutex_lock(&g_lock);
    g_nev = 0;
    /* At most 4 ms of this 10 ms pass may be spent waiting for xWMA PCM. */
    g_pass_decode_wait_deadline_us = now_us() + 4000u;
    for (int i = 0; i < MAX_VOICES; i++) {
        Voice *v = g_voices[i];
        if (v && !v->dead && v->acc) memset(v->acc, 0, sizeof(float) * (size_t)(v->in_ch * frames));
    }
    for (int i = 0; i < MAX_VOICES; i++) {
        Voice *v = g_voices[i];
        if (v && !v->dead && v->kind == V_SOURCE && v->running) mix_source(v, frames, tmp, c);
    }
    /* submixes in processing-stage order, then the master */
    for (int stage = 0; stage < 64; stage++)
        for (int i = 0; i < MAX_VOICES; i++) {
            Voice *v = g_voices[i];
            if (!v || v->dead || v->kind != V_SUBMIX || v->stage != stage) continue;
            route(v, v->acc, frames);
        }
    if (g_master && g_master->acc) {
        Voice *m = g_master;
        for (int f = 0; f < frames; f++) {
            float l = 0, r = 0;
            for (int ch = 0; ch < m->in_ch; ch++) {
                float s = m->acc[f * m->in_ch + ch] * m->volume * m->chvol[ch];
                if (m->in_ch == 1) l = r = s;
                else if (ch == 0) l += s;
                else if (ch == 1) r += s;
                else if (ch == 2) { l += 0.707f * s; r += 0.707f * s; }
                else if (ch >= 4) { if (ch % 2 == 0) l += 0.707f * s; else r += 0.707f * s; }
            }
            /* A hard clamp creates audible high-order distortion when music,
             * voices and effects sum above full scale. Use a continuous soft
             * knee so the output remains bounded without a sharp clip edge. */
            float sample[2] = { l, r };
            for (int c = 0; c < 2; c++) {
                float x = sample[c];
                if (!isfinite(x)) {
                    x = 0.0f;
                    g_xa_mix_invalid++;
                }
                float ax = fabsf(x);
                if (ax > g_xa_mix_peak) g_xa_mix_peak = ax;
                if (ax > 0.90f) {
                    g_xa_mix_clipped++;
                    float over = (ax - 0.90f) / 0.10f;
                    float limited = 0.90f + 0.10f * (over / (1.0f + over));
                    x = copysignf(limited, x);
                }
                out[2 * f + c] = x;
            }
        }
    } else memset(out, 0, sizeof(float) * 2 * (size_t)frames);
    if (++g_xa_mix_passes >= 100) {
        if (g_xa_mix_clipped || g_xa_mix_invalid || g_xa_mix_peak > 0.90f)
            XA_LOG("xaudio2: mix peak=%.3f soft_limited=%llu invalid=%llu (last 1s)",
                   g_xa_mix_peak,
                   (unsigned long long)g_xa_mix_clipped,
                   (unsigned long long)g_xa_mix_invalid);
        g_xa_mix_passes = 0;
        g_xa_mix_clipped = 0;
        g_xa_mix_invalid = 0;
        g_xa_mix_peak = 0.0f;
        if (g_xa_starved_pending_frames || g_xa_starved_empty_frames)
            XA_LOG("xaudio2: source starvation frames pending_decode=%llu empty_queue=%llu (last 1s)",
                   (unsigned long long)g_xa_starved_pending_frames,
                   (unsigned long long)g_xa_starved_empty_frames);
        g_xa_starved_pending_frames = 0;
        g_xa_starved_empty_frames = 0;
    }
    int nev = g_nev;
    static Event ev[MAX_EVENTS];
    memcpy(ev, g_ev, sizeof(Event) * (size_t)nev);
    pthread_mutex_unlock(&g_lock);

    for (int i = 0; i < nev; i++) {
        if (ev[i].v->dead) continue;
        uint32_t fn = M32(M32(ev[i].cb) + 4 * (uint32_t)ev[i].method);
        args[0] = ev[i].cb;
        args[1] = ev[i].arg;
        rt_invoke(c, fn, ev[i].method == 2 ? 1 : 2, args);
    }
    for (int i = 0; i < g_nengine_cb; i++) {
        args[0] = g_engine_cb[i];
        rt_invoke(c, M32(M32(g_engine_cb[i]) + 4), 1, args);  /* OnProcessingPassEnd */
    }
    /* free voices destroyed during the pass */
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_VOICES; i++) {
        Voice *v = g_voices[i];
        if (!v || !v->dead || v->decoding) continue;
        free_voice_locked(v);
    }
    g_pass_active = 0;
    pthread_mutex_unlock(&g_lock);
}

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

static void *engine_main(void *arg) {
    (void)arg;
    CPU *c = rt_new_cpu();
    rt_thread_init(c);
    int frames = g_rate * PASS_FRAMES_MS / 1000;
    float *out = malloc(sizeof(float) * 2 * (size_t)frames);
    uint64_t next = now_us();
    uint64_t diag_passes = 0, queue_low = 0, slow_passes = 0, max_pass_us = 0;
    Uint32 min_queue_bytes = UINT32_MAX;
    for (;;) {
        if (g_dev) {
            /* Keep 60 ms queued to absorb Android scheduling jitter; a device that
             * stops draining for 200 ms is dropped and mixing continues on the timer. */
            Uint32 q0 = SDL_GetQueuedAudioSize(g_dev), q;
            uint64_t t0 = now_us();
            while ((q = SDL_GetQueuedAudioSize(g_dev)) > (Uint32)(6 * frames * 2 * sizeof(float))) {
                if (q < q0) { q0 = q; t0 = now_us(); }
                else if (now_us() - t0 > 200000) {
                    LOG("xaudio2: audio device stalled, mixing silently");
                    SDL_CloseAudioDevice(g_dev);
                    g_dev = 0;
                    next = now_us();
                    break;
                }
                usleep(1000);
            }
        }
        if (!g_dev) {
            uint64_t t = now_us();
            if (t < next) usleep((useconds_t)(next - t));
            next += PASS_FRAMES_MS * 1000;
        }
        if (!g_running) { usleep(PASS_FRAMES_MS * 1000); continue; }
        if (g_dev) {
            Uint32 queued = SDL_GetQueuedAudioSize(g_dev);
            if (queued < min_queue_bytes) min_queue_bytes = queued;
            if (queued < (Uint32)(frames * 2 * sizeof(float))) queue_low++;
        }
        uint64_t mix_start = now_us();
        engine_pass(c, frames, out);
        uint64_t mix_us = now_us() - mix_start;
        if (mix_us > max_pass_us) max_pass_us = mix_us;
        if (mix_us > (uint64_t)PASS_FRAMES_MS * 1000) slow_passes++;
        if (g_dev) SDL_QueueAudio(g_dev, out, (Uint32)(sizeof(float) * 2 * (size_t)frames));
        if (++diag_passes >= 100) {
            if (queue_low || slow_passes)
                XA_LOG("xaudio2: output timing queue_low=%llu min_queue=%uB slow_mix=%llu max_mix=%llums (last 1s)",
                       (unsigned long long)queue_low, min_queue_bytes,
                       (unsigned long long)slow_passes,
                       (unsigned long long)(max_pass_us / 1000));
            diag_passes = queue_low = slow_passes = max_pass_us = 0;
            min_queue_bytes = UINT32_MAX;
        }
    }
    return NULL;
}

static void engine_start_thread(void) {
    if (g_thread_started) return;
    if (!start_xwma_decode_worker())
        XA_LOG("xaudio2: failed to start xWMA decode worker");
    g_thread_started = 1;
    if (!SDL_WasInit(SDL_INIT_AUDIO)) SDL_InitSubSystem(SDL_INIT_AUDIO);
    SDL_AudioSpec want = {0}, have;
    want.freq = g_rate;
    want.format = AUDIO_F32SYS;
    want.channels = 2;
    want.samples = 1024;
    rt_host_affinity_begin();  /* SDL's audio thread runs no guest code */
    g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    rt_host_affinity_end();
    if (!g_dev) LOG("xaudio2: SDL_OpenAudioDevice failed: %s (mixing silently)", SDL_GetError());
    else SDL_PauseAudioDevice(g_dev, 0);
    pthread_t t;
    pthread_create(&t, NULL, engine_main, NULL);
    pthread_detach(t);
}

/* ---------------------------------------------------------------- voices */
static Voice *voice_new(int kind, int ch, int rate, uint32_t flags) {
    Voice *v = calloc(1, sizeof *v);
    v->kind = kind;
    v->in_ch = ch < 1 ? 2 : ch > MAX_CH ? MAX_CH : ch;
    v->rate = rate ? rate : g_rate;
    v->flags = flags;
    v->volume = 1.0f;
    v->ratio = 1.0f;
    for (int i = 0; i < MAX_CH; i++) v->chvol[i] = 1.0f;
    if (kind != V_SOURCE) v->acc = calloc((size_t)(v->in_ch * 48000 / 10 * 2), sizeof(float));
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_VOICES; i++)
        if (!g_voices[i]) { g_voices[i] = v; break; }
    pthread_mutex_unlock(&g_lock);
    return v;
}

static void set_sends(Voice *v, uint32_t sl) {
    v->nsends = 0;
    memset(v->matrix_set, 0, sizeof v->matrix_set);
    if (!sl) return;
    uint32_t n = M32(sl), p = M32(sl + 4);
    for (uint32_t i = 0; i < n && i < MAX_SENDS; i++) {
        Voice *d = hcom_host(M32(p + 8 * i + 4));
        v->send[v->nsends++] = d;
    }
}

#define V ((Voice *)hcom_host(ARG(0)))
SHIM(xv_GetVoiceDetails) {
    Voice *v = V;
    M32(ARG(1)) = v->flags;
    M32(ARG(1) + 4) = (uint32_t)v->in_ch;
    M32(ARG(1) + 8) = (uint32_t)v->rate;
    RET(0, 2);
}
SHIM(xv_SetOutputVoices) {
    pthread_mutex_lock(&g_lock);
    set_sends(V, ARG(1));
    pthread_mutex_unlock(&g_lock);
    RET(0, 2);
}
SHIM(xv_SetEffectChain) { RET(0, 2); }
SHIM(xv_EnableEffect) { RET(0, 3); }
SHIM(xv_DisableEffect) { RET(0, 3); }
SHIM(xv_GetEffectState) { if (ARG(2)) M32(ARG(2)) = 1; RET(0, 3); }
SHIM(xv_SetEffectParameters) { RET(0, 5); }
SHIM(xv_GetEffectParameters) { if (ARG(2)) memset(MEM + ARG(2), 0, ARG(3)); RET(0, 4); }
SHIM(xv_SetFilterParameters) { RET(0, 3); }
SHIM(xv_GetFilterParameters) {
    /* LowPassFilter, frequency 1.0 (passes everything), 1/Q 1.0 */
    M32(ARG(1)) = 0;
    MF32(ARG(1) + 4) = 1.0f;
    MF32(ARG(1) + 8) = 1.0f;
    RET(0, 2);
}
SHIM(xv_SetOutputFilterParameters) { RET(0, 4); }
SHIM(xv_GetOutputFilterParameters) {
    M32(ARG(2)) = 0;
    MF32(ARG(2) + 4) = 1.0f;
    MF32(ARG(2) + 8) = 1.0f;
    RET(0, 3);
}
SHIM(xv_SetVolume) { V->volume = ARGF(1); RET(0, 3); }
SHIM(xv_GetVolume) { MF32(ARG(1)) = V->volume; RET(0, 2); }
SHIM(xv_SetChannelVolumes) {
    Voice *v = V;
    for (uint32_t i = 0; i < ARG(1) && i < MAX_CH; i++) v->chvol[i] = MF32(ARG(2) + 4 * i);
    RET(0, 4);
}
SHIM(xv_GetChannelVolumes) {
    Voice *v = V;
    for (uint32_t i = 0; i < ARG(1) && i < MAX_CH; i++) MF32(ARG(2) + 4 * i) = v->chvol[i];
    RET(0, 3);
}
SHIM(xv_SetOutputMatrix) {
    Voice *v = V, *d = hcom_host(ARG(1));
    uint32_t sc = ARG(2), dc = ARG(3), m = ARG(4);
    pthread_mutex_lock(&g_lock);
    int idx = -1;
    for (int i = 0; i < v->nsends; i++) if (v->send[i] == d) idx = i;
    if (idx < 0 && !v->nsends && v->kind != V_MASTER) {  /* default send to the master */
        v->send[0] = g_master;
        v->nsends = 1;
        idx = d == g_master || !d ? 0 : -1;
    }
    if (idx >= 0 && sc <= MAX_CH && dc <= MAX_CH) {
        memset(v->matrix[idx], 0, sizeof v->matrix[idx]);
        for (uint32_t k = 0; k < sc * dc; k++) v->matrix[idx][k] = MF32(m + 4 * k);
        v->matrix_set[idx] = 1;
    }
    pthread_mutex_unlock(&g_lock);
    RET(0, 6);
}
SHIM(xv_GetOutputMatrix) {
    Voice *v = V, *d = hcom_host(ARG(1));
    uint32_t sc = ARG(2), dc = ARG(3), m = ARG(4);
    float def[MAX_CH * MAX_CH];
    const float *src = def;
    int idx = -1;
    for (int i = 0; i < v->nsends; i++) if (v->send[i] == d) idx = i;
    if (idx >= 0 && v->matrix_set[idx]) src = v->matrix[idx];
    else default_matrix((int)sc, (int)dc, def);
    for (uint32_t k = 0; k < sc * dc && k < MAX_CH * MAX_CH; k++) MF32(m + 4 * k) = src[k];
    RET(0, 5);
}
SHIM(xv_DestroyVoice) {
    Voice *v = V;
    uint32_t g = ARG(0);
    pthread_mutex_lock(&g_lock);
    v->dead = 1;
    v->destroy_pending = 1;
    if (v == g_master) g_master = NULL;
    for (int i = 0; i < MAX_VOICES; i++) {
        Voice *o = g_voices[i];
        if (o) for (int k = 0; k < o->nsends; k++) if (o->send[k] == v) o->send[k] = NULL;
    }
    /* When the engine is stopped no future pass can release this voice.
     * Keep it alive until all out-of-lock xWMA decodes have returned. */
    if (!g_running && !g_pass_active && v->decoding == 0) free_voice_locked(v);
    pthread_mutex_unlock(&g_lock);
    g_hcom_slots[M32(g + 8)] = NULL;
    M32(g) = 0xDEAD0002u;
    gfree(g);
    RET(0, 1);
}
SHIM(xs_Start) {
    Voice *v = V;
    v->running = 1;
    if (v->fmt == FMT_XWMA) {
        static unsigned s;
        if (++s <= 10 || s % 50 == 0)
            XA_LOG("xaudio2: Start xWMA voice fmt=%d ch=%d rate=%d queued=%d (#%u)",
                   v->fmt, v->in_ch, v->rate, v->qn, s);
    }
    RET(0, 3);
}
SHIM(xs_Stop) {
    Voice *v = V;
    v->running = 0;
    v->primed = 0;   /* reset interpolator — avoids click on next Start */
    v->frac = 0.0;
    RET(0, 3);
}
SHIM(xs_SubmitSourceBuffer) {
    Voice *v = V;
    uint32_t b = ARG(1);
    uint32_t bwma = ARG(2);  /* XAUDIO2_BUFFER_WMA* */
    pthread_mutex_lock(&g_lock);
    if (v->dead || v->qn >= MAX_QUEUE) {
        pthread_mutex_unlock(&g_lock);
        RET(0x88960001u, 3); /* XAUDIO2_E_INVALID_CALL */
    }
    XBuf *x = &v->q[(v->qhead + v->qn) % MAX_QUEUE];
    memset(x, 0, sizeof *x);
    x->flags = M32(b);
    x->bytes = M32(b + 4);
    x->data = M32(b + 8);
    x->play_begin = M32(b + 12);
    x->play_len = M32(b + 16);
    x->loop_begin = M32(b + 20);
    x->loop_len = M32(b + 24);
    x->loop_count = M32(b + 28);
    x->ctx = M32(b + 32);

    /* Pair this submission with the most recent completed buffer on the
     * same voice. Do not infer an underrun when no replacement is submitted:
     * that is the normal lifecycle of a one-shot source voice. */
    if (v->refill_pending) {
        uint64_t gap_us = now_us() - v->last_buffer_end_us;
        v->refill_gap_total_us += gap_us;
        v->refill_gap_count++;
        if (gap_us >= 5000) v->refill_gap_over_5ms++;
        if (gap_us >= 10000) v->refill_gap_over_10ms++;
        if (gap_us >= 2000 || v->last_buffer_was_eos) {
            XA_LOG("xaudio2: refill gap voice=%08x gap=%llums eos=%d queue_before=%d totals=%u/>5ms:%u/>10ms:%u",
                   v->guest, (unsigned long long)(gap_us / 1000),
                   v->last_buffer_was_eos, v->qn, v->refill_gap_count,
                   v->refill_gap_over_5ms, v->refill_gap_over_10ms);
        }
        v->refill_pending = 0;
        v->last_buffer_was_eos = 0;
    }

    if (v->fmt == FMT_XWMA) {
        /* Decode asynchronously: this method can be called by an XAudio2
         * callback on the engine thread, so decoding here can drain SDL's
         * queued audio and create audible gaps in long dialogue streams. */
        if (!start_xwma_decode_worker()) {
            pthread_mutex_unlock(&g_lock);
            RET(0x80004005u, 3); /* E_FAIL */
        }
        uint32_t bytes = x->bytes;
        uint32_t packet_count = bwma ? M32(bwma + 4) : 0;
        uint32_t *packet_info = bwma && packet_count
            ? (uint32_t *)(MEM + M32(bwma)) : NULL;
        if (!bytes || (packet_count && packet_count > bytes)) {
            pthread_mutex_unlock(&g_lock);
            RET(0x88960001u, 3);
        }

        XDecodeJob *job = calloc(1, sizeof *job);
        if (job) job->data = malloc(bytes);
        if (job && packet_count && packet_info)
            job->dpds = malloc((size_t)packet_count * sizeof(uint32_t));
        if (!job || !job->data || (packet_count && packet_info && !job->dpds)) {
            if (job) { free(job->data); free(job->dpds); free(job); }
            pthread_mutex_unlock(&g_lock);
            RET(0x8007000Eu, 3); /* E_OUTOFMEMORY */
        }

        memcpy(job->data, MEM + x->data, bytes);
        if (job->dpds)
            memcpy(job->dpds, packet_info, (size_t)packet_count * sizeof(uint32_t));
        job->voice = v;
        job->slot = (int)((v->qhead + v->qn) % MAX_QUEUE);
        job->bytes = bytes;
        job->packet_count = packet_count;
        job->channels = v->in_ch;
        job->rate = v->rate;
        job->block_align = v->block_align;
        job->avg_bps = v->xwma_avg_bps;
        job->tag = v->xwma_tag;
        job->bits = v->xwma_bits;
        job->extra_size = v->xwma_extra_size;
        memcpy(job->extra, v->xwma_extra, sizeof job->extra);
        job->submit_no = ++g_xa_xwma_submits;
        job->queued_us = now_us();

        x->ready = 0;
        v->qn++;
        v->decoding++;
        if (g_decode_tail) g_decode_tail->next = job;
        else g_decode_head = job;
        g_decode_tail = job;
        pthread_cond_signal(&g_decode_cond);
        pthread_mutex_unlock(&g_lock);
        RET(0, 3);
    }

    if (v->fmt == FMT_ADPCM && v->block_align && v->spb) {
        g_xa_adpcm_submits++;
        uint32_t nb = x->bytes / (uint32_t)v->block_align;
        x->nframes = nb * (uint32_t)v->spb;
        x->decoded = malloc(sizeof(int16_t) * (size_t)x->nframes * (size_t)v->in_ch + 2);
        for (uint32_t i = 0; i < nb; i++)
            adpcm_block(v, MEM + x->data + i * (uint32_t)v->block_align,
                        x->decoded + (size_t)i * (size_t)v->spb * (size_t)v->in_ch);
    } else if (v->fmt == FMT_NONE) {
        x->nframes = 0;
    } else {
        g_xa_pcm_submits++;
        x->nframes = v->bpf ? x->bytes / (uint32_t)v->bpf : 0;
    }
    x->pos = x->play_begin < x->nframes ? x->play_begin : x->nframes;
    x->end = x->play_len ? x->play_begin + x->play_len : x->nframes;
    if (x->end > x->nframes) x->end = x->nframes;
    if (!x->loop_len && x->loop_count) x->loop_len = x->end - x->loop_begin;
    x->loops_left = x->loop_count == 255 ? 255 : (int)x->loop_count;
    x->ready = 1;
    v->qn++;
    pthread_mutex_unlock(&g_lock);
    RET(0, 3);
}
SHIM(xs_FlushSourceBuffers) {
    Voice *v = V;
    pthread_mutex_lock(&g_lock);
    /* every queued buffer but the playing one ends, reporting OnBufferEnd on
     * the next pass: keep the current one, mark the rest finished */
    int keep_current = v->running && v->qn && v->q[v->qhead].started;
    for (int k = keep_current ? 1 : 0; k < v->qn; k++) {
        XBuf *b = &v->q[(v->qhead + k) % MAX_QUEUE];
        b->flushed = 1;
        b->pos = b->end;
        b->loops_left = 0;
        b->flags &= ~0x40u;
    }
    if (!v->running && v->qn && !g_running && v->decoding == 0) {
        /* No engine pass will consume flushed buffers. */
        drop_flushed_buffers_locked(v);
    }
    pthread_mutex_unlock(&g_lock);
    RET(0, 1);
}
SHIM(xs_Discontinuity) { RET(0, 1); }
SHIM(xs_ExitLoop) {
    Voice *v = V;
    pthread_mutex_lock(&g_lock);
    if (v->qn) v->q[v->qhead].loops_left = 0;
    pthread_mutex_unlock(&g_lock);
    RET(0, 2);
}
SHIM(xs_GetState) {
    Voice *v = V;
    uint32_t s = ARG(1);
    pthread_mutex_lock(&g_lock);
    M32(s) = v->qn ? v->q[v->qhead].ctx : 0;
    M32(s + 4) = (uint32_t)v->qn;
    M32(s + 8) = (uint32_t)v->played;
    M32(s + 12) = (uint32_t)(v->played >> 32);
    pthread_mutex_unlock(&g_lock);
    RET(0, 2);
}
SHIM(xs_SetFrequencyRatio) {
    float r = ARGF(1);
    Voice *v = V;
    if (r < 1.0f / 1024) r = 1.0f / 1024;
    if (v->max_ratio > 0 && r > v->max_ratio) r = v->max_ratio;
    v->ratio = r;
    RET(0, 3);
}
SHIM(xs_GetFrequencyRatio) { MF32(ARG(1)) = V->ratio; RET(0, 2); }
SHIM(xs_SetSourceSampleRate) { V->rate = (int)ARG(1); RET(0, 2); }
#undef V

#define VOICE_FNS \
    shim_xv_GetVoiceDetails, shim_xv_SetOutputVoices, shim_xv_SetEffectChain, shim_xv_EnableEffect, \
    shim_xv_DisableEffect, shim_xv_GetEffectState, shim_xv_SetEffectParameters, shim_xv_GetEffectParameters, \
    shim_xv_SetFilterParameters, shim_xv_GetFilterParameters, shim_xv_SetOutputFilterParameters, \
    shim_xv_GetOutputFilterParameters, shim_xv_SetVolume, shim_xv_GetVolume, \
    shim_xv_SetChannelVolumes, shim_xv_GetChannelVolumes, shim_xv_SetOutputMatrix, shim_xv_GetOutputMatrix, \
    shim_xv_DestroyVoice
static const ShimFn k_src_fns[] = {
    VOICE_FNS, shim_xs_Start, shim_xs_Stop, shim_xs_SubmitSourceBuffer, shim_xs_FlushSourceBuffers,
    shim_xs_Discontinuity, shim_xs_ExitLoop, shim_xs_GetState, shim_xs_SetFrequencyRatio,
    shim_xs_GetFrequencyRatio, shim_xs_SetSourceSampleRate,
};
static const ShimFn k_sub_fns[] = {VOICE_FNS};

/* ---------------------------------------------------------------- IXAudio2 */
SHIM(xa_QueryInterface) {
    hcom_addref(ARG(0));
    M32(ARG(2)) = ARG(0);
    RET(0, 3);
}
SHIM(xa_AddRef) { RET(hcom_addref(ARG(0)), 1); }
SHIM(xa_Release) {
    uint32_t left;
    if (hcom_release(ARG(0), &left)) g_running = 0;
    RET(left, 1);
}
SHIM(xa_GetDeviceCount) { M32(ARG(1)) = 1; RET(0, 2); }
SHIM(xa_GetDeviceDetails) {
    uint32_t d = ARG(2);
    memset(MEM + d, 0, 1068);
    const char *id = "SDL", *name = "Speakers";
    for (int i = 0; id[i]; i++) M16(d + 2 * i) = (uint8_t)id[i];
    for (int i = 0; name[i]; i++) M16(d + 512 + 2 * i) = (uint8_t)name[i];
    M32(d + 1024) = 0xF;  /* GlobalDefaultDevice */
    uint32_t f = d + 1028;
    M16(f) = 0xFFFE;
    M16(f + 2) = 2;
    M32(f + 4) = (uint32_t)g_rate;
    M32(f + 8) = (uint32_t)g_rate * 8;
    M16(f + 12) = 8;
    M16(f + 14) = 32;
    M16(f + 16) = 22;
    M16(f + 18) = 32;
    M32(f + 20) = 3;  /* SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT */
    static const uint8_t k_float_guid[16] = {3, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71};
    memcpy(MEM + f + 24, k_float_guid, 16);
    RET(0, 3);
}
SHIM(xa_Initialize) { RET(0, 3); }
SHIM(xa_RegisterForCallbacks) {
    if (g_nengine_cb < 16) g_engine_cb[g_nengine_cb++] = ARG(1);
    RET(0, 2);
}
SHIM(xa_UnregisterForCallbacks) {
    for (int i = 0; i < g_nengine_cb; i++)
        if (g_engine_cb[i] == ARG(1)) { g_engine_cb[i] = g_engine_cb[--g_nengine_cb]; break; }
    RET(0, 2);
}
/* CreateSourceVoice(this, ppVoice, pFormat, Flags, MaxFrequencyRatio, pCallback, pSendList, pEffectChain) */
SHIM(xa_CreateSourceVoice) {
    uint32_t wf = ARG(2);
    Voice *v = voice_new(V_SOURCE, M16(wf + 2), (int)M32(wf + 4), ARG(3));
    pthread_mutex_lock(&g_lock);
    set_format(v, wf);
    v->max_ratio = ARGF(4);
    v->callback = ARG(5);
    set_sends(v, ARG(6));
    pthread_mutex_unlock(&g_lock);
    v->guest = hcom_new(&k_src, v);
    M32(ARG(1)) = v->guest;
    TRACE("xaudio2: source voice %d ch %d Hz fmt %d cb %08x", v->in_ch, v->rate, v->fmt, v->callback);
    RET(0, 8);
}
/* CreateSubmixVoice(this, ppVoice, InputChannels, InputSampleRate, Flags, ProcessingStage, pSendList, pEffectChain) */
SHIM(xa_CreateSubmixVoice) {
    Voice *v = voice_new(V_SUBMIX, (int)ARG(2), (int)ARG(3), ARG(4));
    pthread_mutex_lock(&g_lock);
    v->stage = (int)(ARG(5) < 64 ? ARG(5) : 63);
    set_sends(v, ARG(6));
    pthread_mutex_unlock(&g_lock);
    v->guest = hcom_new(&k_sub, v);
    M32(ARG(1)) = v->guest;
    RET(0, 8);
}
/* CreateMasteringVoice(this, ppVoice, InputChannels, InputSampleRate, Flags, DeviceIndex, pEffectChain) */
SHIM(xa_CreateMasteringVoice) {
    int ch = ARG(2) ? (int)ARG(2) : 2;
    if (ARG(3)) g_rate = (int)ARG(3);
    Voice *v = voice_new(V_MASTER, ch, g_rate, ARG(4));
    g_master = v;
    v->guest = hcom_new(&k_mst, v);
    M32(ARG(1)) = v->guest;
    XA_LOG("xaudio2: mastering voice %d ch at %d Hz", v->in_ch, g_rate);
    engine_start_thread();  /* real XAudio2 processes from creation; StartEngine is optional */
    RET(0, 7);
}
SHIM(xa_StartEngine) { g_running = 1; engine_start_thread(); RET(0, 1); }
SHIM(xa_StopEngine) { g_running = 0; RET(0, 1); }
SHIM(xa_CommitChanges) { RET(0, 2); }
SHIM(xa_GetPerformanceData) { memset(MEM + ARG(1), 0, 64); RET(0, 2); }
SHIM(xa_SetDebugConfiguration) { RET(0, 3); }

static const ShimFn k_xa_fns[] = {
    shim_xa_QueryInterface, shim_xa_AddRef, shim_xa_Release, shim_xa_GetDeviceCount, shim_xa_GetDeviceDetails,
    shim_xa_Initialize, shim_xa_RegisterForCallbacks, shim_xa_UnregisterForCallbacks, shim_xa_CreateSourceVoice,
    shim_xa_CreateSubmixVoice, shim_xa_CreateMasteringVoice, shim_xa_StartEngine, shim_xa_StopEngine,
    shim_xa_CommitChanges, shim_xa_GetPerformanceData, shim_xa_SetDebugConfiguration,
};

/* XAPOFX effects: plain IUnknown objects that the voices never run. */
SHIM(fx_QueryInterface) { hcom_addref(ARG(0)); M32(ARG(2)) = ARG(0); RET(0, 3); }
SHIM(fx_AddRef) { RET(hcom_addref(ARG(0)), 1); }
SHIM(fx_Release) { uint32_t left; hcom_release(ARG(0), &left); RET(left, 1); }
SHIM(fx_Stub) { RET(0, 1); }
static const ShimFn k_fx_fns[] = {shim_fx_QueryInterface, shim_fx_AddRef, shim_fx_Release,
                                  shim_fx_Stub, shim_fx_Stub, shim_fx_Stub, shim_fx_Stub, shim_fx_Stub};

static void classes_init(void) {
    static int done;
    if (done) return;
    done = 1;
    hcom_class_init(&k_xa, "IXAudio2", k_xa_fns, sizeof k_xa_fns / sizeof k_xa_fns[0]);
    hcom_class_init(&k_src, "IXAudio2SourceVoice", k_src_fns, sizeof k_src_fns / sizeof k_src_fns[0]);
    hcom_class_init(&k_sub, "IXAudio2SubmixVoice", k_sub_fns, sizeof k_sub_fns / sizeof k_sub_fns[0]);
    hcom_class_init(&k_mst, "IXAudio2MasteringVoice", k_sub_fns, sizeof k_sub_fns / sizeof k_sub_fns[0]);
    hcom_class_init(&k_fx, "XAPOFX", k_fx_fns, sizeof k_fx_fns / sizeof k_fx_fns[0]);
}

/* Called by CoCreateInstance: 1 and *out set when clsid is XAudio2's. */
int xaudio2_create(uint32_t clsid, uint32_t *out) {
    if (!guid_eq(clsid, CLSID_XAudio2) && !guid_eq(clsid, CLSID_XAudio2_Debug) &&
        !guid_eq(clsid, CLSID_XAudio2_26) && !guid_eq(clsid, CLSID_XAudio2_26_Debug)) return 0;
    classes_init();
    *out = hcom_new(&k_xa, NULL);
    g_running = 1;
    XA_LOG("xaudio2: created IXAudio2 %08x", *out);
    XA_LOG("xaudio2: build: xWMA path=yes decoder=%s log=port/log.txt + logcat",
#ifdef LB_HAS_FFMPEG
           "FFmpeg WMAV2"
#else
           "NONE (voices/music silent until FFmpeg linked)"
#endif
    );
    return 1;
}

/* XAPOFX1_5!CreateFX(REFCLSID, IUnknown **, const void *init, UINT32 size), cdecl */
SHIM(CreateFX) {
    classes_init();
    M32(ARG(1)) = hcom_new(&k_fx, NULL);
    RETC(0);
}

static const ShimDef defs[] = {
    {"XAPOFX1_5.dll", "CreateFX", shim_CreateFX},
    {NULL, NULL, NULL}
};

void xaudio2_init(void) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_lock, &a);
    rt_register_shims(defs);
}
