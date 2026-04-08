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
 * Whisper lookahead pipeline: runs a secondary audio decode + whisper filter
 * ahead of the main playback pipeline, so subtitles are ready before the
 * player reaches the corresponding audio.
 *
 * Architecture:
 *   [Secondary Demuxer (same file)] -> [Audio Decoder] -> [lavfi whisper] -> [Sink]
 *                                                                             |
 *   The sink extracts whisper text metadata and injects it as subtitle        |
 *   packets into the primary audio stream via demuxer_feed_af_sub().          v
 *                                              Primary playback (no whisper in af chain)
 *
 * Initialization runs on a dedicated background thread (init_thread) so that
 * heavy operations like opening the secondary demuxer, creating the audio
 * decoder, and loading the whisper model do not block the main playloop.
 */

#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>
#include <math.h>

#include "mpv_talloc.h"

#include "common/msg.h"
#include "common/common.h"
#include "options/options.h"
#include "options/path.h"
#include "osdep/threads.h"

#include "audio/aframe.h"
#include "demux/demux.h"
#include "demux/stheader.h"
#include "filters/f_decoder_wrapper.h"
#include "filters/f_lavfi.h"
#include "filters/filter.h"
#include "filters/filter_internal.h"
#include "misc/dispatch.h"
#include "misc/thread_tools.h"
#include "stream/stream.h"

#include <libavutil/dict.h>
#include <libavutil/frame.h>

#include "core.h"
#include "whisper_translate.h"

struct whisper_lookahead {
    struct MPContext *mpctx;
    struct mp_log *log;

    // Secondary demuxer (independent of primary playback)
    struct demuxer *demuxer;
    struct sh_stream *audio_stream;     // audio stream in secondary demuxer

    // Primary audio stream (subtitle injection target)
    struct sh_stream *primary_stream;
    struct demuxer *primary_demuxer;    // for packet_pool access

    // Independent filter graph (runs on background thread)
    struct mp_filter *root_filter;
    struct mp_decoder_wrapper *decoder;
    struct mp_lavfi *lavfi;             // whisper filter instance
    struct mp_filter *sink;             // custom sink that extracts metadata

    // Background init thread: performs heavy initialization (demux open,
    // decoder create, whisper model load) without blocking the playloop.
    mp_thread init_thread;
    bool init_thread_valid;
    atomic_bool init_done;              // set when init thread finishes
    bool init_ok;                       // true if init succeeded

    // Lookahead processing thread
    mp_thread thread;
    struct mp_dispatch_queue *dispatch;
    bool thread_valid;
    int terminate;                      // set to 1 to request thread exit

    // Seek synchronization
    double seek_pts;
    bool seek_pending;

    // Deduplication
    char *last_text;

    // Lifecycle
    struct mp_cancel *cancel;

    // Parameters captured at start time for the init thread
    char *filename;
    char *whisper_opts;
    int audio_demuxer_id;
    bool rebase_start_time;
    double start_seek_pts;              // playback position to seek to

    // Track auto-selection (only once)
    bool track_selected;

    // Debug counters
    int frames_received;
    int frames_with_meta;
    int subtitles_injected;

    // Translation
    struct whisper_translator *translator;
    int translations_injected;
};

// --- Custom sink filter: consumes audio frames, extracts whisper metadata ---

struct sink_priv {
    struct whisper_lookahead *wl;
};

static void sink_process(struct mp_filter *f)
{
    struct sink_priv *p = f->priv;
    struct whisper_lookahead *wl = p->wl;

    while (mp_pin_out_request_data(f->ppins[0])) {
        struct mp_frame frame = mp_pin_out_read(f->ppins[0]);
        if (frame.type == MP_FRAME_NONE)
            return;

        if (frame.type == MP_FRAME_EOF) {
            MP_INFO(wl, "sink: received EOF after %d frames (%d with meta, %d injected)\n",
                    wl->frames_received, wl->frames_with_meta, wl->subtitles_injected);
            mp_frame_unref(&frame);
            return;
        }

        if (frame.type != MP_FRAME_AUDIO) {
            MP_INFO(wl, "sink: unexpected frame type %d\n", frame.type);
            mp_frame_unref(&frame);
            continue;
        }

        wl->frames_received++;

        // Log first frame and periodic progress
        if (wl->frames_received == 1) {
            struct mp_aframe *af = frame.data;
            MP_INFO(wl, "sink: first audio frame received, pts=%.3f, rate=%d\n",
                    mp_aframe_get_pts(af), mp_aframe_get_rate(af));
        }
        if (wl->frames_received % 500 == 0) {
            MP_INFO(wl, "sink: progress %d frames, %d subtitles injected\n",
                    wl->frames_received, wl->subtitles_injected);
        }

        struct mp_aframe *af = frame.data;
        AVFrame *avf = mp_aframe_get_raw_avframe(af);
        if (avf && avf->metadata) {
            // Log all metadata keys on first frame that has any
            if (wl->frames_with_meta == 0) {
                MP_INFO(wl, "sink: first frame with metadata, keys:\n");
                const AVDictionaryEntry *t = NULL;
                while ((t = av_dict_get(avf->metadata, "", t, AV_DICT_IGNORE_SUFFIX)))
                    MP_INFO(wl, "  '%s' = '%s'\n", t->key, t->value);
            }
            wl->frames_with_meta++;

            const AVDictionaryEntry *e =
                av_dict_get(avf->metadata, "lavfi.whisper.text", NULL, 0);
            const char *text = e ? e->value : NULL;
            if (text && text[0]) {
                bool changed = !wl->last_text ||
                               strcmp(wl->last_text, text) != 0;
                if (changed) {
                    talloc_free(wl->last_text);
                    wl->last_text = talloc_strdup(wl, text);

                    if (wl->primary_stream && wl->primary_demuxer) {
                        // Use start_ms from whisper metadata for accurate PTS
                        const AVDictionaryEntry *e_start =
                            av_dict_get(avf->metadata, "lavfi.whisper.start_ms", NULL, 0);
                        const AVDictionaryEntry *e_dur =
                            av_dict_get(avf->metadata, "lavfi.whisper.duration", NULL, 0);

                        double pts;
                        if (e_start) {
                            pts = strtoll(e_start->value, NULL, 10) / 1000.0;
                        } else {
                            pts = mp_aframe_get_pts(af);
                        }

                        double dur = e_dur ? atof(e_dur->value) : 5.0;

                        // Build subtitle text: if translator is active,
                        // create bilingual ASS event (translated on top,
                        // original smaller below); otherwise just the
                        // original text as a plain ASS dialogue line.
                        char *sub_text = NULL;
                        if (wl->translator) {
                            char *translated = whisper_translate(
                                wl->translator, wl, text);
                            if (translated) {
                                // ASS dialogue text:
                                // Top: translated (white, large, black outline)
                                // Bottom: original (light yellow, smaller, black outline)
                                // \c&H00FFFFFF& = white, \c&H00E0FFFF& = light yellow (BGR)
                                // \3c&H00000000& = black outline
                                sub_text = talloc_asprintf(wl,
                                    "{\\fs72\\c&H00FFFFFF&\\3c&H00000000&\\bord3}%s"
                                    "\\N{\\fs48\\c&H00E0FFFF&\\3c&H00000000&\\bord2}%s",
                                    translated, text);
                                wl->translations_injected++;
                                MP_INFO(wl, "translated #%d: %.40s%s\n",
                                        wl->translations_injected,
                                        translated,
                                        strlen(translated) > 40 ? "..." : "");
                                talloc_free(translated);
                            }
                        }

                        // Fallback: no translator or translation failed
                        if (!sub_text)
                            sub_text = talloc_strdup(wl, text);

                        // Build ASS chunk for ass_process_chunk():
                        // Format: ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,Effect,Text
                        // (no Start/End — PTS and duration come from demux_packet)
                        char *ass_line = talloc_asprintf(wl,
                            "%d,0,Default,,0,0,0,,%s",
                            wl->subtitles_injected, sub_text);
                        talloc_free(sub_text);

                        size_t ass_len = strlen(ass_line);
                        struct demux_packet *dp = new_demux_packet_from(
                            wl->primary_demuxer->packet_pool,
                            (void *)ass_line, ass_len);
                        if (dp) {
                            dp->pts = pts;
                            dp->duration = dur;
                            dp->sub_duration = dur;

                            demuxer_feed_af_sub(wl->primary_stream, dp);
                            wl->subtitles_injected++;
                            MP_INFO(wl, "subtitle #%d @ %.3f (dur=%.1f): %.40s%s\n",
                                    wl->subtitles_injected, pts, dur, text,
                                    strlen(text) > 40 ? "..." : "");

                            // Wake up the main playloop so it can pick up
                            // the new subtitle track (on first injection)
                            // and display the subtitle.
                            mp_wakeup_core(wl->mpctx);
                        }
                        talloc_free(ass_line);
                    }
                }
            }
        }

        mp_frame_unref(&frame);
    }
}

static const struct mp_filter_info sink_filter_info = {
    .name = "whisper_sink",
    .priv_size = sizeof(struct sink_priv),
    .process = sink_process,
};

// --- Background threads ---

static void wakeup_lookahead(void *ctx)
{
    struct whisper_lookahead *wl = ctx;
    mp_dispatch_interrupt(wl->dispatch);
}

static void onlock_lookahead(void *ctx)
{
    struct whisper_lookahead *wl = ctx;
    mp_filter_graph_interrupt(wl->root_filter);
}

static MP_THREAD_VOID lookahead_thread(void *ptr)
{
    struct whisper_lookahead *wl = ptr;
    mp_thread_set_name("whisper/la");

    MP_INFO(wl, "thread: started\n");

    int iterations = 0;
    while (!wl->terminate) {
        // Handle pending seek
        if (wl->seek_pending) {
            wl->seek_pending = false;
            double pts = wl->seek_pts;
            MP_INFO(wl, "thread: seeking to %.3f\n", pts);
            demux_seek(wl->demuxer, pts, SEEK_BLOCK);
            demux_block_reading(wl->demuxer, false);
            mp_filter_reset(wl->root_filter);
            talloc_free(wl->last_text);
            wl->last_text = NULL;
        }

        bool progress = mp_filter_graph_run(wl->root_filter);

        iterations++;
        if (iterations == 1 || iterations == 10 || iterations == 100) {
            MP_INFO(wl, "thread: iteration %d, graph_run returned %s, "
                    "frames=%d\n",
                    iterations, progress ? "progress" : "blocked",
                    wl->frames_received);
        }

        // Use finite timeout so the thread can check wl->terminate
        // periodically, even if no filter graph wakeup arrives (e.g.,
        // when whisper inference blocks for an extended period and the
        // user closes the player).
        mp_dispatch_queue_process(wl->dispatch, 0.5);
    }

    MP_INFO(wl, "thread: exiting after %d iterations, %d frames\n",
            iterations, wl->frames_received);

    MP_THREAD_RETURN();
}

// --- Initialization thread ---
// Performs all heavy work (demux open, decoder init, whisper model load)
// on a background thread to avoid blocking the main playloop.

static MP_THREAD_VOID init_thread_fn(void *ptr)
{
    struct whisper_lookahead *wl = ptr;
    mp_thread_set_name("whisper/init");

    MP_INFO(wl, "init: starting background initialization\n");

    // Open secondary demuxer for the same file
    struct demuxer_params params = {
        .is_top_level = true,
        .stream_flags = STREAM_ORIGIN_DIRECT,
        .allow_playlist_create = false,
    };

    MP_INFO(wl, "init: opening secondary demuxer for '%s'\n", wl->filename);

    struct demuxer *demuxer = demux_open_url(wl->filename, &params, wl->cancel,
                                             wl->mpctx->global);

    if (!demuxer) {
        MP_ERR(wl, "init: failed to open secondary demuxer\n");
        goto done;
    }

    // Check for cancellation after the slowest operation
    if (mp_cancel_test(wl->cancel))
        goto fail_demuxer;

    MP_INFO(wl, "init: secondary demuxer opened, format=%s, streams=%d\n",
            demuxer->filetype ? demuxer->filetype : "(null)",
            demux_get_num_stream(demuxer));

    wl->demuxer = demuxer;

    // Rebase timestamps like the primary demuxer
    if (wl->rebase_start_time)
        demux_set_ts_offset(demuxer, -demuxer->start_time);

    // Find matching audio stream by demuxer_id
    int target_id = wl->audio_demuxer_id;
    struct sh_stream *audio_sh = NULL;
    for (int n = 0; n < demux_get_num_stream(demuxer); n++) {
        struct sh_stream *sh = demux_get_stream(demuxer, n);
        MP_INFO(wl, "init: stream[%d] type=%d demuxer_id=%d codec=%s\n",
                n, sh->type, sh->demuxer_id,
                sh->codec ? (sh->codec->codec ? sh->codec->codec : "?") : "null");
        if (sh->type == STREAM_AUDIO) {
            if (target_id < 0 || sh->demuxer_id == target_id) {
                audio_sh = sh;
                break;
            }
            if (!audio_sh)
                audio_sh = sh; // fallback to first audio
        }
    }

    if (!audio_sh) {
        MP_ERR(wl, "init: no audio stream in secondary demuxer\n");
        goto fail_demuxer;
    }

    MP_INFO(wl, "init: selected audio stream[%d] codec=%s\n",
            audio_sh->index,
            audio_sh->codec ? (audio_sh->codec->codec ? audio_sh->codec->codec : "?") : "null");

    wl->audio_stream = audio_sh;

    // Select the audio stream and start background demuxing
    demuxer_select_track(demuxer, audio_sh, 0, true);
    demux_start_thread(demuxer);
    demux_start_prefetch(demuxer);
    MP_INFO(wl, "init: demux thread started\n");

    // Create independent filter graph root
    wl->root_filter = mp_filter_create_root(wl->mpctx->global);
    if (!wl->root_filter) {
        MP_ERR(wl, "init: failed to create filter root\n");
        goto fail_demuxer;
    }

    // Create dispatch queue for thread synchronization
    wl->dispatch = mp_dispatch_create(wl);
    mp_filter_graph_set_wakeup_cb(wl->root_filter, wakeup_lookahead, wl);
    mp_dispatch_set_onlock_fn(wl->dispatch, onlock_lookahead, wl);

    // Create decoder wrapper
    MP_INFO(wl, "init: creating audio decoder...\n");
    wl->decoder = mp_decoder_wrapper_create(wl->root_filter, audio_sh);
    if (!wl->decoder) {
        MP_ERR(wl, "init: failed to create audio decoder\n");
        goto fail_filter;
    }

    MP_INFO(wl, "init: initializing audio decoder (reinit)...\n");
    if (!mp_decoder_wrapper_reinit(wl->decoder)) {
        MP_ERR(wl, "init: failed to init audio decoder\n");
        goto fail_filter;
    }
    MP_INFO(wl, "init: audio decoder initialized\n");

    if (mp_cancel_test(wl->cancel))
        goto fail_filter;

    // Parse whisper_opts into key=value array for mp_lavfi_create_filter
    int num_opts = 0;
    char **filter_opts = NULL;
    char *translate_to = NULL;
    enum wt_provider translate_provider = WT_PROVIDER_NONE;
    char *whisper_language = NULL;
    if (wl->whisper_opts && wl->whisper_opts[0]) {
        char *opts_copy = talloc_strdup(wl, wl->whisper_opts);
        char *p = opts_copy;
        while (p && *p) {
            char *comma = strchr(p, ',');
            if (comma)
                *comma = '\0';
            char *eq = strchr(p, '=');
            if (eq) {
                *eq = '\0';
                if (strcmp(p, "translate_to") == 0) {
                    translate_to = talloc_strdup(wl, eq + 1);
                } else if (strcmp(p, "translate_provider") == 0) {
                    if (strcmp(eq + 1, "google") == 0)
                        translate_provider = WT_PROVIDER_GOOGLE;
                    else if (strcmp(eq + 1, "azure") == 0)
                        translate_provider = WT_PROVIDER_AZURE;
                } else {
                    if (strcmp(p, "language") == 0)
                        whisper_language = talloc_strdup(wl, eq + 1);
                    MP_TARRAY_APPEND(wl, filter_opts, num_opts, talloc_strdup(wl, p));
                    MP_TARRAY_APPEND(wl, filter_opts, num_opts, talloc_strdup(wl, eq + 1));
                }
            }
            p = comma ? comma + 1 : NULL;
        }
    }
    // Force synchronous mode: lookahead runs on its own thread at full speed,
    // so we want whisper's filter_frame to block until inference completes.
    MP_TARRAY_APPEND(wl, filter_opts, num_opts, talloc_strdup(wl, "sync"));
    MP_TARRAY_APPEND(wl, filter_opts, num_opts, talloc_strdup(wl, "1"));
    MP_TARRAY_APPEND(wl, filter_opts, num_opts, NULL);

    MP_INFO(wl, "init: creating whisper filter with %d option pairs:\n",
            (num_opts - 1) / 2);
    for (int i = 0; i + 1 < num_opts && filter_opts[i]; i += 2) {
        MP_INFO(wl, "  %s = %s\n", filter_opts[i], filter_opts[i + 1]);
    }

    wl->lavfi = mp_lavfi_create_filter(wl->root_filter, MP_FRAME_AUDIO, true,
                                        NULL, NULL, "whisper", filter_opts);
    if (!wl->lavfi) {
        MP_ERR(wl, "init: failed to create whisper filter "
               "(is the whisper filter available in ffmpeg?)\n");
        goto fail_filter;
    }

    if (mp_cancel_test(wl->cancel))
        goto fail_filter;

    MP_INFO(wl, "init: whisper filter created, pins: in=%d out=%d\n",
            wl->lavfi->f->num_pins,
            wl->lavfi->f->num_pins >= 2 ? 2 : wl->lavfi->f->num_pins);

    // Create sink filter
    wl->sink = mp_filter_create(wl->root_filter, &sink_filter_info);
    if (!wl->sink) {
        MP_ERR(wl, "init: failed to create sink\n");
        goto fail_filter;
    }
    struct sink_priv *sp = wl->sink->priv;
    sp->wl = wl;
    mp_filter_add_pin(wl->sink, MP_PIN_IN, "in");

    // Connect pipeline: decoder -> whisper -> sink
    mp_pin_connect(wl->lavfi->f->pins[0], wl->decoder->f->pins[0]);
    mp_pin_connect(wl->sink->pins[0], wl->lavfi->f->pins[1]);

    MP_INFO(wl, "init: pipeline connected: decoder -> whisper -> sink\n");

    // Seek the lookahead to the captured playback position
    if (wl->start_seek_pts != MP_NOPTS_VALUE && wl->start_seek_pts > 0.5) {
        MP_INFO(wl, "init: seeking to playback pos %.3f\n", wl->start_seek_pts);
        demux_seek(demuxer, wl->start_seek_pts, SEEK_BLOCK);
        demux_block_reading(demuxer, false);
    } else {
        MP_INFO(wl, "init: starting from beginning (seek_pts=%.3f)\n",
                wl->start_seek_pts);
    }

    // Initialize translator if requested
    if (translate_to && translate_to[0] && translate_provider != WT_PROVIDER_NONE) {
        const char *src_lang = whisper_language ? whisper_language : "auto";
        wl->translator = whisper_translator_create(wl, wl->log,
                                                    translate_provider,
                                                    src_lang, translate_to);
        if (wl->translator) {
            MP_INFO(wl, "init: translator enabled (%s -> %s, %s)\n",
                    src_lang, translate_to,
                    translate_provider == WT_PROVIDER_GOOGLE ? "google" : "azure");
        } else {
            MP_WARN(wl, "init: failed to create translator\n");
        }
    }

    // Start lookahead processing thread
    if (mp_thread_create(&wl->thread, lookahead_thread, wl)) {
        MP_ERR(wl, "init: failed to create lookahead thread\n");
        goto fail_filter;
    }
    wl->thread_valid = true;

    wl->init_ok = true;
    MP_INFO(wl, "init: pipeline started successfully\n");
    atomic_store(&wl->init_done, true);
    mp_wakeup_core(wl->mpctx);

    MP_THREAD_RETURN();

fail_filter:
    talloc_free(wl->root_filter);
    wl->root_filter = NULL;
fail_demuxer:
    if (wl->demuxer) {
        demux_cancel_and_free(wl->demuxer);
        wl->demuxer = NULL;
    }
done:
    wl->init_ok = false;
    atomic_store(&wl->init_done, true);
    mp_wakeup_core(wl->mpctx);

    MP_THREAD_RETURN();
}

// --- Public API ---

void whisper_lookahead_start(struct MPContext *mpctx, const char *whisper_opts)
{
    // Stop any existing lookahead
    whisper_lookahead_stop(mpctx);

    if (!mpctx->demuxer || !mpctx->filename) {
        MP_ERR(mpctx, "whisper lookahead: no file loaded\n");
        return;
    }

    // Find primary audio track
    struct track *audio_track = mpctx->current_track[0][STREAM_AUDIO];
    if (!audio_track || !audio_track->stream || !audio_track->demuxer) {
        MP_ERR(mpctx, "whisper lookahead: no audio track selected\n");
        return;
    }

    MP_INFO(mpctx, "whisper lookahead: starting (async), opts='%s'\n", whisper_opts);

    struct whisper_lookahead *wl = talloc_zero(NULL, struct whisper_lookahead);
    wl->mpctx = mpctx;
    wl->log = mp_log_new(wl, mpctx->log, "whisper-la");
    wl->primary_stream = audio_track->stream;
    wl->primary_demuxer = audio_track->demuxer;
    atomic_store(&wl->init_done, false);

    // Create cancellation token slaved to playback_abort
    wl->cancel = mp_cancel_new(wl);
    mp_cancel_set_parent(wl->cancel, mpctx->playback_abort);

    // Capture parameters for init thread (so it doesn't touch mpctx fields
    // that may change or require main-thread access)
    char *path = mp_get_user_path(wl, mpctx->global, mpctx->filename);
    wl->filename = talloc_strdup(wl, path);
    wl->whisper_opts = talloc_strdup(wl, whisper_opts);
    wl->audio_demuxer_id = audio_track->stream->demuxer_id;
    wl->rebase_start_time = mpctx->opts->rebase_start_time;

    // Capture current playback position for the init thread to seek to.
    double playback_pts = get_current_time(mpctx);
    if (playback_pts == MP_NOPTS_VALUE)
        playback_pts = mpctx->last_seek_pts;
    if (playback_pts == MP_NOPTS_VALUE)
        playback_pts = get_start_time(mpctx, 1);
    wl->start_seek_pts = playback_pts;

    // Launch init thread
    if (mp_thread_create(&wl->init_thread, init_thread_fn, wl)) {
        MP_ERR(mpctx, "whisper lookahead: failed to create init thread\n");
        talloc_free(wl);
        return;
    }
    wl->init_thread_valid = true;

    mpctx->whisper_lookahead = wl;
    MP_INFO(mpctx, "whisper lookahead: init thread launched, "
            "playback continues unblocked\n");
}

void whisper_lookahead_stop(struct MPContext *mpctx)
{
    struct whisper_lookahead *wl = mpctx->whisper_lookahead;
    if (!wl)
        return;

    MP_INFO(mpctx, "whisper lookahead: stopping (frames=%d, subs=%d, trans=%d)\n",
            wl->frames_received, wl->subtitles_injected, wl->translations_injected);

    // Cancel any in-progress I/O (this unblocks demux_open_url in the init
    // thread as well as demux reads in the lookahead thread).
    mp_cancel_trigger(wl->cancel);

    // Wait for init thread to finish first (it may still be loading the model)
    if (wl->init_thread_valid)
        mp_thread_join(wl->init_thread);
    wl->init_thread_valid = false;

    // Now tear down the lookahead thread and resources (only if init succeeded)
    if (wl->init_ok) {
        wl->terminate = 1;
        if (wl->demuxer) {
            demux_cancel_and_free(wl->demuxer);
            wl->demuxer = NULL;
        }
        if (wl->root_filter)
            mp_filter_graph_interrupt(wl->root_filter);
        if (wl->dispatch)
            mp_dispatch_interrupt(wl->dispatch);

        if (wl->thread_valid)
            mp_thread_join(wl->thread);

        talloc_free(wl->root_filter);
    }

    // Destroy translator (closes WinHTTP handles)
    whisper_translator_destroy(&wl->translator);

    mpctx->whisper_lookahead = NULL;
    talloc_free(wl);

    MP_INFO(mpctx, "whisper lookahead: stopped\n");
}

void whisper_lookahead_seek(struct MPContext *mpctx, double pts)
{
    struct whisper_lookahead *wl = mpctx->whisper_lookahead;
    if (!wl || pts == MP_NOPTS_VALUE)
        return;

    // Only forward seeks if the pipeline is fully initialized
    if (!atomic_load(&wl->init_done) || !wl->init_ok)
        return;

    wl->seek_pts = pts;
    wl->seek_pending = true;
    mp_filter_graph_interrupt(wl->root_filter);
    mp_dispatch_interrupt(wl->dispatch);
}

bool whisper_lookahead_track_selected(struct MPContext *mpctx)
{
    struct whisper_lookahead *wl = mpctx->whisper_lookahead;
    return wl && wl->track_selected;
}

void whisper_lookahead_set_track_selected(struct MPContext *mpctx, bool val)
{
    struct whisper_lookahead *wl = mpctx->whisper_lookahead;
    if (wl)
        wl->track_selected = val;
}

bool whisper_lookahead_ready(struct MPContext *mpctx)
{
    struct whisper_lookahead *wl = mpctx->whisper_lookahead;
    return wl && atomic_load(&wl->init_done) && wl->init_ok;
}

bool whisper_lookahead_failed(struct MPContext *mpctx)
{
    struct whisper_lookahead *wl = mpctx->whisper_lookahead;
    return wl && atomic_load(&wl->init_done) && !wl->init_ok;
}
