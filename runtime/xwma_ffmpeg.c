/* xWMA decode via FFmpeg — Skyrim voices/music (tag 0x0161 / 0x0162). */
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

/* Common WMA2 extradata when WAVEFORMATEX.cbSize == 0 (many game streams). */
static const uint8_t k_wma2_extra_default[] = {
    0x00, 0x88, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00
};

static void xa_err(const char *what, int err)
{
    char buf[128];
    av_strerror(err, buf, sizeof buf);
    fprintf(stderr, "xwma_ffmpeg: %s: %s (%d)\n", what, buf, err);
}

static int frame_channels(const AVFrame *frame, int fallback)
{
#if LIBAVCODEC_VERSION_MAJOR >= 59
    int n = frame->ch_layout.nb_channels;
#else
    int n = frame->channels;
#endif
    return n > 0 ? n : fallback;
}

static int append_frame(AVFrame *frame, int want_ch,
                        int16_t **pcm, size_t *len, size_t *cap)
{
    int n = frame->nb_samples;
    int src_ch = frame_channels(frame, want_ch);
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
                s = ((int16_t *)frame->data[0])[i * src_ch + sc] / 32768.f;
                break;
            case AV_SAMPLE_FMT_S16P:
                s = ((int16_t *)frame->data[sc])[i] / 32768.f;
                break;
            case AV_SAMPLE_FMT_FLT:
                s = ((float *)frame->data[0])[i * src_ch + sc];
                break;
            case AV_SAMPLE_FMT_FLTP:
                s = ((float *)frame->data[sc])[i];
                break;
            default:
                s = 0.f;
                break;
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
                 int16_t **pcm, size_t *len, size_t *cap, int *got)
{
    for (;;) {
        int r = avcodec_receive_frame(ctx, frame);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return 0;
        if (r < 0) return r;
        if (append_frame(frame, want_ch, pcm, len, cap) < 0) return AVERROR(ENOMEM);
        (*got)++;
    }
}

static int send_raw(AVCodecContext *ctx, const uint8_t *p, int psz, AVPacket *pkt)
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
    return err;
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
        fprintf(stderr, "xwma_ffmpeg: no decoder for tag %04x\n", tag);
        return NULL;
    }

    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx) return NULL;

    ctx->sample_rate = rate > 0 ? rate : 44100;
#if LIBAVCODEC_VERSION_MAJOR >= 59
    av_channel_layout_default(&ctx->ch_layout, channels);
#else
    ctx->channels = channels;
    ctx->channel_layout = av_get_default_channel_layout(channels);
#endif
    ctx->block_align = block_align > 0 ? block_align : 0;
    if (avg_bps > 0) ctx->bit_rate = (int64_t)avg_bps * 8;
    ctx->request_sample_fmt = AV_SAMPLE_FMT_FLTP;

    const uint8_t *extra = extradata;
    int extra_sz = extradata_size;
    if ((!extra || extra_sz <= 0) && tag == 0x0161) {
        extra = k_wma2_extra_default;
        extra_sz = (int)sizeof k_wma2_extra_default;
    }
    if (extra && extra_sz > 0) {
        ctx->extradata = av_mallocz((size_t)extra_sz + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!ctx->extradata) { avcodec_free_context(&ctx); return NULL; }
        memcpy(ctx->extradata, extra, (size_t)extra_sz);
        ctx->extradata_size = extra_sz;
    }

    int err = avcodec_open2(ctx, codec, NULL);
    if (err < 0) {
        xa_err("avcodec_open2", err);
        avcodec_free_context(&ctx);
        return NULL;
    }

    size_t cap = 0, len = 0;
    int16_t *pcm = NULL;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    if (!pkt || !frame) goto fail;

    int packets_sent = 0, frames_got = 0, send_err = 0, recv_err = 0;
    int send_fail = 0;

    int use_cum = 0;
    if (cum_bytes && packet_count > 0) {
        uint32_t last = cum_bytes[packet_count - 1];
        if (cum_bytes[0] <= size && (last == size || last + 8 >= size))
            use_cum = 1;
    }

    if (!use_cum && block_align <= 0) {
        err = send_raw(ctx, data, (int)size, pkt);
        if (err < 0) send_err = err; else packets_sent++;
        err = drain(ctx, frame, channels, &pcm, &len, &cap, &frames_got);
        if (err < 0) recv_err = err;
    } else if (use_cum) {
        uint32_t prev = 0;
        for (uint32_t i = 0; i < packet_count; i++) {
            uint32_t cum = cum_bytes[i];
            if (cum < prev) continue;
            if (cum > size) cum = size;
            uint32_t psz = cum - prev;
            if (!psz) { prev = cum; continue; }
            err = send_raw(ctx, data + prev, (int)psz, pkt);
            if (err < 0) { send_err = err; send_fail++; }
            else packets_sent++;
            err = drain(ctx, frame, channels, &pcm, &len, &cap, &frames_got);
            if (err < 0) { recv_err = err; break; }
            prev = cum;
            if (prev >= size) break;
        }
    } else {
        uint32_t ba = (uint32_t)block_align;
        for (uint32_t off = 0; off < size; ) {
            uint32_t psz = ba;
            if (off + psz > size) psz = size - off;
            if (!psz) break;
            if (psz < ba && psz < 64) break;
            err = send_raw(ctx, data + off, (int)psz, pkt);
            if (err < 0) { send_err = err; send_fail++; }
            else packets_sent++;
            err = drain(ctx, frame, channels, &pcm, &len, &cap, &frames_got);
            if (err < 0) { recv_err = err; break; }
            off += psz;
        }
    }

    avcodec_send_packet(ctx, NULL);
    {
        int e = drain(ctx, frame, channels, &pcm, &len, &cap, &frames_got);
        if (e < 0) recv_err = e;
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);

    if (!pcm || !len) {
        fprintf(stderr,
                "xwma_ffmpeg: no PCM tag=%04x size=%u ba=%d tbl=%u sent=%d fail=%d got=%d "
                "use_cum=%d se=%d re=%d extra=%d\n",
                tag, size, block_align, packet_count, packets_sent, send_fail, frames_got,
                use_cum, send_err, recv_err, extra_sz);
        if (send_err) xa_err("send", send_err);
        if (recv_err) xa_err("recv", recv_err);
        free(pcm);
        return NULL;
    }

    if (send_fail > 0 || (avg_bps > 0 && rate > 0)) {
        double sec_cmp = avg_bps > 0 ? (double)size / (double)avg_bps : 0;
        double sec_pcm = (double)(len / (size_t)channels) / (double)(rate > 0 ? rate : 1);
        fprintf(stderr,
                "xwma_ffmpeg: ok tag=%04x pcm=%.2fs cmp=%.2fs frames=%u sent=%d fail=%d use_cum=%d ba=%d extra=%d\n",
                tag, sec_pcm, sec_cmp, (unsigned)(len / (size_t)channels),
                packets_sent, send_fail, use_cum, block_align, extra_sz);
    }

    *out_frames = (uint32_t)(len / (size_t)channels);
    return pcm;

fail:
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    free(pcm);
    return NULL;
}

#endif /* LB_HAS_FFMPEG */
