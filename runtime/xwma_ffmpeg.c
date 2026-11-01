/* xWMA via FFmpeg for Skyrim Android.
 *
 * WAVEFORMATEX.nBlockAlign (e.g. 2230) does NOT match actual xWMA packet
 * sizes (46..1594). Wrong block_align -> noise. Derive BA from packet table.
 * Reject under-decoded large buffers instead of playing crackle.
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
        size_t ncap = *cap ? *cap * 2 : need + 65536;
        if (ncap < need) ncap = need;
        int16_t *np = (int16_t *)realloc(*pcm, ncap);
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
            case AV_SAMPLE_FMT_S32:
                s = ((int32_t *)frame->data[0])[i * src_ch + sc] / 2147483648.f; break;
            case AV_SAMPLE_FMT_S32P:
                s = ((int32_t *)frame->data[sc])[i] / 2147483648.f; break;
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
    av_channel_layout_default(&ctx->ch_layout, channels > 0 ? channels : 2);
#else
    ctx->channels = channels > 0 ? channels : 2;
    ctx->channel_layout = av_get_default_channel_layout(ctx->channels);
#endif
    ctx->block_align = block_align > 0 ? block_align : 1;
    if (avg_bps > 0) ctx->bit_rate = (int64_t)avg_bps * 8;
    ctx->request_sample_fmt = AV_SAMPLE_FMT_FLTP;

    if (extradata && extradata_size > 0) {
        ctx->extradata = (uint8_t *)av_mallocz((size_t)extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!ctx->extradata) { avcodec_free_context(&ctx); return NULL; }
        memcpy(ctx->extradata, extradata, (size_t)extradata_size);
        ctx->extradata_size = extradata_size;
    }
    int err = avcodec_open2(ctx, codec, NULL);
    if (err < 0) {
        xa_err("avcodec_open2", err);
        avcodec_free_context(&ctx);
        return NULL;
    }
    return ctx;
}

static int feed_block(AVCodecContext *ctx, const uint8_t *p, int psz,
                      AVPacket *pkt, AVFrame *frame, int channels,
                      int16_t **pcm, size_t *len, size_t *cap)
{
    if (psz <= 0) return 0;
    uint8_t *padded = (uint8_t *)av_malloc((size_t)psz + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!padded) return AVERROR(ENOMEM);
    memcpy(padded, p, (size_t)psz);
    memset(padded + psz, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    pkt->data = padded;
    pkt->size = psz;
    int err = avcodec_send_packet(ctx, pkt);
    av_free(padded);
    pkt->data = NULL;
    pkt->size = 0;
    if (err < 0 && err != AVERROR(EAGAIN)) {
        if (err != AVERROR_INVALIDDATA) return err;
        return 0;
    }
    return drain(ctx, frame, channels, pcm, len, cap);
}

static int effective_block_align(int fmt_ba, const uint32_t *cum, uint32_t n, uint32_t size)
{
    if (!cum || n < 1) return fmt_ba > 0 ? fmt_ba : 1;
    uint32_t prev = 0;
    uint32_t min_ps = UINT32_MAX, cnt = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t end = cum[i];
        if (end > size) end = size;
        if (end <= prev) continue;
        uint32_t psz = end - prev;
        prev = end;
        if (psz < 32) continue;
        if (psz < min_ps) min_ps = psz;
        cnt++;
        if (prev >= size) break;
    }
    if (cnt == 0) return fmt_ba > 0 ? fmt_ba : 1;
    return (int)min_ps;
}

static int duration_ok(size_t len, int channels, int rate, uint32_t size, int avg_bps)
{
    if (!len || channels < 1 || rate < 1) return 0;
    uint32_t frames = (uint32_t)(len / (size_t)channels);
    double sec = (double)frames / (double)rate;

    if (size < 16 * 1024 && frames >= 1024) return 1;
    if (size < 48 * 1024 && sec >= 0.02) return 1;

    double exp_sec = (avg_bps > 0) ? (double)size / (double)avg_bps : 0;
    if (exp_sec <= 0.1) return sec >= 0.02;
    return sec >= exp_sec * 0.25;
}

static void clear_pcm(int16_t **pcm, size_t *len, size_t *cap)
{
    free(*pcm);
    *pcm = NULL;
    *len = 0;
    *cap = 0;
}

int16_t *xwma_decode(const uint8_t *data, uint32_t size, int channels, int rate,
                     int block_align, int avg_bps, int tag,
                     const uint32_t *cum_bytes, uint32_t packet_count,
                     const uint8_t *extradata, int extradata_size,
                     uint32_t *out_frames)
{
    *out_frames = 0;
    if (!data || !size || channels < 1) return NULL;

    enum AVCodecID id = (tag == 0x0162) ? AV_CODEC_ID_WMAPRO : AV_CODEC_ID_WMAV2;
    const AVCodec *codec = avcodec_find_decoder(id);
    if (!codec) {
        fprintf(stderr, "xwma_ffmpeg: no decoder tag=%04x\n", tag);
        return NULL;
    }

    int ba = effective_block_align(block_align, cum_bytes, packet_count, size);
    if (ba != block_align)
        fprintf(stderr, "xwma_ffmpeg: ba fmt=%d -> effective=%d (packets=%u size=%u)\n",
                block_align, ba, packet_count, size);

    size_t cap = 0, len = 0;
    int16_t *pcm = NULL;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    if (!pkt || !frame) {
        av_packet_free(&pkt);
        av_frame_free(&frame);
        return NULL;
    }

    int used_packets = 0;
    const char *how = NULL;

    {
        AVCodecContext *ctx = open_ctx(codec, channels, rate, ba, avg_bps,
                                       extradata, extradata_size);
        if (ctx) {
            feed_block(ctx, data, (int)size, pkt, frame, channels, &pcm, &len, &cap);
            avcodec_send_packet(ctx, NULL);
            drain(ctx, frame, channels, &pcm, &len, &cap);
            avcodec_free_context(&ctx);
            if (len && duration_ok(len, channels, rate, size, avg_bps))
                how = "whole";
            else {
                if (len && size >= 48 * 1024)
                    fprintf(stderr, "xwma_ffmpeg: whole reject samples=%zu size=%u\n",
                            len / (size_t)channels, size);
                clear_pcm(&pcm, &len, &cap);
            }
        }
    }

    if (!len && cum_bytes && packet_count > 0 && packet_count < 100000) {
        AVCodecContext *ctx = open_ctx(codec, channels, rate, ba, avg_bps,
                                       extradata, extradata_size);
        if (ctx) {
            int sent = 0, skip = 0;
            uint32_t prev = 0;
            for (uint32_t i = 0; i < packet_count; i++) {
                uint32_t end = cum_bytes[i];
                if (end > size) end = size;
                if (end <= prev) continue;
                uint32_t psz = end - prev;
                prev = end;
                if (psz < 32) { skip++; continue; }
                if (feed_block(ctx, data + (end - psz), (int)psz, pkt, frame, channels,
                               &pcm, &len, &cap) >= 0)
                    sent++;
                if (prev >= size) break;
            }
            avcodec_send_packet(ctx, NULL);
            drain(ctx, frame, channels, &pcm, &len, &cap);
            avcodec_free_context(&ctx);
            fprintf(stderr, "xwma_ffmpeg: packets sent=%d skip=%d samples=%zu ba=%d\n",
                    sent, skip, len / (size_t)(channels > 0 ? channels : 1), ba);
            if (len && duration_ok(len, channels, rate, size, avg_bps)) {
                how = "packets";
                used_packets = 1;
            } else {
                if (len)
                    fprintf(stderr, "xwma_ffmpeg: packets reject samples=%zu size=%u\n",
                            len / (size_t)channels, size);
                clear_pcm(&pcm, &len, &cap);
            }
        }
    }

    if (!len && ba >= 32) {
        AVCodecContext *ctx = open_ctx(codec, channels, rate, ba, avg_bps,
                                       extradata, extradata_size);
        if (ctx) {
            int sent = 0;
            for (uint32_t off = 0; off + 32 <= size; ) {
                uint32_t psz = (uint32_t)ba;
                if (off + psz > size) psz = size - off;
                if (psz < 32) break;
                if (feed_block(ctx, data + off, (int)psz, pkt, frame, channels,
                               &pcm, &len, &cap) >= 0)
                    sent++;
                off += psz;
            }
            avcodec_send_packet(ctx, NULL);
            drain(ctx, frame, channels, &pcm, &len, &cap);
            avcodec_free_context(&ctx);
            fprintf(stderr, "xwma_ffmpeg: sliced sent=%d samples=%zu ba=%d\n",
                    sent, len / (size_t)(channels > 0 ? channels : 1), ba);
            if (len && duration_ok(len, channels, rate, size, avg_bps))
                how = "sliced";
            else
                clear_pcm(&pcm, &len, &cap);
        }
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);

    if (!pcm || !len) {
        fprintf(stderr, "xwma_ffmpeg: FAILED tag=%04x size=%u ba=%d/%d packets=%u\n",
                tag, size, block_align, ba, packet_count);
        free(pcm);
        return NULL;
    }

    *out_frames = (uint32_t)(len / (size_t)channels);
    fprintf(stderr,
            "xaudio2: xWMA buffer: %u bytes -> %u frames (%d ch, %d Hz) via %s tag=%04x%s\n",
            size, *out_frames, channels, rate, how ? how : "?", tag,
            used_packets ? " [packets]" : "");
    return pcm;
}

#endif /* LB_HAS_FFMPEG */
