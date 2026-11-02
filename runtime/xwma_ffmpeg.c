/* xWMA decode via FFmpeg for Skyrim (tag 0x0161 / 0x0162).
 *
 * xWMA data is a sequence of block_align compressed packets; the final
 * packet may be shorter. Preserve decoder state across packet submissions.
 * XAUDIO2_BUFFER_WMA::pDecodedPacketCumulativeBytes is decoded PCM byte
 * accounting (the RIFF dpds table), never compressed input offsets.
 */
#ifdef LB_HAS_FFMPEG

#include <stdint.h>
#include <limits.h>
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

static int xwma_normalize_bitrate(int channels, int rate, int bitrate)
{
    /* Match libavformat/xwma.c: some xWMA encoders write placeholder bitrates. */
    if (channels == 1) {
        if ((rate == 22050 || rate == 32000) &&
            (bitrate == 48000 || bitrate == 192000))
            bitrate = 20000;
        else if (rate == 44100 && (bitrate == 96000 || bitrate == 192000))
            bitrate = 48000;
    } else if (channels == 2) {
        if (rate == 22050 && (bitrate == 48000 || bitrate == 192000))
            bitrate = 32000;
        else if (rate == 32000 && bitrate == 192000)
            bitrate = 48000;
    }
    return bitrate;
}

static AVCodecContext *open_ctx(const AVCodec *codec, int channels, int rate,
                                int block_align, int avg_bps, int bits_per_sample,
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
    if (bits_per_sample > 0) ctx->bits_per_coded_sample = bits_per_sample;
    if (avg_bps > 0)
        ctx->bit_rate = xwma_normalize_bitrate(channels, ctx->sample_rate, avg_bps * 8);
    ctx->request_sample_fmt = AV_SAMPLE_FMT_FLTP;

    /* xWMA RIFF files normally omit WMA codec extradata. Match the fake
     * extradata synthesized by FFmpeg's libavformat/xwma.c demuxer. */
    if ((!extradata || extradata_size <= 0) && codec->id == AV_CODEC_ID_WMAV2) {
        extradata_size = 6;
        extradata = NULL;
    } else if ((!extradata || extradata_size <= 0) && codec->id == AV_CODEC_ID_WMAPRO) {
        extradata_size = 18;
        extradata = NULL;
    }
    if (extradata_size > 0) {
        ctx->extradata = av_mallocz((size_t)extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!ctx->extradata) { avcodec_free_context(&ctx); return NULL; }
        if (extradata) {
            memcpy(ctx->extradata, extradata, (size_t)extradata_size);
        } else if (codec->id == AV_CODEC_ID_WMAV2 && extradata_size >= 6) {
            ctx->extradata[4] = 31;
        } else if (codec->id == AV_CODEC_ID_WMAPRO && extradata_size >= 18) {
            ctx->extradata[0] = (uint8_t)(bits_per_sample > 0 ? bits_per_sample : 16);
            ctx->extradata[14] = 224;
        }
        ctx->extradata_size = extradata_size;
    }
    int open_err = avcodec_open2(ctx, codec, NULL);
    if (open_err < 0) {
        xa_err("avcodec_open2", open_err);
        fprintf(stderr,
                "xwma_ffmpeg: codec open rejected tag=%04x ch=%d rate=%d block_align=%d avg_bps=%d extra=%d\n",
                codec->id == AV_CODEC_ID_WMAPRO ? 0x0162 : 0x0161,
                channels, ctx->sample_rate, ctx->block_align, avg_bps,
                extradata_size);
        avcodec_free_context(&ctx);
        return NULL;
    }
    return ctx;
}

static int feed_block(AVCodecContext *ctx, const uint8_t *p, int psz,
                      AVPacket *pkt, AVFrame *frame, int channels,
                      int16_t **pcm, size_t *len, size_t *cap)
{
    av_packet_unref(pkt);
    int err = av_new_packet(pkt, psz);
    if (err < 0) return err;
    memcpy(pkt->data, p, (size_t)psz);

    err = avcodec_send_packet(ctx, pkt);
    if (err == AVERROR(EAGAIN)) {
        /* Drain pending output and retry this exact packet. */
        int dr = drain(ctx, frame, channels, pcm, len, cap);
        if (dr < 0) {
            av_packet_unref(pkt);
            return dr;
        }
        err = avcodec_send_packet(ctx, pkt);
    }
    av_packet_unref(pkt);
    if (err < 0) return err;
    return drain(ctx, frame, channels, pcm, len, cap);
}

int16_t *xwma_decode(const uint8_t *data, uint32_t size, int channels, int rate,
                     int block_align, int avg_bps, int tag, int bits_per_sample,
                     const uint32_t *cum_bytes, uint32_t packet_count,
                     const uint8_t *extradata, int extradata_size,
                     uint32_t *out_frames)
{
    *out_frames = 0;
    if (!data || !size || channels < 1 || channels > 8) return NULL;

    /* xWMA uses fixed block_align packets; the final packet may be short.
     * Match FFmpeg's xWMA demuxer, which reads min(block_align, bytes_left). */
    uint32_t packet_size = (block_align > 1) ? (uint32_t)block_align : 2230u;
    if (!packet_size || !size) return NULL;
    uint32_t expected_packets = (size - 1u) / packet_size + 1u;
    if (packet_count && packet_count != expected_packets) {
        fprintf(stderr,
                "xwma_ffmpeg: packet count mismatch descriptor=%u expected=%u size=%u block_align=%u\n",
                packet_count, expected_packets, size, packet_size);
        return NULL;
    }
    if (!packet_count) packet_count = expected_packets;
    if (!packet_count || packet_size > INT_MAX) return NULL;

    /* Validate dpds values without interpreting them as compressed offsets. */
    uint32_t dpds_last = 0;
    int dpds_valid = cum_bytes != NULL;
    if (dpds_valid) {
        uint32_t prev = 0;
        for (uint32_t i = 0; i < packet_count; i++) {
            uint32_t value = cum_bytes[i];
            if (value < prev) { dpds_valid = 0; break; }
            prev = value;
        }
        if (dpds_valid) {
            dpds_last = prev;
            if (dpds_last % (uint32_t)(channels * 2) != 0) dpds_valid = 0;
        }
        if (!dpds_valid) fprintf(stderr, "xwma_ffmpeg: invalid dpds table (non-monotonic or misaligned) (%u packets)\n",
                     packet_count);
    }

    enum AVCodecID id = (tag == 0x0162) ? AV_CODEC_ID_WMAPRO : AV_CODEC_ID_WMAV2;
    const AVCodec *codec = avcodec_find_decoder(id);
    if (!codec) {
        fprintf(stderr, "xwma_ffmpeg: no decoder tag=%04x\n", tag);
        return NULL;
    }

    size_t cap = 0, len = 0;
    int16_t *pcm = NULL;
    int decode_failed = 0;
    int codec_extra_size = 0, codec_flags = -1, codec_bitrate = 0;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    if (!pkt || !frame) {
        av_packet_free(&pkt);
        av_frame_free(&frame);
        return NULL;
    }

    AVCodecContext *ctx = open_ctx(codec, channels, rate, (int)packet_size,
                                   avg_bps, bits_per_sample, extradata, extradata_size);
    if (!ctx) {
        fprintf(stderr,
                "xwma_ffmpeg: open failed tag=%04x ch=%d rate=%d packet=%u extra=%d\n",
                tag, channels, rate, packet_size, extradata_size);
        goto done;
    }
    codec_extra_size = ctx->extradata_size;
    codec_bitrate = (int)(ctx->bit_rate / 1000);
    if (tag == 0x0161 && ctx->extradata_size >= 6)
        codec_flags = (int)ctx->extradata[4] | ((int)ctx->extradata[5] << 8);

    /* xWMA's data chunk is a sequence of fixed-size codec packets. Feeding the
     * complete data chunk as one AVPacket is not equivalent: WMA decoders may
     * consume only one block and leave the remainder unprocessed. */
    for (uint32_t i = 0; i < packet_count; i++) {
        size_t offset = (size_t)i * packet_size;
        uint32_t remaining = size - (uint32_t)offset;
        int this_packet_size = (int)(remaining < packet_size ? remaining : packet_size);
        int err = feed_block(ctx, data + offset, this_packet_size,
                             pkt, frame, channels, &pcm, &len, &cap);
        if (err < 0) {
            xa_err("packet decode", err);
            fprintf(stderr,
                    "xwma_ffmpeg: packet %u/%u failed size=%u tag=%04x\n",
                    i + 1, packet_count, packet_size, tag);
            avcodec_free_context(&ctx);
            decode_failed = 1;
            goto done;
        }
    }

    /* Drain codec delay/tail after the final compressed packet. */
    int flush_err = avcodec_send_packet(ctx, NULL);
    if (flush_err == AVERROR(EAGAIN)) {
        int dr = drain(ctx, frame, channels, &pcm, &len, &cap);
        if (dr >= 0) flush_err = avcodec_send_packet(ctx, NULL);
        else flush_err = dr;
    }
    if (flush_err < 0 && flush_err != AVERROR_EOF) {
        xa_err("decoder flush", flush_err);
        decode_failed = 1;
    } else {
        int dr = drain(ctx, frame, channels, &pcm, &len, &cap);
        if (dr < 0) {
            xa_err("flush drain", dr);
            decode_failed = 1;
        }
    }
    avcodec_free_context(&ctx);

    if (dpds_valid) {
        size_t decoded_bytes = len * sizeof(int16_t);
        if (decoded_bytes != (size_t)dpds_last) {
            fprintf(stderr,
                    "xwma_ffmpeg: dpds mismatch decoded=%zu expected=%u bytes (packets=%u tag=%04x)\n",
                    decoded_bytes, dpds_last, packet_count, tag);
            decode_failed = 1;
        }
    }

    {
        uint32_t frames = (uint32_t)(len / (size_t)channels);
        double sec_pcm = (rate > 0) ? (double)frames / (double)rate : 0.0;
        double sec_dpds = (dpds_valid && rate > 0)
            ? (double)dpds_last / (double)(channels * 2 * rate) : 0.0;
        fprintf(stderr,
                "xwma_ffmpeg: packets=%u block_align=%u pcm_frames=%u pcm=%.3fs dpds=%.3fs tag=%04x ba=%d extra=%d flags=%d bitrate=%dkbps\n",
                packet_count, packet_size, frames, sec_pcm, sec_dpds,
                tag, block_align, codec_extra_size, codec_flags, codec_bitrate);
    }

done:
    av_frame_free(&frame);
    av_packet_free(&pkt);

    if (decode_failed || !pcm || !len) {
        fprintf(stderr, "xwma_ffmpeg: FAILED tag=%04x size=%u packets=%u ba=%d extra=%d\n",
                tag, size, packet_count, block_align, extradata_size);
        free(pcm);
        return NULL;
    }
    *out_frames = (uint32_t)(len / (size_t)channels);
    return pcm;
}

#endif /* LB_HAS_FFMPEG */
