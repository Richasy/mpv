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
 * Seek-preview thumbnails, zero-disk:
 *
 * `thumbnail-raw <time> [<max-width>] [<mode>]` decodes a single video frame
 * near the requested timestamp and hands it back as a raw BGRA node map —
 * exactly like `screenshot-raw`, but for an arbitrary timestamp and without
 * disturbing playback. Out-of-range/undecodable positions report cached=false
 * (a normal, non-error outcome) so callers can cheaply show a placeholder.
 *
 * Two routes, selected by the <mode> argument (the caller knows the exact
 * source kind; "auto" falls back to mpv's own demuxer->is_network heuristic):
 *
 *   - LOCAL files (mode "local", or "auto" + is_network==false): a *persistent,
 *     independent secondary demuxer* is opened on the same file, seeked to the
 *     keyframe at/just-before the target, decoded forward to the closest frame
 *     and scaled to BGRA. Because it owns all of its own packets it can preview
 *     the *entire timeline* — including content already played and no longer
 *     cached — and never underflows the playback demuxer's refcounted packets.
 *     Opening a second handle on a local file is free (no server, no session).
 *
 *   - NETWORK sources (mode "cache", or "auto" + is_network==true): emphatically
 *     must NOT open a second connection. Streaming services (Emby / Jellyfin /
 *     Plex) count and cap concurrent streams per device and may spin up a second
 *     transcode for a second session, so a secondary demuxer would be hostile to
 *     the server and can get the user throttled or banned. Instead we preview
 *     strictly from the *playback demuxer's already-buffered cache*
 *     (demux_cache_visit_packets): zero extra connection, zero extra session.
 *     Positions outside the buffered range simply report cached=false.
 *
 *   demux_open_url(url) / cache packets --> keyframe-near-target packets
 *        --> private AVCodecContext --> AVFrame (closest pts)
 *        --> mp_image_from_av_frame --> mp_sws_scale -> BGRA
 *        --> node map {w,h,stride,format,data}
 *
 * Lifecycle / threading:
 *   - Registered with .spawn_thread, so the handler runs on a worker thread and
 *     enters with the core locked. The secondary demuxer state lives in
 *     mpctx->thumbnail (a talloc child of the MPContext with a destructor) and
 *     is only ever touched by this handler. Callers (the UI) serialize
 *     thumbnail-raw requests, so the state needs no extra locking.
 *   - The state is allocated under the core lock (talloc on the shared MPContext
 *     context is not thread-safe), but all the heavy work — opening/seeking the
 *     secondary demuxer, decoding and downscaling — runs with the core UNLOCKED.
 *     Holding the core lock across that would block the playloop, the render
 *     context and synchronous client API calls (mpv_get_property) and deadlock
 *     on a hot hover path. The secondary demuxer has its own internal lock and
 *     is independent of the core lock.
 *   - The state is never freed on file unload (uninit_demuxer does not wait for
 *     outstanding async commands, so freeing it there could race a decode in
 *     flight). Instead it persists until the MPContext is destroyed (by which
 *     point all async commands have drained), and is lazily reopened by the
 *     worker itself when the playback URL changes.
 *   - The private decoder is software-only, so frames come back in a normal sw
 *     pixel format that sws can convert to BGRA.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>

#include "mpv_talloc.h"

#include "common/common.h"
#include "common/msg.h"
#include "common/av_common.h"
#include "demux/demux.h"
#include "demux/packet.h"
#include "stream/stream.h"
#include "demux/stheader.h"
#include "misc/node.h"
#include "misc/thread_tools.h"
#include "video/mp_image.h"
#include "video/img_format.h"
#include "video/sws_utils.h"
#include "input/cmd.h"
#include "command.h"
#include "core.h"
#include "thumbnail.h"

// Hard cap on packets fed to the decoder for a single preview, guarding against
// pathologically long GOPs / broken streams. A keyframe seek lands us on the
// keyframe <= target, so one GOP is normally far below this.
#define THUMB_MAX_PACKETS 900

// Persistent secondary demuxer + decoder for seek-preview thumbnails. Lives as
// a talloc child of the MPContext (see core.h: mpctx->thumbnail) and is only
// touched by the thumbnail-raw worker thread.
struct thumb_ctx {
    char *url;                  // source URL the demuxer was opened for
    struct mp_cancel *cancel;   // abort handle for demux I/O (talloc child)
    struct demuxer *demuxer;    // independent demuxer, NO demux thread
    struct sh_stream *vsh;      // selected video stream
    AVCodecContext *avctx;      // persistent software decoder
    AVRational tb;              // video stream timebase
};

// Tear down the open demuxer/decoder of `tc` but keep the (talloc) struct, so
// it can be reopened for a different URL. Safe to call with everything NULL.
static void thumb_ctx_close(struct thumb_ctx *tc)
{
    if (tc->avctx)
        avcodec_free_context(&tc->avctx);
    if (tc->demuxer) {
        demux_cancel_and_free(tc->demuxer);
        tc->demuxer = NULL;
    }
    if (tc->cancel) {
        talloc_free(tc->cancel);
        tc->cancel = NULL;
    }
    tc->vsh = NULL;
    talloc_free(tc->url);
    tc->url = NULL;
}

static void thumb_ctx_destroy(void *p)
{
    thumb_ctx_close(p);
}

// Build a fresh software decoder from `c` (an mp_codec_params snapshot).
static AVCodecContext *thumb_build_decoder(struct mp_log *log,
                                           const struct mp_codec_params *c,
                                           AVRational *out_tb)
{
    if (!c || !c->codec)
        return NULL;

    AVCodecParameters *par = mp_codec_params_to_av(c);
    if (!par)
        return NULL;

    const AVCodec *codec = avcodec_find_decoder(par->codec_id);
    if (!codec) {
        mp_err(log, "thumbnail: no decoder for codec id %d\n", par->codec_id);
        avcodec_parameters_free(&par);
        return NULL;
    }

    AVCodecContext *avctx = avcodec_alloc_context3(codec);
    if (!avctx) {
        avcodec_parameters_free(&par);
        return NULL;
    }

    if (avcodec_parameters_to_context(avctx, par) < 0) {
        avcodec_free_context(&avctx);
        avcodec_parameters_free(&par);
        return NULL;
    }

    *out_tb = mp_get_codec_timebase(c);
    avctx->pkt_timebase = *out_tb;
    // Be lenient: emit whatever we can even from imperfect mid-stream input and
    // don't run aggressive error checking that only adds error-concealment work.
    avctx->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;
    avctx->err_recognition = 0;

    // Speed over fidelity: a downscaled preview frame does not need deblocking
    // or bit-exact IDCT. Skipping the loop filter and enabling libavcodec's
    // "fast" (inaccurate) decode tricks roughly halves software decode time on
    // HEVC/H.264. Do NOT skip the IDCT entirely (AVDISCARD_ALL) here: we must
    // decode the non-keyframes between the seek keyframe and the target.
    avctx->skip_loop_filter = AVDISCARD_ALL;
    avctx->flags2 |= AV_CODEC_FLAG2_FAST;

    // Decode strictly single-threaded. Frame/slice threading would spawn and
    // tear down an internal libavcodec worker pool on *every* hover; doing that
    // dozens of times per second while feeding malformed mid-stream packets
    // (partial GOPs, HEVC + Dolby Vision dual layer) is a known heap-corruption
    // path. A single preview frame does not need threading anyway.
    avctx->thread_count = 1;
    avctx->thread_type = 0;

    if (avcodec_open2(avctx, codec, NULL) < 0) {
        mp_err(log, "thumbnail: avcodec_open2 failed\n");
        avcodec_free_context(&avctx);
        avcodec_parameters_free(&par);
        return NULL;
    }
    avcodec_parameters_free(&par);
    return avctx;
}

// Open the secondary demuxer for `url`, select its (default) video stream and
// build a private decoder. Returns true on success with tc fully populated.
// Must be called with the core UNLOCKED (does file I/O); only allocates talloc
// children of `tc` (never of the shared MPContext), so it is thread-safe as
// long as nothing else touches `tc`.
static bool thumb_ctx_open(struct thumb_ctx *tc, struct mpv_global *global,
                           struct mp_log *log, const char *url)
{
    thumb_ctx_close(tc);

    tc->cancel = mp_cancel_new(tc);

    struct demuxer_params params = {
        .is_top_level = true,
        .allow_playlist_create = false,
        // Mirror the primary loadfile origin so the file/network stream
        // backends accept the URL. Without an origin bit the file stream
        // treats the open as an unsafe playlist reference (STREAM_UNSAFE),
        // the stream loop falls through and the open fails with the
        // misleading "No protocol handler found" error.
        .stream_flags = STREAM_ORIGIN_DIRECT,
    };
    tc->demuxer = demux_open_url(url, &params, tc->cancel, global);
    if (!tc->demuxer) {
        mp_verbose(log, "thumbnail: failed to open secondary demuxer\n");
        thumb_ctx_close(tc);
        return false;
    }

    // Pick a video stream: prefer the default flagged one, else the first.
    struct sh_stream *vsh = NULL;
    int num = demux_get_num_stream(tc->demuxer);
    for (int n = 0; n < num; n++) {
        struct sh_stream *sh = demux_get_stream(tc->demuxer, n);
        if (sh && sh->type == STREAM_VIDEO && !sh->image) {
            if (!vsh)
                vsh = sh;
            if (sh->default_track) {
                vsh = sh;
                break;
            }
        }
    }
    if (!vsh) {
        mp_verbose(log, "thumbnail: no video stream in secondary demuxer\n");
        thumb_ctx_close(tc);
        return false;
    }

    demuxer_select_track(tc->demuxer, vsh, MP_NOPTS_VALUE, true);

    tc->avctx = thumb_build_decoder(log, vsh->codec, &tc->tb);
    if (!tc->avctx) {
        thumb_ctx_close(tc);
        return false;
    }

    tc->vsh = vsh;
    tc->url = talloc_strdup(tc, url);
    return true;
}

// Keyframe-seek the secondary demuxer to <= target, decode forward and return
// the frame whose pts is closest to `target` as a freshly referenced mp_image
// owned by `ta_parent` (or NULL). Must be called with the core UNLOCKED.
static struct mp_image *thumb_seek_decode(void *ta_parent, struct mp_log *log,
                                          struct thumb_ctx *tc, double target)
{
    struct mp_image *best = NULL;
    double best_diff = INFINITY;

    AVPacket *avp = av_packet_alloc();
    AVFrame *avf = av_frame_alloc();
    if (!avp || !avf)
        goto done;

    // Backward (keyframe) seek: SEEK_FORWARD unset => prefer the keyframe at or
    // before the target. No SEEK_HR => no exact/slow seek. With no demux thread
    // running this executes synchronously.
    avcodec_flush_buffers(tc->avctx);
    demux_seek(tc->demuxer, target, 0);

    int read = 0;
    bool draining = false;
    bool stop = false;

    while (!stop) {
        if (!draining && read < THUMB_MAX_PACKETS) {
            struct demux_packet *dp = NULL;
            // No demux thread => this blocks until a video packet or EOF.
            int r = demux_read_packet_async(tc->vsh, &dp);
            if (dp) {
                read++;
                // mp_set_av_packet() makes avp *borrow* the demux packet's
                // refcounted buffer/side_data without taking a ref.
                // avcodec_send_packet() refs whatever it needs to retain, so
                // once it returns we can clear the borrow and free our packet.
                mp_set_av_packet(avp, dp, &tc->tb);
                avcodec_send_packet(tc->avctx, avp);
                mp_set_av_packet(avp, NULL, NULL);
                talloc_free(dp);
            } else if (r < 0) {
                // EOF: flush the decoder so reordered tail frames come out.
                avcodec_send_packet(tc->avctx, NULL);
                draining = true;
            } else {
                // r == 0 only happens with a running demux thread, which we
                // never start; treat as EOF to be safe.
                avcodec_send_packet(tc->avctx, NULL);
                draining = true;
            }
        } else {
            avcodec_send_packet(tc->avctx, NULL);
            draining = true;
        }

        for (;;) {
            int err = avcodec_receive_frame(tc->avctx, avf);
            if (err < 0) {
                if (err == AVERROR_EOF)
                    stop = true;
                break;
            }

            int64_t apts = avf->best_effort_timestamp != AV_NOPTS_VALUE
                         ? avf->best_effort_timestamp : avf->pts;
            double fpts = mp_pts_from_av(apts, &tc->tb);
            double diff = isfinite(fpts) ? fabs(fpts - target) : INFINITY;

            if (diff < best_diff) {
                struct mp_image *mpi = mp_image_from_av_frame(avf);
                if (mpi) {
                    talloc_free(best);
                    best = talloc_steal(ta_parent, mpi);
                    best_diff = diff;
                }
            }

            // Once we have decoded a frame at/after the target, the closest
            // frame has been seen and going further only increases the distance.
            bool reached = isfinite(fpts) && fpts >= target;
            av_frame_unref(avf);
            if (reached)
                stop = true;
        }
    }

done:
    // avp may still borrow the last demux packet's buffer/side_data. Clear the
    // borrowed pointers so av_packet_free() can't unref data we don't own.
    if (avp)
        mp_set_av_packet(avp, NULL, NULL);
    av_frame_free(&avf);
    av_packet_free(&avp);
    return best;
}

// Scale `src` down to `max_width` (keeping display aspect, never upscaling) and
// convert to BGRA. Returned image is allocated with talloc(NULL) and must be
// freed (or stolen) by the caller.
static struct mp_image *thumb_scale_bgra(struct mp_log *log,
                                         struct mp_image *src, int max_width)
{
    int dw, dh;
    mp_image_params_get_dsize(&src->params, &dw, &dh);
    if (dw < 1 || dh < 1) {
        dw = src->w;
        dh = src->h;
    }

    int tw = max_width;
    if (tw < 16)
        tw = 16;
    if (tw > dw)
        tw = dw;
    int th = (int)lround((double)tw * dh / dw);
    if (th < 1)
        th = 1;

    // Even dimensions keep swscale happy across chroma subsampling.
    tw &= ~1;
    th &= ~1;
    if (tw < 2)
        tw = 2;
    if (th < 2)
        th = 2;

    struct mp_image_params p = {
        .imgfmt = IMGFMT_BGRA,
        .w = tw,
        .h = th,
        .p_w = 1,
        .p_h = 1,
    };
    mp_image_params_guess_csp(&p);

    struct mp_image *dst = mp_image_alloc(p.imgfmt, p.w, p.h);
    if (!dst) {
        mp_err(log, "thumbnail: out of memory\n");
        return NULL;
    }
    mp_image_copy_attributes(dst, src);
    dst->params = p;

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

// ---------------------------------------------------------------------------
// Cache-only route (network sources): preview strictly from the *playback*
// demuxer's already-buffered packets. Opens no second connection and creates no
// second server session, so it is safe against streaming services (Emby /
// Jellyfin / Plex) that count and cap concurrent streams per device. Positions
// outside the buffered range simply report cached=false.
// ---------------------------------------------------------------------------

// How far past the requested timestamp we let the cache visit run, so the
// decoder is guaranteed to emit at least one frame at or after the target (the
// visit always starts at the keyframe <= target). Kept small so we never decode
// more than roughly one GOP.
#define THUMB_FORWARD_WINDOW 0.5

struct thumb_collect {
    void *ta_ctx;
    struct demux_packet **pkts;
    int num_pkts;
};

static void thumb_collect_cb(void *ctx, struct demux_packet *dp)
{
    struct thumb_collect *c = ctx;
    if (c->num_pkts >= THUMB_MAX_PACKETS) {
        talloc_free(dp);
        return;
    }
    // The visit hands us ownership of dp; keep it alive under our context.
    talloc_steal(c->ta_ctx, dp);
    MP_TARRAY_APPEND(c->ta_ctx, c->pkts, c->num_pkts, dp);
}

// Decode the collected cache packets and return the frame whose pts is closest
// to `target` as a freshly referenced mp_image owned by `ta_parent` (or NULL).
static struct mp_image *thumb_decode_closest(void *ta_parent, struct mp_log *log,
                                             AVCodecContext *avctx, AVRational tb,
                                             struct demux_packet **pkts,
                                             int num_pkts, double target)
{
    struct mp_image *best = NULL;
    double best_diff = INFINITY;

    AVPacket *avp = av_packet_alloc();
    AVFrame *avf = av_frame_alloc();
    if (!avp || !avf)
        goto done;

    for (int i = 0; i <= num_pkts; i++) {
        if (i < num_pkts) {
            // mp_set_av_packet() makes avp *borrow* the demux packet's
            // refcounted buffer/side_data without taking a ref. We must NOT
            // unref avp afterwards: that would decrement a buffer we don't own
            // and free the demuxer's still-referenced packet data -> heap
            // corruption. The next mp_set_av_packet() (or the cleanup below)
            // clears it safely.
            mp_set_av_packet(avp, pkts[i], &tb);
            int err = avcodec_send_packet(avctx, avp);
            mp_set_av_packet(avp, NULL, NULL);
            if (err < 0 && err != AVERROR(EAGAIN))
                continue;
        } else {
            // Flush so reordered (B-)frames at the tail are emitted.
            avcodec_send_packet(avctx, NULL);
        }

        for (;;) {
            int err = avcodec_receive_frame(avctx, avf);
            if (err < 0)
                break;

            int64_t apts = avf->best_effort_timestamp != AV_NOPTS_VALUE
                         ? avf->best_effort_timestamp : avf->pts;
            double fpts = mp_pts_from_av(apts, &tb);
            double diff = isfinite(fpts) ? fabs(fpts - target) : INFINITY;

            if (diff < best_diff) {
                struct mp_image *mpi = mp_image_from_av_frame(avf);
                if (mpi) {
                    talloc_free(best);
                    best = talloc_steal(ta_parent, mpi);
                    best_diff = diff;
                }
            }
            av_frame_unref(avf);
        }
    }

done:
    // avp may still borrow the last demux packet's buffer/side_data. Clear those
    // borrowed pointers so av_packet_free() can't unref data we don't own.
    if (avp)
        mp_set_av_packet(avp, NULL, NULL);
    av_frame_free(&avf);
    av_packet_free(&avp);
    return best;
}

// Produce a BGRA preview from the *playback* demuxer's cached packets only (no
// extra connection). Enters and returns with the core LOCKED; unlocks only for
// the heavy decode + scale. Returns a talloc(NULL)-rooted BGRA image, or NULL
// (no frame: position not buffered / decode failure).
static struct mp_image *thumb_produce_cache(struct MPContext *mpctx, void *tmp,
                                            struct mp_log *log, double target,
                                            int max_width)
{
    AVCodecContext *avctx = NULL;
    struct mp_image *frame = NULL;
    struct mp_image *out = NULL;
    bool unlocked = false;

    struct track *track = mpctx->current_track[0][STREAM_VIDEO];
    if (!track || !track->stream || !track->demuxer)
        goto done;

    struct demuxer *demuxer = track->demuxer;
    struct sh_stream *sh = track->stream;

    // Snapshot the already-buffered packets around the target under the core
    // lock (in-memory only, no I/O, no low-level seek; returns private copies we
    // own), so the decode below can run safely after the core is released. Do
    // this *before* building a decoder so an out-of-buffer hover (the common hot
    // case for network sources) costs only a cheap cache walk, not an
    // avcodec_open2.
    struct thumb_collect collect = { .ta_ctx = tmp };
    bool any = demux_cache_visit_packets(demuxer, sh, target,
                                         target + THUMB_FORWARD_WINDOW,
                                         NULL, NULL, thumb_collect_cb, &collect);
    if (!any || collect.num_pkts == 0)
        goto done;

    AVRational tb;
    avctx = thumb_build_decoder(log, sh->codec, &tb);
    if (!avctx)
        goto done;

    // Heavy work with the core released (see cmd_thumbnail_raw). Only touches our
    // private decoder, the packet copies owned by `tmp`, and the thread-safe
    // mp_log — never mpctx state.
    mp_core_unlock(mpctx);
    unlocked = true;

    frame = thumb_decode_closest(tmp, log, avctx, tb,
                                 collect.pkts, collect.num_pkts, target);
    if (frame)
        out = thumb_scale_bgra(log, frame, max_width);

    mp_core_lock(mpctx);
    unlocked = false;

done:
    if (unlocked)
        mp_core_lock(mpctx);
    if (avctx)
        avcodec_free_context(&avctx);
    return out;
}

void cmd_thumbnail_raw(void *p)
{
    struct mp_cmd_ctx *cmd = p;
    struct MPContext *mpctx = cmd->mpctx;
    struct mpv_node *res = &cmd->result;

    cmd->success = false;

    double target = cmd->args[0].v.d;
    int max_width = cmd->args[1].v.i;
    int mode = cmd->args[2].v.i; // 0=auto, 1=local (secondary), 2=cache-only
    if (max_width <= 0)
        max_width = 320;
    if (!isfinite(target))
        return;
    if (target < 0)
        target = 0;

    // This command is registered with .spawn_thread, so it runs on a worker
    // thread and enters with the core locked. The persistent secondary-demuxer
    // state must be allocated under the lock (talloc on the shared MPContext is
    // not thread-safe), but the heavy demux/seek/decode/scale work runs with the
    // core UNLOCKED to avoid blocking the playloop / render context / sync
    // client API calls (which would deadlock on a hot hover path).

    void *tmp = talloc_new(NULL);
    struct mp_log *log = mpctx->log;
    struct mpv_global *global = mpctx->global;
    struct mp_image *frame = NULL;
    struct mp_image *out = NULL;
    struct thumb_ctx *tc = NULL;
    char *url = NULL;
    bool unlocked = false;

    // Routing between the two preview strategies. The caller (which knows the
    // exact source kind: local disk vs. a session-limited streaming service)
    // selects the mode explicitly; "auto" falls back to mpv's own is_network
    // heuristic for direct opens with no caller hint.
    //
    // Network / session-limited sources (Emby / Jellyfin / Plex / WebDAV / SMB /
    // Alist / 115 / ... ) must NOT open a second connection or server session:
    // that would be counted as an extra concurrent stream (and may spin up a
    // second transcode), which is hostile to the server and can get the user
    // throttled. Preview them strictly from the playback demuxer's already-
    // buffered cache; out-of-buffer positions return unavailable. Only genuine
    // local files use the whole-timeline secondary demuxer below.
    bool use_cache;
    if (mode == 1)
        use_cache = false; // local: force whole-timeline secondary demuxer
    else if (mode == 2)
        use_cache = true;  // streaming: force cache-only
    else
        use_cache = mpctx->demuxer && mpctx->demuxer->is_network; // auto
    if (use_cache) {
        out = thumb_produce_cache(mpctx, tmp, log, target, max_width);
        // thumb_produce_cache returns with the core locked.
        if (!out)
            goto unavailable;
        goto publish;
    }

    // Snapshot the current playback URL (under the lock) and ensure the state
    // struct exists.
    if (mpctx->demuxer && mpctx->demuxer->filename)
        url = talloc_strdup(tmp, mpctx->demuxer->filename);
    if (!url)
        goto unavailable;

    tc = mpctx->thumbnail;
    if (!tc) {
        tc = talloc_zero(mpctx, struct thumb_ctx);
        talloc_set_destructor(tc, thumb_ctx_destroy);
        mpctx->thumbnail = tc;
    }

    // Everything below only touches `tc`, the secondary demuxer/decoder and our
    // private `tmp` allocations — never shared MPContext state — so it is safe
    // with the core released.
    mp_core_unlock(mpctx);
    unlocked = true;

    if (tc->demuxer && (!tc->url || strcmp(tc->url, url) != 0))
        thumb_ctx_close(tc); // playback switched files: drop the stale demuxer

    if (!tc->demuxer && !thumb_ctx_open(tc, global, log, url))
        goto relock_unavailable;

    frame = thumb_seek_decode(tmp, log, tc, target);
    if (frame)
        out = thumb_scale_bgra(log, frame, max_width);

    mp_core_lock(mpctx);
    unlocked = false;

    if (!out)
        goto unavailable;

publish:
    node_init(res, MPV_FORMAT_NODE_MAP, NULL);
    node_map_add_flag(res, "cached", true);
    node_map_add_int64(res, "w", out->w);
    node_map_add_int64(res, "h", out->h);
    node_map_add_int64(res, "stride", out->stride[0]);
    node_map_add_string(res, "format", "bgra");
    struct mpv_byte_array *ba =
        node_map_add(res, "data", MPV_FORMAT_BYTE_ARRAY)->u.ba;
    *ba = (struct mpv_byte_array){
        .data = out->planes[0],
        .size = (size_t)out->stride[0] * out->h,
    };
    talloc_steal(ba, out);
    cmd->success = true;
    goto done;

relock_unavailable:
    mp_core_lock(mpctx);
    unlocked = false;

unavailable:
    // No frame could be produced (no file, open/seek/decode failure). This is a
    // normal, non-error outcome on a hot hover path: report it so callers can
    // cheaply show a "no preview" state without an exception.
    node_init(res, MPV_FORMAT_NODE_MAP, NULL);
    node_map_add_flag(res, "cached", false);
    cmd->success = true;

done:
    // Always return with the core locked (the worker-thread framework re-locks
    // around mp_cmd_ctx_complete after the handler returns).
    if (unlocked)
        mp_core_lock(mpctx);
    talloc_free(tmp);
}
