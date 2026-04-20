/*
 * Copyright (C) 2025 mpv project
 *
 * NETWORK-mode audio extractor for whisper-lookahead.
 *
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>

#include "common/msg.h"
#include "misc/thread_tools.h"
#include "osdep/io.h"
#include "stream/stream.h"

#include "whisper_pcm.h"

#define WPCM_SAMPLE_RATE 16000
#define WPCM_BYTES_PER_SAMPLE 2  // s16

// Minimal RIFF/WAVE header for PCM s16 mono.
// data_size = num_samples * WPCM_BYTES_PER_SAMPLE.
static int write_wav_header(FILE *f, uint32_t data_size, uint32_t sample_rate)
{
    uint8_t hdr[44];
    uint32_t riff_size = 36 + data_size;
    uint32_t fmt_chunk = 16;
    uint16_t fmt_pcm = 1;
    uint16_t channels = 1;
    uint16_t bits = 16;
    uint32_t byte_rate = sample_rate * channels * (bits / 8);
    uint16_t block_align = channels * (bits / 8);

    memcpy(hdr + 0, "RIFF", 4);
    hdr[4] = riff_size & 0xff;
    hdr[5] = (riff_size >> 8) & 0xff;
    hdr[6] = (riff_size >> 16) & 0xff;
    hdr[7] = (riff_size >> 24) & 0xff;
    memcpy(hdr + 8, "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    hdr[16] = fmt_chunk & 0xff;
    hdr[17] = (fmt_chunk >> 8) & 0xff;
    hdr[18] = (fmt_chunk >> 16) & 0xff;
    hdr[19] = (fmt_chunk >> 24) & 0xff;
    hdr[20] = fmt_pcm & 0xff;
    hdr[21] = (fmt_pcm >> 8) & 0xff;
    hdr[22] = channels & 0xff;
    hdr[23] = (channels >> 8) & 0xff;
    hdr[24] = sample_rate & 0xff;
    hdr[25] = (sample_rate >> 8) & 0xff;
    hdr[26] = (sample_rate >> 16) & 0xff;
    hdr[27] = (sample_rate >> 24) & 0xff;
    hdr[28] = byte_rate & 0xff;
    hdr[29] = (byte_rate >> 8) & 0xff;
    hdr[30] = (byte_rate >> 16) & 0xff;
    hdr[31] = (byte_rate >> 24) & 0xff;
    hdr[32] = block_align & 0xff;
    hdr[33] = (block_align >> 8) & 0xff;
    hdr[34] = bits & 0xff;
    hdr[35] = (bits >> 8) & 0xff;
    memcpy(hdr + 36, "data", 4);
    hdr[40] = data_size & 0xff;
    hdr[41] = (data_size >> 8) & 0xff;
    hdr[42] = (data_size >> 16) & 0xff;
    hdr[43] = (data_size >> 24) & 0xff;

    if (fwrite(hdr, 1, 44, f) != 44)
        return -1;
    return 0;
}

static int wpcm_interrupt_cb(void *opaque)
{
    struct mp_cancel *c = opaque;
    return c && mp_cancel_test(c) ? 1 : 0;
}

int wpcm_extract_chunk_to_wav(struct mp_log *log,
                              struct mpv_global *global,
                              struct mp_cancel *cancel,
                              const char *url,
                              double t0_sec,
                              double t1_sec,
                              const char *out_wav_path)
{
    int rc = -1;
    AVFormatContext *fmt = NULL;
    AVCodecContext *cctx = NULL;
    AVPacket *pkt = NULL;
    AVFrame *frame = NULL;
    SwrContext *swr = NULL;
    AVDictionary *opts = NULL;
    FILE *out = NULL;
    int audio_idx = -1;
    uint32_t total_samples = 0;
    uint8_t *buf_out = NULL;
    int buf_out_cap = 0;
    bool swr_inited = false;

    if (cancel && mp_cancel_test(cancel))
        return -2;

    // Pre-allocate format context so we can attach the interrupt callback
    // BEFORE network I/O begins (otherwise avformat_open_input itself
    // can block uncancellably).
    fmt = avformat_alloc_context();
    if (!fmt) return -1;
    fmt->interrupt_callback.callback = wpcm_interrupt_cb;
    fmt->interrupt_callback.opaque = cancel;

    // Build network options dict (auth headers, UA, cookies, etc).
    mp_setup_av_network_options(&opts, NULL, global, log);
    // Mirror what stream_lavf does for resilience against flaky HTTP servers.
    av_dict_set(&opts, "reconnect", "1", 0);
    av_dict_set(&opts, "reconnect_streamed", "1", 0);
    av_dict_set(&opts, "reconnect_delay_max", "7", 0);
    // seekable=0 disables byte-Range requests entirely. Many shared/proxy
    // servers (alist /dav, some Emby reverse proxies, signed-URL CDNs)
    // accept the initial GET but 403 on Range. Since we re-open per chunk
    // and read sequentially anyway, byte-range seeking buys us little --
    // av_seek_frame will fall back to read-and-discard either way.
    av_dict_set(&opts, "seekable", "0", 0);

    int err = avformat_open_input(&fmt, url, NULL, &opts);
    av_dict_free(&opts);
    opts = NULL;
    if (err < 0) {
        char ebuf[128] = {0};
        av_strerror(err, ebuf, sizeof(ebuf));
        mp_err(log, "wpcm: avformat_open_input failed: %s (%d)\n", ebuf, err);
        // avformat_open_input frees fmt on failure.
        fmt = NULL;
        return cancel && mp_cancel_test(cancel) ? -2 : -1;
    }

    if (cancel && mp_cancel_test(cancel)) { rc = -2; goto cleanup; }

    err = avformat_find_stream_info(fmt, NULL);
    if (err < 0) {
        mp_err(log, "wpcm: find_stream_info failed: %d\n", err);
        goto cleanup;
    }

    audio_idx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (audio_idx < 0) {
        mp_err(log, "wpcm: no audio stream\n");
        goto cleanup;
    }

    AVStream *st = fmt->streams[audio_idx];
    const AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) {
        mp_err(log, "wpcm: no decoder for audio codec %d\n",
               st->codecpar->codec_id);
        goto cleanup;
    }

    cctx = avcodec_alloc_context3(dec);
    if (!cctx) goto cleanup;
    if (avcodec_parameters_to_context(cctx, st->codecpar) < 0)
        goto cleanup;
    cctx->pkt_timebase = st->time_base;
    if (avcodec_open2(cctx, dec, NULL) < 0) {
        mp_err(log, "wpcm: avcodec_open2 failed\n");
        goto cleanup;
    }

    // Seek slightly before t0; decoder skips until first valid frame.
    if (t0_sec > 0.0) {
        int64_t ts = (int64_t)((t0_sec - 0.5) * AV_TIME_BASE);
        if (ts < 0) ts = 0;
        if (av_seek_frame(fmt, -1, ts, AVSEEK_FLAG_BACKWARD) < 0) {
            mp_warn(log, "wpcm: seek to %.2f failed; reading from start\n",
                    t0_sec);
        }
    }

    out = fopen(out_wav_path, "wb");
    if (!out) {
        mp_err(log, "wpcm: cannot open output '%s'\n", out_wav_path);
        goto cleanup;
    }
    if (write_wav_header(out, 0, WPCM_SAMPLE_RATE) < 0)
        goto cleanup;

    pkt = av_packet_alloc();
    frame = av_frame_alloc();
    if (!pkt || !frame) goto cleanup;

    int64_t t1_pts_in_st = (int64_t)(t1_sec / av_q2d(st->time_base));
    bool eof_input = false;

    while (true) {
        if (cancel && mp_cancel_test(cancel)) { rc = -2; goto cleanup; }

        if (!eof_input) {
            err = av_read_frame(fmt, pkt);
            if (err == AVERROR_EOF) {
                eof_input = true;
                avcodec_send_packet(cctx, NULL);
            } else if (err == AVERROR_EXIT) {
                // interrupt callback fired
                rc = -2;
                goto cleanup;
            } else if (err < 0) {
                mp_warn(log, "wpcm: av_read_frame error %d\n", err);
                break;
            } else {
                if (pkt->stream_index != audio_idx) {
                    av_packet_unref(pkt);
                    continue;
                }
                if (avcodec_send_packet(cctx, pkt) < 0) {
                    av_packet_unref(pkt);
                    continue;
                }
                av_packet_unref(pkt);
            }
        }

        bool need_more_input = false;
        while (true) {
            int r = avcodec_receive_frame(cctx, frame);
            if (r == AVERROR(EAGAIN)) { need_more_input = true; break; }
            if (r == AVERROR_EOF) { eof_input = true; break; }
            if (r < 0) { need_more_input = true; break; }

            // Lazy-init swr from the first decoded frame so we use the
            // *actual* layout/format/rate rather than the codecpar guess
            // (codecpar can be missing channel layout for some codecs).
            if (!swr_inited) {
                AVChannelLayout out_layout;
                av_channel_layout_default(&out_layout, 1);
                AVChannelLayout in_layout;
                if (frame->ch_layout.nb_channels > 0) {
                    av_channel_layout_copy(&in_layout, &frame->ch_layout);
                } else {
                    int nch = frame->ch_layout.nb_channels > 0
                              ? frame->ch_layout.nb_channels
                              : (cctx->ch_layout.nb_channels > 0
                                 ? cctx->ch_layout.nb_channels : 2);
                    av_channel_layout_default(&in_layout, nch);
                }
                int sr = frame->sample_rate > 0 ? frame->sample_rate
                                                : cctx->sample_rate;
                int sf = frame->format >= 0 ? frame->format : cctx->sample_fmt;
                err = swr_alloc_set_opts2(&swr,
                                          &out_layout, AV_SAMPLE_FMT_S16,
                                          WPCM_SAMPLE_RATE,
                                          &in_layout, sf, sr,
                                          0, NULL);
                av_channel_layout_uninit(&out_layout);
                av_channel_layout_uninit(&in_layout);
                if (err < 0 || !swr || swr_init(swr) < 0) {
                    mp_err(log, "wpcm: swr_init failed\n");
                    av_frame_unref(frame);
                    goto cleanup;
                }
                swr_inited = true;
            }

            // PTS-based clip: skip frames before t0, stop at/after t1.
            int64_t fpts = frame->best_effort_timestamp;
            if (fpts == AV_NOPTS_VALUE) fpts = frame->pts;
            double frame_t = (fpts != AV_NOPTS_VALUE)
                ? fpts * av_q2d(st->time_base) : -1.0;

            if (frame_t >= 0 && frame_t < t0_sec - 0.05) {
                av_frame_unref(frame);
                continue;
            }
            if (frame_t >= 0 && fpts >= t1_pts_in_st) {
                av_frame_unref(frame);
                goto flush;
            }

            int in_sr = frame->sample_rate > 0 ? frame->sample_rate
                                               : cctx->sample_rate;
            int64_t out_n = av_rescale_rnd(
                swr_get_delay(swr, in_sr) + frame->nb_samples,
                WPCM_SAMPLE_RATE, in_sr, AV_ROUND_UP);
            int needed = out_n * WPCM_BYTES_PER_SAMPLE;
            if (needed > buf_out_cap) {
                uint8_t *nb = av_realloc(buf_out, needed);
                if (!nb) { av_frame_unref(frame); goto cleanup; }
                buf_out = nb;
                buf_out_cap = needed;
            }
            uint8_t *out_planes[1] = { buf_out };
            int got = swr_convert(swr, out_planes, out_n,
                                  (const uint8_t **)frame->data,
                                  frame->nb_samples);
            av_frame_unref(frame);
            if (got > 0) {
                size_t bytes = (size_t)got * WPCM_BYTES_PER_SAMPLE;
                if (fwrite(buf_out, 1, bytes, out) != bytes) {
                    mp_err(log, "wpcm: write failed\n");
                    goto cleanup;
                }
                total_samples += got;
            }
        }

        if (eof_input && need_more_input)
            break;
    }

flush:;
    // Drain leftover samples held in swr.
    if (swr_inited) {
        int leftover = swr_get_out_samples(swr, 0);
        if (leftover > 0) {
            int needed = leftover * WPCM_BYTES_PER_SAMPLE;
            if (needed > buf_out_cap) {
                uint8_t *nb = av_realloc(buf_out, needed);
                if (nb) { buf_out = nb; buf_out_cap = needed; }
            }
            if (buf_out) {
                uint8_t *out_planes[1] = { buf_out };
                int got = swr_convert(swr, out_planes, leftover, NULL, 0);
                if (got > 0) {
                    size_t bytes = (size_t)got * WPCM_BYTES_PER_SAMPLE;
                    if (fwrite(buf_out, 1, bytes, out) == bytes)
                        total_samples += got;
                }
            }
        }
    }

    // Patch header sizes.
    uint32_t data_size = total_samples * WPCM_BYTES_PER_SAMPLE;
    if (fseek(out, 0, SEEK_SET) == 0) {
        write_wav_header(out, data_size, WPCM_SAMPLE_RATE);
    }

    rc = (total_samples > 0) ? 0 : -1;

cleanup:
    if (out) fclose(out);
    if (pkt) av_packet_free(&pkt);
    if (frame) av_frame_free(&frame);
    if (swr) swr_free(&swr);
    if (cctx) avcodec_free_context(&cctx);
    if (fmt) avformat_close_input(&fmt);
    av_freep(&buf_out);
    return rc;
}
