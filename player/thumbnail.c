/*
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

/*
 * Local previews use an independent demuxer. Network previews only snapshot
 * packets from the playback cache: never open another connection or session.
 * Both routes retain a private software decoder and run outside the core lock.
 * The worker mutex protects concurrent commands; file unload only advances the
 * generation, since an outstanding command may still own the decoder.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>

#include "mpv_talloc.h"
#include "common/av_common.h"
#include "common/common.h"
#include "common/msg.h"
#include "demux/demux.h"
#include "demux/packet.h"
#include "demux/stheader.h"
#include "input/cmd.h"
#include "misc/node.h"
#include "misc/thread_tools.h"
#include "stream/stream.h"
#include "video/img_format.h"
#include "video/mp_image.h"
#include "video/sws_utils.h"
#include "command.h"
#include "core.h"
#include "thumbnail.h"

#define THUMB_MAX_PACKETS 900
#define THUMB_FORWARD_WINDOW 0.5

struct thumb_ctx {
    mp_mutex lock;
    uint64_t generation;
    int track_id;
    bool cache;
    char *url;
    struct mp_cancel *cancel;
    struct demuxer *demuxer;
    struct sh_stream *vsh;
    AVCodecContext *avctx;
    AVCodecParameters *parameters;
    AVRational tb;
};

struct thumb_decode {
    void *ta_ctx;
    AVCodecContext *avctx;
    AVRational tb;
    AVPacket *packet;
    AVFrame *frame;
    struct mp_cancel *cancel;
    double target;
    bool keyframes;
    bool done;
    struct mp_image *best;
    double best_pts;
    double best_diff;
    int packets;
    int frames;
};

struct thumb_collect {
    void *ta_ctx;
    struct mp_cancel *cancel;
    struct demux_packet **pkts;
    int num_pkts;
};

static void thumb_ctx_close(struct thumb_ctx *tc)
{
    avcodec_free_context(&tc->avctx);
    avcodec_parameters_free(&tc->parameters);
    if (tc->demuxer) {
        demux_cancel_and_free(tc->demuxer);
        tc->demuxer = NULL;
    }
    TA_FREEP(&tc->cancel);
    TA_FREEP(&tc->url);
    tc->vsh = NULL;
}

static void thumb_ctx_destroy(void *p)
{
    struct thumb_ctx *tc = p;
    thumb_ctx_close(tc);
    mp_mutex_destroy(&tc->lock);
}

static bool thumb_parameters_equal(const AVCodecParameters *a,
                                   const AVCodecParameters *b)
{
    return a && b && a->codec_id == b->codec_id &&
        a->codec_tag == b->codec_tag && a->format == b->format &&
        a->width == b->width && a->height == b->height &&
        a->profile == b->profile && a->level == b->level &&
        a->color_range == b->color_range && a->color_space == b->color_space &&
        a->color_primaries == b->color_primaries && a->color_trc == b->color_trc &&
        a->sample_aspect_ratio.num == b->sample_aspect_ratio.num &&
        a->sample_aspect_ratio.den == b->sample_aspect_ratio.den &&
        a->extradata_size == b->extradata_size &&
        (!a->extradata_size ||
         memcmp(a->extradata, b->extradata, a->extradata_size) == 0);
}

static bool thumb_prepare_decoder(struct thumb_ctx *tc, struct mp_log *log,
                                   const AVCodecParameters *parameters,
                                   AVRational tb, bool *reused)
{
    *reused = tc->avctx && thumb_parameters_equal(tc->parameters, parameters) &&
              av_cmp_q(tc->tb, tb) == 0;
    if (*reused) {
        avcodec_flush_buffers(tc->avctx);
        return true;
    }

    avcodec_free_context(&tc->avctx);
    avcodec_parameters_free(&tc->parameters);
    const AVCodec *codec = avcodec_find_decoder(parameters->codec_id);
    if (!codec)
        return false;

    tc->avctx = avcodec_alloc_context3(codec);
    tc->parameters = avcodec_parameters_alloc();
    if (!tc->avctx || !tc->parameters ||
        avcodec_parameters_copy(tc->parameters, parameters) < 0 ||
        avcodec_parameters_to_context(tc->avctx, parameters) < 0)
        goto fail;

    tc->tb = tb;
    tc->avctx->pkt_timebase = tb;
    tc->avctx->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;
    tc->avctx->err_recognition = 0;
    tc->avctx->skip_loop_filter = AVDISCARD_ALL;
    tc->avctx->flags2 |= AV_CODEC_FLAG2_FAST;
    // Preserve the established single-threaded decoder and full IDCT policy.
    tc->avctx->thread_count = 1;
    tc->avctx->thread_type = 0;
    if (avcodec_open2(tc->avctx, codec, NULL) < 0)
        goto fail;
    return true;

fail:
    mp_err(log, "thumbnail: decoder initialization failed\n");
    avcodec_free_context(&tc->avctx);
    avcodec_parameters_free(&tc->parameters);
    return false;
}

static bool thumb_open_local(struct thumb_ctx *tc, struct mpv_global *global,
                             struct mp_log *log, const char *url,
                             int stream_index, struct mp_cancel *cancel)
{
    tc->cancel = mp_cancel_new(tc);
    mp_cancel_set_parent(tc->cancel, cancel);
    struct demuxer_params params = {
        .is_top_level = true,
        .allow_playlist_create = false,
        .stream_flags = STREAM_ORIGIN_DIRECT,
    };
    tc->demuxer = demux_open_url(url, &params, tc->cancel, global);
    if (!tc->demuxer)
        goto fail;

    tc->vsh = demux_get_stream(tc->demuxer, stream_index);
    if (!tc->vsh || tc->vsh->type != STREAM_VIDEO || tc->vsh->image)
        goto fail;
    demuxer_select_track(tc->demuxer, tc->vsh, MP_NOPTS_VALUE, true);
    tc->url = talloc_strdup(tc, url);
    return true;

fail:
    mp_verbose(log, "thumbnail: secondary video stream unavailable\n");
    thumb_ctx_close(tc);
    return false;
}

static void thumb_receive(struct thumb_decode *d)
{
    while (!d->done && !mp_cancel_test(d->cancel)) {
        int err = avcodec_receive_frame(d->avctx, d->frame);
        if (err < 0) {
            if (err == AVERROR_EOF)
                d->done = true;
            return;
        }
        d->frames++;
        int64_t apts = d->frame->best_effort_timestamp != AV_NOPTS_VALUE
                     ? d->frame->best_effort_timestamp : d->frame->pts;
        double pts = mp_pts_from_av(apts, &d->tb);
        bool valid_pts = pts != MP_NOPTS_VALUE && isfinite(pts);
        double diff = valid_pts ? fabs(pts - d->target) : INFINITY;
        if (!d->best || diff < d->best_diff) {
            struct mp_image *image = mp_image_from_av_frame(d->frame);
            if (image) {
                talloc_free(d->best);
                d->best = talloc_steal(d->ta_ctx, image);
                d->best_pts = pts;
                d->best_diff = diff;
            }
        }
        av_frame_unref(d->frame);
        if (d->best && (d->keyframes || (valid_pts && pts >= d->target)))
            d->done = true;
    }
}

static void thumb_send(struct thumb_decode *d, struct demux_packet *packet)
{
    if (d->done || mp_cancel_test(d->cancel))
        return;
    mp_set_av_packet(d->packet, packet, packet ? &d->tb : NULL);
    int err = avcodec_send_packet(d->avctx, packet ? d->packet : NULL);
    if (err == AVERROR(EAGAIN)) {
        thumb_receive(d);
        if (!d->done && !mp_cancel_test(d->cancel))
            err = avcodec_send_packet(d->avctx, packet ? d->packet : NULL);
    }
    // mp_set_av_packet borrows the demux packet, so never unref that borrow.
    mp_set_av_packet(d->packet, NULL, NULL);
    if (err >= 0) {
        d->packets += !!packet;
        thumb_receive(d);
    }
}

static void thumb_decode_local(struct thumb_ctx *tc, struct thumb_decode *d)
{
    if (!demux_seek(tc->demuxer, d->target, 0))
        return;
    for (int n = 0; n < THUMB_MAX_PACKETS &&
                    !d->done && !mp_cancel_test(d->cancel); n++) {
        struct demux_packet *packet = NULL;
        demux_read_packet_async(tc->vsh, &packet);
        if (!packet)
            break;
        thumb_send(d, packet);
        talloc_free(packet);
    }
    thumb_send(d, NULL);
}

static void thumb_collect_cb(void *ctx, struct demux_packet *packet)
{
    struct thumb_collect *c = ctx;
    if (mp_cancel_test(c->cancel)) {
        talloc_free(packet);
        return;
    }
    talloc_steal(c->ta_ctx, packet);
    MP_TARRAY_APPEND(c->ta_ctx, c->pkts, c->num_pkts, packet);
}

static struct mp_image *thumb_scale_bgra(struct mp_log *log,
                                        struct mp_image *src, int max_width)
{
    int dw, dh;
    mp_image_params_get_dsize(&src->params, &dw, &dh);
    if (dw < 1 || dh < 1) {
        dw = src->w;
        dh = src->h;
    }
    int tw = MPMIN(MPMAX(max_width, 16), dw);
    int th = (int)lround((double)tw * dh / dw);
    tw = MPMAX(tw & ~1, 2);
    th = MPMAX(th & ~1, 2);
    struct mp_image_params params = {
        .imgfmt = IMGFMT_BGRA,
        .w = tw,
        .h = th,
        .p_w = 1,
        .p_h = 1,
    };
    mp_image_params_guess_csp(&params);
    struct mp_image *dst = mp_image_alloc(params.imgfmt, params.w, params.h);
    if (!dst)
        return NULL;
    mp_image_copy_attributes(dst, src);
    dst->params = params;
    struct mp_sws_context *sws = mp_sws_alloc(NULL);
    sws->log = log;
    bool ok = mp_sws_scale(sws, dst, src) >= 0;
    talloc_free(sws);
    if (!ok) {
        mp_err(log, "thumbnail: scale failed\n");
        talloc_free(dst);
        return NULL;
    }
    return dst;
}

void cmd_thumbnail_raw(void *p)
{
    struct mp_cmd_ctx *cmd = p;
    struct MPContext *mpctx = cmd->mpctx;
    struct mp_cancel *cancel = cmd->abort->cancel;
    struct mpv_node *res = &cmd->result;
    cmd->success = false;

    double target = cmd->args[0].v.d;
    int max_width = cmd->args[1].v.i;
    int mode = cmd->args[2].v.i;
    bool keyframes = cmd->args[3].v.i == 1;
    if (!isfinite(target))
        return;
    target = MPMAX(target, 0);
    if (max_width <= 0)
        max_width = 320;

    void *tmp = talloc_new(NULL);
    struct mp_log *log = mpctx->log;
    struct mpv_global *global = mpctx->global;
    struct mp_image *out = NULL;
    AVCodecParameters *parameters = NULL;
    struct thumb_decode decode = {
        .ta_ctx = tmp, .cancel = cancel, .target = target,
        .keyframes = keyframes, .best_pts = MP_NOPTS_VALUE,
        .best_diff = INFINITY,
    };
    bool reused = false;
    struct track *track = mpctx->current_track[0][STREAM_VIDEO];
    if (!track || !track->stream || !track->demuxer || mp_cancel_test(cancel))
        goto done;

    uint64_t generation = mpctx->thumbnail_generation;
    int track_id = track->user_tid;
    int stream_index = track->stream->index;
    bool cache = mode == 2 || (mode == 0 && track->demuxer->is_network);
    char *url = cache ? NULL : talloc_strdup(tmp, track->demuxer->filename);
    struct thumb_collect collect = { .ta_ctx = tmp, .cancel = cancel };
    if (cache) {
        demux_cache_visit_packets_limited(track->demuxer, track->stream, target,
                                         target + THUMB_FORWARD_WINDOW,
                                         NULL, NULL, thumb_collect_cb, &collect,
                                         THUMB_MAX_PACKETS, cancel);
        if (!collect.num_pkts)
            goto done;
    } else if (!url) {
        goto done;
    }
    parameters = mp_codec_params_to_av(track->stream->codec);
    if (!parameters)
        goto done;
    AVRational tb = mp_get_codec_timebase(track->stream->codec);

    struct thumb_ctx *tc = mpctx->thumbnail;
    if (!tc) {
        tc = talloc_zero(mpctx, struct thumb_ctx);
        mp_mutex_init(&tc->lock);
        talloc_set_destructor(tc, thumb_ctx_destroy);
        mpctx->thumbnail = tc;
    }

    // Never wait for the preview worker while holding the playback core lock.
    mp_core_unlock(mpctx);
    mp_mutex_lock(&tc->lock);
    if (mp_cancel_test(cancel))
        goto unlock;
    if (tc->generation != generation || tc->track_id != track_id ||
        tc->cache != cache || (!cache && tc->url && strcmp(tc->url, url))) {
        thumb_ctx_close(tc);
    }
    tc->generation = generation;
    tc->track_id = track_id;
    tc->cache = cache;
    if (!cache) {
        if (!tc->demuxer &&
            !thumb_open_local(tc, global, log, url, stream_index, cancel))
            goto unlock;
        mp_cancel_set_parent(tc->cancel, cancel);
    }
    if (!thumb_prepare_decoder(tc, log, parameters, tb, &reused))
        goto unlock;

    decode.avctx = tc->avctx;
    decode.tb = tb;
    decode.packet = av_packet_alloc();
    decode.frame = av_frame_alloc();
    if (!decode.packet || !decode.frame)
        goto unlock;
    mp_verbose(log, "thumbnail: decoding %s\n", keyframes ? "keyframes" : "exact");
    if (cache) {
        for (int n = 0; n < collect.num_pkts &&
                        !decode.done && !mp_cancel_test(cancel); n++)
            thumb_send(&decode, collect.pkts[n]);
        thumb_send(&decode, NULL);
    } else {
        thumb_decode_local(tc, &decode);
    }
    if (decode.best && !mp_cancel_test(cancel))
        out = thumb_scale_bgra(log, decode.best, max_width);

unlock:
    if (tc->cancel) {
        mp_cancel_set_parent(tc->cancel, NULL);
        // A cancelled demuxer retains interrupted stream state; reopen it.
        if (mp_cancel_test(tc->cancel))
            thumb_ctx_close(tc);
    }
    mp_mutex_unlock(&tc->lock);
    mp_core_lock(mpctx);
    track = mpctx->current_track[0][STREAM_VIDEO];
    if (mp_cancel_test(cancel) || generation != mpctx->thumbnail_generation ||
        !track || track->user_tid != track_id) {
        talloc_free(out);
        out = NULL;
    }

done:
    av_frame_free(&decode.frame);
    av_packet_free(&decode.packet);
    avcodec_parameters_free(&parameters);
    if (!mp_cancel_test(cancel)) {
        node_init(res, MPV_FORMAT_NODE_MAP, NULL);
        node_map_add_flag(res, "cached", out != NULL);
        if (out) {
            node_map_add_int64(res, "w", out->w);
            node_map_add_int64(res, "h", out->h);
            node_map_add_int64(res, "stride", out->stride[0]);
            node_map_add_string(res, "format", "bgra");
            node_map_add_string(res, "precision", keyframes ? "keyframes" : "exact");
            if (decode.best_pts != MP_NOPTS_VALUE && isfinite(decode.best_pts))
                node_map_add_double(res, "pts", decode.best_pts);
            node_map_add_int64(res, "decoded-packets", decode.packets);
            node_map_add_int64(res, "decoded-frames", decode.frames);
            node_map_add_flag(res, "decoder-reused", reused);
            struct mpv_byte_array *ba =
                node_map_add(res, "data", MPV_FORMAT_BYTE_ARRAY)->u.ba;
            *ba = (struct mpv_byte_array){
                .data = out->planes[0],
                .size = (size_t)out->stride[0] * out->h,
            };
            talloc_steal(ba, out);
            out = NULL;
        }
        cmd->success = true;
    }
    talloc_free(out);
    talloc_free(tmp);
}
