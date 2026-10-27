/* xWMA / WMA decode via FFmpeg libavcodec.
 * Skyrim uses WAVE_FORMAT_WMAUDIO2 (0x0161) / WMAUDIO3 (0x0162) with
 * XAUDIO2_BUFFER_WMA::pDecodedPacketCumulativeBytes (CUMULATIVE end offsets).
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
        fprintf(stderr, "xwma_ffmpeg: decoder not found for tag %04x\n", tag);
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
    ctx->bits_per_coded_sample = 16;

    /* WMA needs WAVEFORMATEX trailing extradata when present */
    if (extradata && extradata_size > 0) {
        ctx->extradata = av_mallocz((size_t)extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!ctx->extradata) {
            avcodec_free_context(&ctx);
            return NULL;
        }
        memcpy(ctx->extradata, extradata, (size_t)extradata_size);
        ctx->extradata_size = extradata_size;
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

    /* Helper: send one encoded packet and drain frames */
    #define DRAIN_FRAMES() do { \
        for (;;) { \
            int r = avcodec_receive_frame(ctx, frame); \
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) break; \
            if (r < 0) { recv_err = r; break; } \
            int n = frame->nb_samples; \
            int ch = channels; \
            size_t need = (len + (size_t)n * (size_t)ch) * sizeof(int16_t); \
            if (need > cap) { \
                size_t ncap = cap ? cap * 2 : need + 8192; \
                if (ncap < need) ncap = need; \
                int16_t *np = realloc(pcm, ncap); \
                if (!np) goto fail; \
                pcm = np; cap = ncap; \
            } \
            if (frame->format == AV_SAMPLE_FMT_S16) { \
                memcpy(pcm + len, frame->data[0], (size_t)n * (size_t)ch * 2); \
            } else if (frame->format == AV_SAMPLE_FMT_FLTP) { \
                for (int i = 0; i < n; i++) \
                    for (int c = 0; c < ch; c++) { \
                        float s = ((float *)frame->data[c])[i]; \
                        if (s > 1.f) s = 1.f; if (s < -1.f) s = -1.f; \
                        pcm[len + (size_t)i * (size_t)ch + (size_t)c] = (int16_t)(s * 32767.f); \
                    } \
            } else if (frame->format == AV_SAMPLE_FMT_FLT) { \
                for (int i = 0; i < n * ch; i++) { \
                    float s = ((float *)frame->data[0])[i]; \
                    if (s > 1.f) s = 1.f; if (s < -1.f) s = -1.f; \
                    pcm[len + (size_t)i] = (int16_t)(s * 32767.f); \
                } \
            } else if (frame->format == AV_SAMPLE_FMT_S16P) { \
                for (int i = 0; i < n; i++) \
                    for (int c = 0; c < ch; c++) \
                        pcm[len + (size_t)i * (size_t)ch + (size_t)c] = ((int16_t *)frame->data[c])[i]; \
            } else { \
                memset(pcm + len, 0, (size_t)n * (size_t)ch * 2); \
            } \
            len += (size_t)n * (size_t)ch; \
            frames_got++; \
        } \
    } while (0)

    /* Build packet list: prefer cumulative table; else block_align slices */
    if (cum_bytes && packet_count > 0) {
        uint32_t prev = 0;
        for (uint32_t pi = 0; pi < packet_count; pi++) {
            uint32_t cum = cum_bytes[pi];
            /* tolerate non-monotonic / oversize entries */
            if (cum < prev) continue;
            if (cum > size) cum = size;
            uint32_t psz = cum - prev;
            if (psz == 0) { prev = cum; continue; }

            /* pad for FFmpeg bitstream readers */
            uint8_t *padded = av_malloc((size_t)psz + AV_INPUT_BUFFER_PADDING_SIZE);
            if (!padded) goto fail;
            memcpy(padded, data + prev, psz);
            memset(padded + psz, 0, AV_INPUT_BUFFER_PADDING_SIZE);

            pkt->data = padded;
            pkt->size = (int)psz;
            err = avcodec_send_packet(ctx, pkt);
            av_free(padded);
            pkt->data = NULL;
            pkt->size = 0;
            if (err < 0) {
                send_err = err;
                /* keep going — some packets may be headers/partial */
            } else {
                packets_sent++;
            }
            DRAIN_FRAMES();
            prev = cum;
            if (prev >= size) break;
        }
    } else {
        /* No table: split by block_align if sensible, else whole buffer */
        uint32_t step = (block_align > 0 && (size_t)block_align * 2 < size)
                        ? (uint32_t)block_align : size;
        for (uint32_t off = 0; off < size; ) {
            uint32_t psz = step;
            if (off + psz > size) psz = size - off;
            if (psz == 0) break;

            uint8_t *padded = av_malloc((size_t)psz + AV_INPUT_BUFFER_PADDING_SIZE);
            if (!padded) goto fail;
            memcpy(padded, data + off, psz);
            memset(padded + psz, 0, AV_INPUT_BUFFER_PADDING_SIZE);

            pkt->data = padded;
            pkt->size = (int)psz;
            err = avcodec_send_packet(ctx, pkt);
            av_free(padded);
            pkt->data = NULL;
            pkt->size = 0;
            if (err < 0) send_err = err;
            else packets_sent++;
            DRAIN_FRAMES();
            off += psz;
        }
    }

    /* flush */
    avcodec_send_packet(ctx, NULL);
    DRAIN_FRAMES();

#undef DRAIN_FRAMES

    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);

    if (!pcm || !len) {
        fprintf(stderr,
                "xwma_ffmpeg: no PCM tag=%04x size=%u pkts=%u sent=%d frames=%d send_err=%d recv_err=%d extra=%d\n",
                tag, size, packet_count, packets_sent, frames_got, send_err, recv_err, extradata_size);
        if (send_err) xa_err("last send", send_err);
        if (recv_err) xa_err("last recv", recv_err);
        free(pcm);
        return NULL;
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
