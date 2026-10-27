/* xWMA / WMA decode via FFmpeg libavcodec.
 * Built only when LB_HAS_FFMPEG is defined and libavcodec is linked.
 *
 * Skyrim submits raw WMA packets (tag 0x0161 = WMAV2, 0x0162 = WMAPRO)
 * with an optional XAUDIO2_BUFFER_WMA seek table (packet sizes).
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

int16_t *xwma_decode(const uint8_t *data, uint32_t size, int channels, int rate,
                     int block_align, int avg_bps, int tag,
                     const uint32_t *packet_info, uint32_t packet_count,
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

    if (avcodec_open2(ctx, codec, NULL) < 0) {
        fprintf(stderr, "xwma_ffmpeg: avcodec_open2 failed tag=%04x\n", tag);
        avcodec_free_context(&ctx);
        return NULL;
    }

    size_t cap = 0, len = 0;
    int16_t *pcm = NULL;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    if (!pkt || !frame) goto fail;

    uint32_t off = 0;
    uint32_t pi = 0;
    for (;;) {
        uint32_t psz;
        if (packet_info && packet_count && pi < packet_count) {
            psz = packet_info[pi++];
            if (psz == 0 || off + psz > size) break;
        } else if (!packet_info || !packet_count) {
            if (off >= size) break;
            if (block_align > 0 && off + (uint32_t)block_align <= size)
                psz = (uint32_t)block_align;
            else
                psz = size - off;
        } else break;

        pkt->data = (uint8_t *)(data + off);
        pkt->size = (int)psz;
        off += psz;

        if (avcodec_send_packet(ctx, pkt) < 0) continue;
        for (;;) {
            int r = avcodec_receive_frame(ctx, frame);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) break;
            if (r < 0) goto fail;

            int n = frame->nb_samples;
            int ch = channels;
            size_t need = (len + (size_t)n * (size_t)ch) * sizeof(int16_t);
            if (need > cap) {
                size_t ncap = cap ? cap * 2 : need + 4096;
                if (ncap < need) ncap = need;
                int16_t *np = realloc(pcm, ncap);
                if (!np) goto fail;
                pcm = np;
                cap = ncap;
            }
            if (frame->format == AV_SAMPLE_FMT_S16) {
                memcpy(pcm + len, frame->data[0], (size_t)n * (size_t)ch * 2);
            } else if (frame->format == AV_SAMPLE_FMT_FLTP) {
                for (int i = 0; i < n; i++)
                    for (int c = 0; c < ch; c++) {
                        float s = ((float *)frame->data[c])[i];
                        if (s > 1) s = 1;
                        if (s < -1) s = -1;
                        pcm[len + (size_t)i * (size_t)ch + (size_t)c] = (int16_t)(s * 32767.f);
                    }
            } else if (frame->format == AV_SAMPLE_FMT_S16P) {
                for (int i = 0; i < n; i++)
                    for (int c = 0; c < ch; c++)
                        pcm[len + (size_t)i * (size_t)ch + (size_t)c] =
                            ((int16_t *)frame->data[c])[i];
            } else {
                memset(pcm + len, 0, (size_t)n * (size_t)ch * 2);
            }
            len += (size_t)n * (size_t)ch;
        }
        if (!packet_info || !packet_count) break;
    }

    avcodec_send_packet(ctx, NULL);
    for (;;) {
        int r = avcodec_receive_frame(ctx, frame);
        if (r < 0) break;
        int n = frame->nb_samples;
        int ch = channels;
        size_t need = (len + (size_t)n * (size_t)ch) * sizeof(int16_t);
        if (need > cap) {
            int16_t *np = realloc(pcm, need + 1024);
            if (!np) break;
            pcm = np;
            cap = need + 1024;
        }
        if (frame->format == AV_SAMPLE_FMT_S16)
            memcpy(pcm + len, frame->data[0], (size_t)n * (size_t)ch * 2);
        else
            memset(pcm + len, 0, (size_t)n * (size_t)ch * 2);
        len += (size_t)n * (size_t)ch;
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);

    if (!pcm || !len) { free(pcm); return NULL; }
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
