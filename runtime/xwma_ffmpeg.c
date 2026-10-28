/* xWMA decode via FFmpeg for Skyrim (tag 0x0161 / 0x0162).
 *
 * XAudio2 gives raw WMA frames of nBlockAlign bytes. Feeding them one-by-one
 * often yields only a few decoded frames (rare voice snippets). Strategy:
 *  1) try continuous feed of the whole buffer (decoder self-syncs)
 *  2) if almost empty, retry with block_align packets
 */
#ifdef LB_HAS_FFMPEG

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavutil/error.h>

static void xa_err(const char *what, int err)
{
    char buf[128];
    av_strerror(err, buf, sizeof buf);
    fprintf(stderr, "xwma_ffmpeg: %s: %s (%d)\n", what, buf, err);
}

static int frame_ch(const AVFrame *f, int fb)
{
#if LIBAVCODEC_VERSION_MAJOR >= 59
    int n = f->ch_layout.nb_channels;
#else
    int n = f->channels;
#endif
    return n > 0 ? n : fb;
}

static int append_frame(AVFrame *frame, int want_ch,
                        int16_t **pcm, size_t *len, size_t *cap)
{
    int n = frame->nb_samples;
    int src_ch = frame_ch(frame, want_ch);
    if (n <= 0 || want_ch <= 0) return 0;

    size_t need = (*len + (size_t)n * (size_t)want_ch) * sizeof(int16_t);
    if (need > *cap) {
        size_t ncap = *cap ? *cap * 2 : need + 16384;
        if (ncap < need) ncap = need;
        int16_t *np = realloc(*pcm, ncap);
        if (!np) return -1;
        *pcm = np;
        *cap = ncap;
    }
    int16_t *dst = *pcm + *len;
    for (int i = 0; i < n; i++) {
        for (int c = 0; c < want_ch; c++) {
            int sc = c < src_ch ? c : 0;
            float s = 0.f;
            switch (frame->format) {
            case AV_SAMPLE_FMT_S16:
                s = ((int16_t *)frame->data[0])[i * src_ch + sc] / 32768.f; break;
            case AV_SAMPLE_FMT_S16P:
                s = ((int16_t *)frame->data[sc])[i] / 32768.f; break;
            case AV_SAMPLE_FMT_FLT:
                s = ((float *)frame->data[0])[i * src_ch + sc]; break;
            case AV_SAMPLE_FMT_FLTP:
                s = ((float *)frame->data[sc])[i]; break;
            default: break;
            }
            if (s > 1.f) s = 1.f;
            if (s < -1.f) s = -1.f;
            dst[i * want_ch + c] = (int16_t)(s * 32767.f);
        }
    }
    *len += (size_t)n * (size_t)want_ch;
    return 0;
}

static int drain(AVCodecContext *ctx, AVFrame *frame, int want_ch,
                 int16_t **pcm, size_t *len, size_t *cap)
{
    for (;;) {
        int r = avcodec_receive_frame(ctx, frame);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return 0;
        if (r < 0) return r;
        if (append_frame(frame, want_ch, pcm, len, cap) < 0) return AVERROR(ENOMEM);
    }
}

static AVCodecContext *open_ctx(const AVCodec *codec, int channels, int rate,
                                int block_align, int avg_bps,
                                const uint8_t *extradata, int extradata_size)
{
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx) return NULL;
    ctx->sample_rate = rate > 0 ? rate : 44100;
#if LIBAVCODEC_VERSION_MAJOR >= 59
    av_channel_layout_default(&ctx->ch_layout, channels);
#else
    ctx->channels = channels;
    ctx->channel_layout = av_get_default_channel_layout(channels);
#endif
    if (block_align > 0) ctx->block_align = block_align;
    if (avg_bps > 0) ctx->bit_rate = (int64_t)avg_bps * 8;
    ctx->request_sample_fmt = AV_SAMPLE_FMT_FLTP;

    if (extradata && extradata_size > 0) {
        ctx->extradata = av_mallocz((size_t)extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!ctx->extradata) { avcodec_free_context(&ctx); return NULL; }
        memcpy(ctx->extradata, extradata, (size_t)extradata_size);
        ctx->extradata_size = extradata_size;
    }
    if (avcodec_open2(ctx, codec, NULL) < 0) {
        avcodec_free_context(&ctx);
        return NULL;
    }
    return ctx;
}

static int feed_block(AVCodecContext *ctx, const uint8_t *p, int psz,
                      AVPacket *pkt, AVFrame *frame, int channels,
                      int16_t **pcm, size_t *len, size_t *cap)
{
    uint8_t *padded = av_malloc((size_t)psz + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!padded) return AVERROR(ENOMEM);
    memcpy(padded, p, (size_t)psz);
    memset(padded + psz, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    pkt->data = padded;
    pkt->size = psz;
    int err = avcodec_send_packet(ctx, pkt);
    av_free(padded);
    pkt->data = NULL;
    pkt->size = 0;
    if (err < 0 && err != AVERROR(EAGAIN)) return err;
    return drain(ctx, frame, channels, pcm, len, cap);
}

int16_t *xwma_decode(const uint8_t *data, uint32_t size, int channels, int rate,
                     int block_align, int avg_bps, int tag,
                     const uint32_t *cum_bytes, uint32_t packet_count,
                     const uint8_t *extradata, int extradata_size,
                     uint32_t *out_frames)
{
    *out_frames = 0;
    if (!data || !size || channels < 1) return NULL;
    (void)cum_bytes;
    (void)packet_count;

    enum AVCodecID id = (tag == 0x0162) ? AV_CODEC_ID_WMAPRO : AV_CODEC_ID_WMAV2;
    const AVCodec *codec = avcodec_find_decoder(id);
    if (!codec) {
        fprintf(stderr, "xwma_ffmpeg: no decoder tag=%04x\n", tag);
        return NULL;
    }

    size_t cap = 0, len = 0;
    int16_t *pcm = NULL;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    if (!pkt || !frame) {
        av_packet_free(&pkt);
        av_frame_free(&frame);
        return NULL;
    }

    /* Pass 1: whole buffer as one bitstream */
    AVCodecContext *ctx = open_ctx(codec, channels, rate, block_align, avg_bps,
                                   extradata, extradata_size);
    if (!ctx) goto done;

    feed_block(ctx, data, (int)size, pkt, frame, channels, &pcm, &len, &cap);
    avcodec_send_packet(ctx, NULL);
    drain(ctx, frame, channels, &pcm, &len, &cap);
    avcodec_free_context(&ctx);

    double sec_cmp = (avg_bps > 0) ? (double)size / (double)avg_bps : 0;
    double sec_pcm = (rate > 0 && len) ? (double)(len / (size_t)channels) / (double)rate : 0;

    int pass1_ok = (len > 0) && (sec_cmp <= 0.01 || sec_pcm >= sec_cmp * 0.25 || sec_pcm >= 0.5);

    if (!pass1_ok && block_align > 0) {
        free(pcm); pcm = NULL; len = 0; cap = 0;
        ctx = open_ctx(codec, channels, rate, block_align, avg_bps,
                       extradata, extradata_size);
        if (ctx) {
            uint32_t ba = (uint32_t)block_align;
            int sent = 0, fails = 0;
            for (uint32_t off = 0; off + 4 < size; ) {
                uint32_t psz = ba;
                if (off + psz > size) psz = size - off;
                if (psz < 32) break;
                int err = feed_block(ctx, data + off, (int)psz, pkt, frame, channels,
                                     &pcm, &len, &cap);
                if (err < 0) fails++;
                else sent++;
                off += psz;
            }
            avcodec_send_packet(ctx, NULL);
            drain(ctx, frame, channels, &pcm, &len, &cap);
            avcodec_free_context(&ctx);
            sec_pcm = (rate > 0 && len) ? (double)(len / (size_t)channels) / (double)rate : 0;
            fprintf(stderr,
                    "xwma_ffmpeg: pass2 ba=%d sent=%d fail=%d pcm=%.2fs cmp=%.2fs tag=%04x\n",
                    block_align, sent, fails, sec_pcm, sec_cmp, tag);
        }
    } else if (len > 0) {
        fprintf(stderr,
                "xwma_ffmpeg: pass1 whole pcm=%.2fs cmp=%.2fs frames=%u tag=%04x ba=%d extra=%d\n",
                sec_pcm, sec_cmp, (unsigned)(len / (size_t)channels), tag, block_align, extradata_size);
    }

done:
    av_frame_free(&frame);
    av_packet_free(&pkt);

    if (!pcm || !len) {
        fprintf(stderr, "xwma_ffmpeg: FAILED tag=%04x size=%u ba=%d extra=%d\n",
                tag, size, block_align, extradata_size);
        free(pcm);
        return NULL;
    }
    *out_frames = (uint32_t)(len / (size_t)channels);
    return pcm;
}

#endif /* LB_HAS_FFMPEG */
