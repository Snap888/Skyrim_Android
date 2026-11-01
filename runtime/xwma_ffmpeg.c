/* xWMA decode via FFmpeg for Skyrim (tag 0x0161 WMAV2 / 0x0162 WMAPRO).
 *
 * Voices (~3-12KB): whole buffer OK.
 * Music (~200KB+): whole buffer often yields only 2048 samples — reject those.
 * Packet sizes from XAUDIO2_BUFFER_WMA are often < block_align; forcing
 * ctx->block_align breaks them (Input packet size too small).
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
                                int block_align, int avg_bps, int force_ba,
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
    if (force_ba && block_align > 0) ctx->block_align = block_align;
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

static int duration_ok(size_t len, int channels, int rate, uint32_t size, int avg_bps)
{
    if (!len || channels < 1 || rate < 1) return 0;
    double sec = (double)(len / (size_t)channels) / (double)rate;
    if (sec >= 0.05 && size < 48 * 1024) return 1;
    double exp_sec = (avg_bps > 0) ? (double)size / (double)avg_bps : 0;
    if (exp_sec <= 0.05) return sec >= 0.02;
    return (sec >= exp_sec * 0.20) || (sec >= 1.0 && size > 64 * 1024);
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
    int is_voice = (size < 48 * 1024);

    if (is_voice) {
        AVCodecContext *ctx = open_ctx(codec, channels, rate, block_align, avg_bps, 0,
                                       extradata, extradata_size);
        if (ctx) {
            feed_block(ctx, data, (int)size, pkt, frame, channels, &pcm, &len, &cap);
            avcodec_send_packet(ctx, NULL);
            drain(ctx, frame, channels, &pcm, &len, &cap);
            avcodec_free_context(&ctx);
            if (len && duration_ok(len, channels, rate, size, avg_bps))
                fprintf(stderr, "xwma_ffmpeg: voice ok samples=%zu size=%u tag=%04x\n",
                        len / (size_t)channels, size, tag);
            else
                clear_pcm(&pcm, &len, &cap);
        }
    }

    if (!len && cum_bytes && packet_count > 0 && packet_count < 100000) {
        AVCodecContext *ctx = open_ctx(codec, channels, rate, block_align, avg_bps, 0,
                                       extradata, extradata_size);
        if (ctx) {
            int sent = 0, fails = 0;
            uint32_t prev = 0;
            for (uint32_t i = 0; i < packet_count; i++) {
                uint32_t end = cum_bytes[i];
                if (end > size) end = size;
                if (end <= prev) continue;
                uint32_t psz = end - prev;
                int err = feed_block(ctx, data + prev, (int)psz, pkt, frame, channels,
                                     &pcm, &len, &cap);
                if (err < 0) fails++;
                else sent++;
                prev = end;
                if (prev >= size) break;
            }
            avcodec_send_packet(ctx, NULL);
            drain(ctx, frame, channels, &pcm, &len, &cap);
            avcodec_free_context(&ctx);
            fprintf(stderr, "xwma_ffmpeg: packets=%u sent=%d fail=%d samples=%zu\n",
                    packet_count, sent, fails, len / (size_t)(channels > 0 ? channels : 1));
            if (len && duration_ok(len, channels, rate, size, avg_bps))
                used_packets = 1;
            else
                clear_pcm(&pcm, &len, &cap);
        }
    }

    if (!len) {
        AVCodecContext *ctx = open_ctx(codec, channels, rate, block_align, avg_bps, 0,
                                       extradata, extradata_size);
        if (ctx) {
            feed_block(ctx, data, (int)size, pkt, frame, channels, &pcm, &len, &cap);
            avcodec_send_packet(ctx, NULL);
            drain(ctx, frame, channels, &pcm, &len, &cap);
            avcodec_free_context(&ctx);
            if (len && duration_ok(len, channels, rate, size, avg_bps))
                fprintf(stderr, "xwma_ffmpeg: whole ok samples=%zu size=%u tag=%04x\n",
                        len / (size_t)channels, size, tag);
            else {
                if (len)
                    fprintf(stderr, "xwma_ffmpeg: whole too short samples=%zu size=%u — reject\n",
                            len / (size_t)channels, size);
                clear_pcm(&pcm, &len, &cap);
            }
        }
    }

    if (!len && block_align > 32) {
        AVCodecContext *ctx = open_ctx(codec, channels, rate, block_align, avg_bps, 1,
                                       extradata, extradata_size);
        if (ctx) {
            uint32_t ba = (uint32_t)block_align;
            int sent = 0;
            if (cum_bytes && packet_count > 0) {
                uint32_t prev = 0, acc_off = 0, acc_sz = 0;
                for (uint32_t i = 0; i < packet_count; i++) {
                    uint32_t end = cum_bytes[i];
                    if (end > size) end = size;
                    if (end <= prev) continue;
                    if (!acc_sz) acc_off = prev;
                    acc_sz += end - prev;
                    prev = end;
                    if (acc_sz >= ba || i + 1 == packet_count || prev >= size) {
                        if (feed_block(ctx, data + acc_off, (int)acc_sz, pkt, frame, channels,
                                       &pcm, &len, &cap) >= 0)
                            sent++;
                        acc_sz = 0;
                    }
                    if (prev >= size) break;
                }
            } else {
                for (uint32_t off = 0; off + 16 < size; ) {
                    uint32_t psz = ba;
                    if (off + psz > size) psz = size - off;
                    if (feed_block(ctx, data + off, (int)psz, pkt, frame, channels,
                                   &pcm, &len, &cap) >= 0)
                        sent++;
                    off += psz;
                }
            }
            avcodec_send_packet(ctx, NULL);
            drain(ctx, frame, channels, &pcm, &len, &cap);
            avcodec_free_context(&ctx);
            fprintf(stderr, "xwma_ffmpeg: merged-ba sent=%d samples=%zu ba=%d\n",
                    sent, len / (size_t)(channels > 0 ? channels : 1), block_align);
            if (!duration_ok(len, channels, rate, size, avg_bps))
                clear_pcm(&pcm, &len, &cap);
        }
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);

    if (!pcm || !len) {
        fprintf(stderr, "xwma_ffmpeg: FAILED tag=%04x size=%u ba=%d extra=%d packets=%u\n",
                tag, size, block_align, extradata_size, packet_count);
        free(pcm);
        return NULL;
    }

    *out_frames = (uint32_t)(len / (size_t)channels);
    fprintf(stderr,
            "xaudio2: xWMA buffer: %u bytes -> %u frames (%u decoded, %d ch, %d Hz) tag=%04x pkt=%u%s\n",
            size, *out_frames, *out_frames, channels, rate, tag, packet_count,
            used_packets ? " [packets]" : "");
    return pcm;
}

#endif /* LB_HAS_FFMPEG */
