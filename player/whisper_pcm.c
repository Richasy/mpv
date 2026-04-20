/*
 * Copyright (C) 2025 mpv project
 *
 * Persistent NETWORK-mode audio extractor session for whisper-lookahead.
 * See whisper_pcm.h for the public contract.
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
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>

#include "common/msg.h"
#include "misc/thread_tools.h"
#include "osdep/io.h"
#include "stream/stream.h"

#include "whisper_pcm.h"

#define WPCM_SAMPLE_RATE 16000
#define WPCM_BYTES_PER_SAMPLE 2  // s16

struct wpcm_session {
    struct mp_log *log;
    struct mpv_global *global;
    struct mp_cancel *cancel;

    AVFormatContext *fmt;
    int audio_idx;
    AVCodecContext *cctx;
    SwrContext *swr;
    bool swr_inited;
    AVRational st_time_base;

    AVPacket *pkt;
    AVFrame *frame;

    // Pending resampled output samples (s16 mono 16k). Carried across
    // extract_to/advance_to calls when a swr_convert produced more than
    // the chunk boundary needed.
    int16_t *pending;
    int pending_capacity;  // in samples
    int pending_count;     // valid samples in pending[]

    // Total output samples consumed by advance_to + extract_to so far.
    // Output cursor in seconds = cursor_samples / WPCM_SAMPLE_RATE.
    int64_t cursor_samples;

    // av_read_frame returned EOF.
    bool input_eof;
    // Decoder + swr have been fully drained after input_eof.
    bool drained;
};

// ---------- WAV header --------------------------------------------------

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

// ---------- AVIO interrupt callback -------------------------------------

static int wpcm_interrupt_cb(void *opaque)
{
    struct mp_cancel *c = opaque;
    return c && mp_cancel_test(c) ? 1 : 0;
}

// ---------- pending[] helpers -------------------------------------------

static int pending_reserve(struct wpcm_session *s, int extra_samples)
{
    int need = s->pending_count + extra_samples;
    if (need <= s->pending_capacity)
        return 0;
    int cap = s->pending_capacity ? s->pending_capacity : 4096;
    while (cap < need) cap *= 2;
    int16_t *nb = av_realloc(s->pending, (size_t)cap * sizeof(int16_t));
    if (!nb) return -1;
    s->pending = nb;
    s->pending_capacity = cap;
    return 0;
}

static int pending_consume(struct wpcm_session *s, int n,
                           FILE *out_or_null)
{
    if (n <= 0) return 0;
    if (n > s->pending_count) n = s->pending_count;
    if (out_or_null) {
        size_t bytes = (size_t)n * WPCM_BYTES_PER_SAMPLE;
        if (fwrite(s->pending, 1, bytes, out_or_null) != bytes)
            return -1;
    }
    if (n < s->pending_count) {
        memmove(s->pending, s->pending + n,
                (size_t)(s->pending_count - n) * sizeof(int16_t));
    }
    s->pending_count -= n;
    s->cursor_samples += n;
    return n;
}

// ---------- swr lazy init ----------------------------------------------

static int swr_lazy_init(struct wpcm_session *s, AVFrame *frame)
{
    if (s->swr_inited)
        return 0;

    AVChannelLayout out_layout, in_layout;
    av_channel_layout_default(&out_layout, 1);

    if (frame->ch_layout.nb_channels > 0 &&
        frame->ch_layout.order != AV_CHANNEL_ORDER_UNSPEC)
    {
        av_channel_layout_copy(&in_layout, &frame->ch_layout);
    } else {
        int nch = frame->ch_layout.nb_channels > 0
                  ? frame->ch_layout.nb_channels
                  : (s->cctx->ch_layout.nb_channels > 0
                     ? s->cctx->ch_layout.nb_channels : 2);
        av_channel_layout_default(&in_layout, nch);
    }
    int sr = frame->sample_rate > 0 ? frame->sample_rate : s->cctx->sample_rate;
    int sf = frame->format >= 0 ? frame->format : s->cctx->sample_fmt;

    int err = swr_alloc_set_opts2(&s->swr,
                                  &out_layout, AV_SAMPLE_FMT_S16,
                                  WPCM_SAMPLE_RATE,
                                  &in_layout, sf, sr,
                                  0, NULL);
    av_channel_layout_uninit(&out_layout);
    av_channel_layout_uninit(&in_layout);
    if (err < 0 || !s->swr || swr_init(s->swr) < 0) {
        mp_err(s->log, "wpcm: swr_init failed\n");
        return -1;
    }
    s->swr_inited = true;
    return 0;
}

// Resample one decoded frame and append to pending[].
static int append_frame_resampled(struct wpcm_session *s, AVFrame *frame)
{
    if (swr_lazy_init(s, frame) < 0)
        return -1;

    int in_sr = frame->sample_rate > 0 ? frame->sample_rate
                                       : s->cctx->sample_rate;
    int64_t out_n = av_rescale_rnd(
        swr_get_delay(s->swr, in_sr) + frame->nb_samples,
        WPCM_SAMPLE_RATE, in_sr, AV_ROUND_UP);
    if (out_n <= 0) return 0;
    if (pending_reserve(s, (int)out_n) < 0) return -1;
    uint8_t *out_planes[1] = { (uint8_t *)(s->pending + s->pending_count) };
    int got = swr_convert(s->swr, out_planes, (int)out_n,
                          (const uint8_t **)frame->data, frame->nb_samples);
    if (got > 0)
        s->pending_count += got;
    return 0;
}

// Drain any samples held inside swr after EOS. Idempotent-ish: returns 0
// once swr is empty.
static int drain_swr(struct wpcm_session *s)
{
    if (!s->swr_inited || !s->swr)
        return 0;
    int leftover = swr_get_out_samples(s->swr, 0);
    if (leftover <= 0)
        return 0;
    if (pending_reserve(s, leftover) < 0)
        return -1;
    uint8_t *out_planes[1] = { (uint8_t *)(s->pending + s->pending_count) };
    int got = swr_convert(s->swr, out_planes, leftover, NULL, 0);
    if (got > 0)
        s->pending_count += got;
    return 0;
}

// ---------- main pump ---------------------------------------------------

// Pump packets/frames once. May append zero or more samples to pending[].
// Returns:
//    0 = made progress (or no-op), keep calling
//   -1 = hard error
//   -2 = cancelled
// Sets s->drained when no more samples will ever be produced.
static int pump_once(struct wpcm_session *s)
{
    if (s->drained)
        return 0;
    if (s->cancel && mp_cancel_test(s->cancel))
        return -2;

    if (!s->input_eof) {
        // Read one audio packet (skip non-audio).
        while (true) {
            int err = av_read_frame(s->fmt, s->pkt);
            if (err == AVERROR_EOF) {
                s->input_eof = true;
                avcodec_send_packet(s->cctx, NULL);
                break;
            }
            if (err == AVERROR_EXIT)
                return -2;
            if (err < 0) {
                // Transient or hard error: treat as EOF and start draining.
                mp_warn(s->log, "wpcm: av_read_frame error %d; ending stream\n",
                        err);
                s->input_eof = true;
                avcodec_send_packet(s->cctx, NULL);
                break;
            }
            if (s->pkt->stream_index != s->audio_idx) {
                av_packet_unref(s->pkt);
                continue;
            }
            int sr = avcodec_send_packet(s->cctx, s->pkt);
            av_packet_unref(s->pkt);
            if (sr < 0 && sr != AVERROR(EAGAIN)) {
                // Decoder unhappy with this packet; skip and continue.
                continue;
            }
            break;
        }
    }

    // Drain whatever the decoder will give us right now.
    while (true) {
        int r = avcodec_receive_frame(s->cctx, s->frame);
        if (r == AVERROR(EAGAIN))
            break;
        if (r == AVERROR_EOF) {
            // Decoder fully drained. Drain swr too.
            drain_swr(s);
            s->drained = true;
            break;
        }
        if (r < 0) {
            // Treat as transient; try next iteration.
            break;
        }
        if (append_frame_resampled(s, s->frame) < 0) {
            av_frame_unref(s->frame);
            return -1;
        }
        av_frame_unref(s->frame);
    }

    // If we already saw input EOF and pump_once produced no new frames
    // beyond drained==true, stop.
    return 0;
}

// Pump until pending_count >= want OR session drained OR error/cancel.
static int pump_until(struct wpcm_session *s, int want)
{
    while (s->pending_count < want && !s->drained) {
        int rc = pump_once(s);
        if (rc < 0) return rc;
    }
    return 0;
}

// ---------- public API: open --------------------------------------------

static struct wpcm_session *try_open_internal(struct mp_log *log,
                                              struct mpv_global *global,
                                              struct mp_cancel *cancel,
                                              const char *url,
                                              int preferred_audio_idx,
                                              bool seekable_off)
{
    struct wpcm_session *s = NULL;
    AVFormatContext *fmt = NULL;
    AVDictionary *opts = NULL;

    if (cancel && mp_cancel_test(cancel))
        return NULL;

    fmt = avformat_alloc_context();
    if (!fmt) return NULL;
    fmt->interrupt_callback.callback = wpcm_interrupt_cb;
    fmt->interrupt_callback.opaque = cancel;

    mp_setup_av_network_options(&opts, NULL, global, log);
    av_dict_set(&opts, "reconnect", "1", 0);
    av_dict_set(&opts, "reconnect_streamed", "1", 0);
    av_dict_set(&opts, "reconnect_delay_max", "7", 0);
    if (seekable_off)
        av_dict_set(&opts, "seekable", "0", 0);

    int err = avformat_open_input(&fmt, url, NULL, &opts);
    av_dict_free(&opts);
    if (err < 0) {
        char ebuf[128] = {0};
        av_strerror(err, ebuf, sizeof(ebuf));
        mp_warn(log, "wpcm: avformat_open_input (%s) failed: %s (%d)\n",
                seekable_off ? "seekable=0" : "default", ebuf, err);
        return NULL;
    }

    if (cancel && mp_cancel_test(cancel)) {
        avformat_close_input(&fmt);
        return NULL;
    }

    err = avformat_find_stream_info(fmt, NULL);
    if (err < 0) {
        mp_warn(log, "wpcm: find_stream_info (%s) failed: %d\n",
                seekable_off ? "seekable=0" : "default", err);
        avformat_close_input(&fmt);
        return NULL;
    }

    int audio_idx = -1;
    if (preferred_audio_idx >= 0 &&
        preferred_audio_idx < (int)fmt->nb_streams &&
        fmt->streams[preferred_audio_idx]->codecpar->codec_type
            == AVMEDIA_TYPE_AUDIO)
    {
        audio_idx = preferred_audio_idx;
    } else {
        audio_idx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO,
                                        -1, -1, NULL, 0);
    }
    if (audio_idx < 0) {
        mp_err(log, "wpcm: no audio stream found\n");
        avformat_close_input(&fmt);
        return NULL;
    }

    AVStream *st = fmt->streams[audio_idx];
    const AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) {
        mp_err(log, "wpcm: no decoder for codec %d\n", st->codecpar->codec_id);
        avformat_close_input(&fmt);
        return NULL;
    }

    AVCodecContext *cctx = avcodec_alloc_context3(dec);
    if (!cctx) {
        avformat_close_input(&fmt);
        return NULL;
    }
    if (avcodec_parameters_to_context(cctx, st->codecpar) < 0) {
        avcodec_free_context(&cctx);
        avformat_close_input(&fmt);
        return NULL;
    }
    cctx->pkt_timebase = st->time_base;
    if (avcodec_open2(cctx, dec, NULL) < 0) {
        mp_err(log, "wpcm: avcodec_open2 failed\n");
        avcodec_free_context(&cctx);
        avformat_close_input(&fmt);
        return NULL;
    }

    s = calloc(1, sizeof(*s));
    if (!s) {
        avcodec_free_context(&cctx);
        avformat_close_input(&fmt);
        return NULL;
    }
    s->log = log;
    s->global = global;
    s->cancel = cancel;
    s->fmt = fmt;
    s->audio_idx = audio_idx;
    s->cctx = cctx;
    s->st_time_base = st->time_base;
    s->pkt = av_packet_alloc();
    s->frame = av_frame_alloc();
    if (!s->pkt || !s->frame) {
        wpcm_session_close(s);
        return NULL;
    }
    return s;
}

struct wpcm_session *wpcm_session_open(struct mp_log *log,
                                       struct mpv_global *global,
                                       struct mp_cancel *cancel,
                                       const char *url,
                                       int preferred_audio_idx)
{
    // We always use seekable=0 for network sources. Some servers (alist
    // /dav, signed-URL CDNs) accept the initial GET but 403 on the Range
    // requests the demuxer issues during probe; those failures don't make
    // open/find_stream_info return an error (they just print warnings),
    // so the session ends up with a dead connection that EOFs immediately.
    // Forcing seekable=0 from the start avoids the Range probes entirely.
    // We re-open per backward seek anyway, so byte-Range seeking inside
    // the session would buy little.
    struct wpcm_session *s = try_open_internal(log, global, cancel, url,
                                               preferred_audio_idx, true);
    if (s) {
        mp_info(log, "wpcm: session opened (audio_idx=%d, seekable=0)\n",
                s->audio_idx);
    }
    return s;
}

// ---------- public API: cursor/eof --------------------------------------

double wpcm_session_cursor_sec(struct wpcm_session *s)
{
    if (!s) return 0.0;
    return (double)s->cursor_samples / (double)WPCM_SAMPLE_RATE;
}

bool wpcm_session_eof(struct wpcm_session *s)
{
    return s && s->drained && s->pending_count == 0;
}

// ---------- public API: advance/extract ---------------------------------

int wpcm_session_advance_to(struct wpcm_session *s, double t0_sec)
{
    if (!s) return -1;
    if (t0_sec < 0) t0_sec = 0;
    int64_t target = (int64_t)(t0_sec * WPCM_SAMPLE_RATE);
    if (target <= s->cursor_samples)
        return 0;

    while (s->cursor_samples < target) {
        if (s->cancel && mp_cancel_test(s->cancel))
            return -2;
        if (s->pending_count > 0) {
            int64_t deficit = target - s->cursor_samples;
            int n = (int)((deficit > s->pending_count)
                          ? s->pending_count : deficit);
            if (pending_consume(s, n, NULL) < 0)
                return -1;
            continue;
        }
        if (s->drained)
            break;
        // Pump enough so the next iteration will have samples to consume.
        // Pump 1024 samples per round; tiny tradeoff.
        int rc = pump_until(s, 1024);
        if (rc < 0) return rc;
    }
    return 0;
}

int wpcm_session_extract_to(struct wpcm_session *s,
                            double t1_sec,
                            const char *out_wav_path)
{
    if (!s) return -1;

    int64_t target = (int64_t)(t1_sec * WPCM_SAMPLE_RATE);
    if (target <= s->cursor_samples) {
        // Nothing to extract for this chunk window. Still produce an
        // empty WAV so callers can treat success uniformly.
        FILE *f = fopen(out_wav_path, "wb");
        if (!f) return -1;
        write_wav_header(f, 0, WPCM_SAMPLE_RATE);
        fclose(f);
        return 0;
    }

    FILE *out = fopen(out_wav_path, "wb");
    if (!out) {
        mp_err(s->log, "wpcm: cannot open output '%s'\n", out_wav_path);
        return -1;
    }
    if (write_wav_header(out, 0, WPCM_SAMPLE_RATE) < 0) {
        fclose(out);
        return -1;
    }

    int64_t start_cursor = s->cursor_samples;
    int rc = 0;

    while (s->cursor_samples < target) {
        if (s->cancel && mp_cancel_test(s->cancel)) { rc = -2; break; }

        if (s->pending_count > 0) {
            int64_t deficit = target - s->cursor_samples;
            int n = (int)((deficit > s->pending_count)
                          ? s->pending_count : deficit);
            int wrote = pending_consume(s, n, out);
            if (wrote < 0) { rc = -1; break; }
            continue;
        }
        if (s->drained) break;
        int pr = pump_until(s, 1024);
        if (pr < 0) { rc = pr; break; }
    }

    int64_t produced = s->cursor_samples - start_cursor;
    uint32_t data_size = (uint32_t)(produced * WPCM_BYTES_PER_SAMPLE);
    if (fseek(out, 0, SEEK_SET) == 0)
        write_wav_header(out, data_size, WPCM_SAMPLE_RATE);
    fclose(out);

    if (rc < 0) return rc;
    return (produced > 0 || s->drained) ? 0 : -1;
}

// ---------- public API: close -------------------------------------------

void wpcm_session_close(struct wpcm_session *s)
{
    if (!s) return;
    if (s->pkt) av_packet_free(&s->pkt);
    if (s->frame) av_frame_free(&s->frame);
    if (s->swr) swr_free(&s->swr);
    if (s->cctx) avcodec_free_context(&s->cctx);
    if (s->fmt) avformat_close_input(&s->fmt);
    av_freep(&s->pending);
    free(s);
}

// ---------- backwards-compat single-shot wrapper ------------------------

int wpcm_extract_chunk_to_wav(struct mp_log *log,
                              struct mpv_global *global,
                              struct mp_cancel *cancel,
                              const char *url,
                              double t0_sec,
                              double t1_sec,
                              const char *out_wav_path)
{
    struct wpcm_session *s = wpcm_session_open(log, global, cancel, url, -1);
    if (!s)
        return cancel && mp_cancel_test(cancel) ? -2 : -1;
    int rc = wpcm_session_advance_to(s, t0_sec);
    if (rc == 0)
        rc = wpcm_session_extract_to(s, t1_sec, out_wav_path);
    wpcm_session_close(s);
    return rc;
}
