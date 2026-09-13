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
 * Whisper realtime captions (V3.1, zero-disk):
 *
 * Read raw audio packets directly out of the mpv demuxer cache (which already
 * holds 30s+ of read-ahead), decode them with a *private* libavcodec audio
 * decoder, push the PCM frames through `lavfi(aresample=...,whisper=...)`, and
 * inject the resulting segments back as subtitles on the primary audio track.
 *
 *   demuxer cache --[demux_cache_visit_packets]--> private AVCodecContext
 *                                                          |
 *                                                          v AVFrame
 *                                                  mp_aframe_from_avframe
 *                                                          |
 *                              wl->queue --[wl_source mp_filter]--> lavfi
 *                                              |
 *                                              v
 *                            [aresample=16k mono s16, whisper=...]
 *                                              |
 *                                              v
 *                                  [sink mp_filter] -> inject_subtitle
 *
 * Lifecycle / threads:
 *   - The "core" thread (playloop) calls whisper_lookahead_publish() each
 *     iteration to refresh a snapshot (demuxer*, audio sh_stream*, playback
 *     pts, audio cache range).
 *   - The worker thread reads only the snapshot under wl->snap_lock, then
 *     runs demux_cache_visit_packets() from its own thread (NOT via dispatch,
 *     to avoid deadlocking with whisper_lookahead_stop() while the playloop
 *     is blocked in mp_thread_join).
 *   - The demuxer/sh_stream pointers are guaranteed live for the worker's
 *     lifetime: stop() joins the worker before returning, and loadfile.c
 *     calls stop() before tearing down the demuxer.
 *   - On audio chain change / seek / start, snap.graph_generation AND
 *     snap.generation are both bumped under snap_lock. The worker
 *     discards in-flight work that started under an older graph
 *     generation, rebuilds the AVCodecContext if codec params changed,
 *     resets the lavfi graph (which re-creates whisper, clearing its
 *     VAD), and resumes from the new playback position.
 *   - On translator-only invalidate (AI translator config / language
 *     change for the SAME audio), only snap.generation is bumped — the
 *     shared translation scheduler drops in-flight tasks/results, but the
 *     recognition graph is preserved (avoids a multi-second whisper
 *     model reload that would burn a few GB of VRAM).
 *
 * Backpressure: wl->queue is bounded (MAX_QUEUE_*); the worker waits on a
 * condition variable when full and is woken by source consumption,
 * terminate, or generation bump.
 */

#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>
#include <math.h>

#include "mpv_talloc.h"

#include "common/msg.h"
#include "common/common.h"
#include "common/av_common.h"
#include "options/options.h"
#include "osdep/threads.h"
#include "osdep/timer.h"

#include "audio/aframe.h"
#include "demux/demux.h"
#include "demux/packet.h"
#include "demux/stheader.h"
#include "filters/f_lavfi.h"
#include "filters/filter.h"
#include "filters/filter_internal.h"
#include "misc/dispatch.h"
#include "misc/json.h"
#include "misc/node.h"
#include "misc/bstr.h"

#include <mpv/client.h>

#include <libavcodec/avcodec.h>
#include <libavutil/dict.h>
#include <libavutil/frame.h>

#include "core.h"
#include "translation.h"

// Worker tuning. With 16 kHz mono s16 the queue uses ~32 KB/s; 30 s ≈ 1 MB.
#define MAX_QUEUE_SECONDS    30.0
#define MAX_QUEUE_FRAMES     4000
// Chunk window. The first chunk is small so subtitles start appearing fast;
// later chunks are larger so we issue fewer demux-lock takes per second of
// audio. Bounded by playback_pts + LOOKAHEAD_MAX (don't run too far ahead
// of the user — saves CPU and matches what's likely cached).
//
// LOOKAHEAD_MAX_SEC bounds how far ahead of playback_pts the worker is
// allowed to pre-transcribe.  Earlier this was raised to 600 s so PAUSED /
// deep-seek did not stall, but in practice that lets large-v3 + CUDA keep
// the GPU at 100% for minutes at a time on long videos (the worker simply
// chases cache_end, which itself can be several minutes ahead).
//
// 120 s is a compromise:
//   - covers normal pause (user takes a phone call, scrubs around) without
//     letting the worker idle right at the cursor;
//   - bounds GPU utilisation so heavy models don't run unbounded;
//   - MAX_QUEUE_SECONDS (30 s) provides the secondary backpressure on the
//     af_whisper async queue regardless.
//
// When an asynchronous translator pipeline is active with a finite cost-guard
// horizon, we further tighten the recognition cap to roughly the translator's
// horizon. Rationale: before d8339acec5, AI translation was a synchronous
// HTTP call inside inject_subtitle(), which transitively throttled the
// lookahead worker via the sink filter. After the async N-worker pipeline
// landed, recognition is no longer rate-limited by translation throughput
// and the worker happily chases playback+LOOKAHEAD_MAX_SEC even though the
// translator's horizon (default 60 s) means tasks beyond that just sit in
// the pending queue without producing any user-visible captions. That
// surplus recognition keeps either the CUDA compute engine or the Vulkan
// compute queue busy, contends with the player's swapchain for SMs / VRAM
// bandwidth, and surfaces as render stutter when translation falls behind.
// Capping at horizon * 1.2 keeps a small safety buffer for the translator
// without paying the GPU cost of speculative recognition that the
// translator will never serve.
#define FIRST_CHUNK_SECONDS              6.0
#define CHUNK_SECONDS                   12.0
#define MIN_CHUNK_SECONDS                1.5
#define LOOKAHEAD_MAX_SEC              120.0
#define LOOKAHEAD_MIN_TRANSLATE_SEC     30.0  // floor when horizon-gated
#define LOOKAHEAD_TRANSLATE_MARGIN     1.2    // fraction of horizon_sec we
                                              // allow recognition to lead
#define WORKER_TICK_SEC                  0.05 // tighter wakeups during startup ramp-up

struct frame_item {
    struct mp_frame f;
    uint64_t generation;
};

// Snapshot of core-thread state visible to the worker. Updated by
// whisper_lookahead_publish(); read by the worker under snap_lock.
struct wl_snap {
    // Bumped on ALL invalidation events (seek, audio-chain change,
    // translator/subtitle config change). The translator pipeline uses
    // this to drop in-flight tasks/results that no longer match the
    // current configuration.
    uint64_t generation;
    // Bumped only on events that require the recognition graph itself
    // to be reset (seek, audio-chain change). Translator-only invalidate
    // does NOT bump this — re-running the whisper model on the same
    // audio just to redo translation would cost ~3 GB VRAM and ~10 s of
    // CUDA reinit on large-v3. The worker uses this to decide whether
    // to call worker_signal_reset() / mp_filter_reset(root_filter).
    uint64_t graph_generation;
    bool valid;
    struct demuxer *demuxer;
    struct sh_stream *audio_sh;
    struct demuxer *sub_demuxer;
    struct sh_stream *sub_stream;
    double playback_pts;
    double cache_start;
    double cache_end;
    // Codec identity for change detection.
    int codec_id;
    int extradata_size;
    uint8_t extradata_hash[16]; // first 16 bytes; cheap fingerprint
};

struct whisper_lookahead {
    struct MPContext *mpctx;
    struct mp_log *log;

    // -------- snap_lock-protected (cross-thread state) --------
    mp_mutex snap_lock;
    struct wl_snap snap;
    // Highest end-pts the worker has finished processing.  Published from
    // the worker thread for the main thread (whisper_lookahead_seek) to
    // decide whether a player seek lands inside already-processed
    // territory and can therefore skip the generation bump.
    double pub_processed_end;
    // -------- end snap_lock-protected --------

    // -------- queue_lock-protected (frame queue + worker signalling) --------
    mp_mutex queue_lock;
    mp_cond queue_cv;            // signalled on consume, gen bump, terminate
    struct frame_item *queue;
    int num_queue;
    double queued_dur;
    bool reset_pending;          // worker raised; source resets graph
    int last_format;
    int last_rate;
    // -------- end queue_lock-protected --------

    // Filter graph (built by init_thread, driven by worker)
    struct mp_filter *root_filter;
    struct mp_dispatch_queue *graph_dispatch;
    struct mp_filter *source;
    struct mp_lavfi *lavfi_resample;
    struct mp_lavfi *lavfi;
    struct mp_filter *sink;

    char *whisper_opts;

    // Threads
    mp_thread init_thread;
    bool init_thread_valid;
    atomic_bool init_done;
    bool init_ok;
    mp_thread thread;
    bool thread_valid;
    atomic_int terminate;

    // ---- Worker-only state (no lock needed; only the worker touches) ----
    AVCodecContext *avctx;
    AVRational pkt_tb;
    int64_t worker_codec_id;
    int worker_extradata_size;
    uint8_t worker_extradata_hash[16];
    uint64_t worker_generation;        // last gen worker observed
    uint64_t worker_graph_generation;  // last graph-gen worker observed
    double worker_last_done;           // last visited end-pts (NOPTS = none)
    double worker_last_dts;            // last decoded packet's dts (dedupe)
    double worker_next_pts;            // interpolated frame pts fallback
    int chunks_visited;
    int packets_visited;
    int64_t last_starve_log_ns;  /* rate-limit for INFO-level starvation diag */
    int packets_skipped;
    int frames_decoded;
    // True once the worker has pushed at least one audio frame into the
    // lavfi graph since the last graph reset. Used to skip the expensive
    // mp_filter_reset() (which would tear down af_whisper and reload the
    // model) when no audio has reached the recognition graph yet — e.g.
    // initial startup, or a generation bump that fires before the worker
    // produced its first frame.
    bool worker_graph_seen_audio;
    // ---- end worker-only state ----

    // Sink-side dedup of last segments JSON (sink thread only)
    char *last_text;

    bool track_selected;
    int frames_with_meta;
    int subtitles_injected;
    int translations_injected;
    bool first_translation_failure_logged;
    uint64_t translation_cue_id;

    // ---- Hallucination & repetition filter (sink thread only) ----
    // Last few normalized texts we've already injected; used to drop whisper
    // duplicates so we don't burn AI tokens on them.
    char *recent_text_norm[16];
    int64_t recent_text_smin[16];
    int64_t recent_text_smax[16];
    int recent_text_count;
    int recent_text_head;

    // Captured at start; populated/refreshed by publish. Subtitle injection
    // target (sub_demuxer/sub_stream from snap captured at last publish).
    struct sh_stream *primary_stream;
    struct demuxer *primary_demuxer;
};

// ---------- Hallucination & dedup helpers ----------

#define WL_RECENT_TEXTS 16

// Normalize a whisper text for dedup/filter: trim, collapse spaces, strip a
// few common trailing/leading punctuation. Returned pointer is a talloc child
// of `parent`. Returns NULL if the text is "garbage" we should never inject
// (empty, whitespace only, music tags, etc.).
static char *wl_normalize_text(void *parent, const char *text)
{
    if (!text || !text[0])
        return NULL;
    // Skip leading whitespace.
    while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r')
        text++;
    if (!*text)
        return NULL;

    // Reject pure non-letter content (e.g. "...", "♪♪♪", "(music)", "[Music]").
    // Cheap heuristic: must contain at least one alphanumeric byte (>=0x30)
    // OR a UTF-8 leading byte (>=0xC0) for CJK/etc.
    bool has_real = false;
    for (const char *p = text; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c >= 0xC0)
        {
            has_real = true;
            break;
        }
    }
    if (!has_real)
        return NULL;

    // Whisper hallucination tags.
    static const char *tags[] = {
        "[Music]", "[music]", "(Music)", "(music)",
        "[Applause]", "[applause]",
        "[Laughter]", "[laughter]",
        "[ Silence ]", "[silence]",
        NULL,
    };
    for (int i = 0; tags[i]; i++) {
        if (strcmp(text, tags[i]) == 0)
            return NULL;
    }
    // Pure ♪ runs.
    bool only_music_glyphs = true;
    for (const char *p = text; *p; ) {
        if ((unsigned char)*p == 0xE2 && (unsigned char)*(p+1) == 0x99 &&
            ((unsigned char)*(p+2) == 0xAA || (unsigned char)*(p+2) == 0xAB))
        {
            p += 3;
            continue;
        }
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
            p++;
            continue;
        }
        only_music_glyphs = false;
        break;
    }
    if (only_music_glyphs)
        return NULL;

    // Build normalized copy: trim trailing whitespace, collapse internal runs.
    char *out = talloc_strdup(parent, text);
    size_t len = strlen(out);
    while (len > 0 && (out[len-1] == ' ' || out[len-1] == '\t' ||
                        out[len-1] == '\n' || out[len-1] == '\r'))
    {
        out[--len] = '\0';
    }
    if (len == 0) {
        talloc_free(out);
        return NULL;
    }
    // Collapse internal whitespace.
    char *src = out, *dst = out;
    bool prev_space = false;
    while (*src) {
        char c = *src++;
        if (c == '\t' || c == '\n' || c == '\r')
            c = ' ';
        if (c == ' ') {
            if (prev_space)
                continue;
            prev_space = true;
        } else {
            prev_space = false;
        }
        *dst++ = c;
    }
    *dst = '\0';
    return out;
}

// Returns true if (norm, s_ms, e_ms) was recently seen. Always records.
static bool wl_recent_seen(struct whisper_lookahead *wl,
                           const char *norm, int64_t s_ms, int64_t e_ms)
{
    for (int i = 0; i < wl->recent_text_count; i++) {
        if (!wl->recent_text_norm[i])
            continue;
        if (strcmp(wl->recent_text_norm[i], norm) == 0) {
            // Same text within ~5s window OR identical timestamps → duplicate.
            int64_t dmin = llabs(wl->recent_text_smin[i] - s_ms);
            int64_t dmax = llabs(wl->recent_text_smax[i] - e_ms);
            if (dmin < 5000 && dmax < 5000)
                return true;
            // Update window so subsequent identical texts continue to suppress.
            wl->recent_text_smin[i] = s_ms;
            wl->recent_text_smax[i] = e_ms;
            return false;
        }
    }
    int idx;
    if (wl->recent_text_count < WL_RECENT_TEXTS) {
        idx = wl->recent_text_count++;
    } else {
        idx = wl->recent_text_head;
        wl->recent_text_head = (wl->recent_text_head + 1) % WL_RECENT_TEXTS;
        talloc_free(wl->recent_text_norm[idx]);
    }
    wl->recent_text_norm[idx] = talloc_strdup(wl, norm);
    wl->recent_text_smin[idx] = s_ms;
    wl->recent_text_smax[idx] = e_ms;
    return false;
}

static void wl_recent_clear(struct whisper_lookahead *wl)
{
    for (int i = 0; i < WL_RECENT_TEXTS; i++) {
        talloc_free(wl->recent_text_norm[i]);
        wl->recent_text_norm[i] = NULL;
    }
    wl->recent_text_count = 0;
    wl->recent_text_head = 0;
}

// Subtitle duration clamp. whisper.cpp occasionally emits a segment whose
// duration spans most of the chunk (~30 s under some VAD-less fallback
// paths in af_whisper), so a single line can squat on screen long after
// later segments have already been transcribed and overlap it. Clamping
// at the inject site bounds the "stuck subtitle" symptom regardless of
// which producer or translator path led here. 10 s is below CHUNK_SECONDS
// (12 s), so any natural segment under normal segmentation passes through.
#define WT_MAX_SUBTITLE_DUR_S          10.0
static void wl_feed_subtitle_text(struct whisper_lookahead *wl,
                                  const char *body,
                                  double pts, double dur,
                                  const char *kind);

static void accept_translation_result(
    void *ctx, const struct mp_translation_result *result)
{
    struct whisper_lookahead *wl = ctx;
    if (result->translated) {
        char *body = talloc_asprintf(NULL,
            "{\\fs72\\c&H00FFFFFF&\\3c&H00000000&\\bord3}%s"
            "\\N{\\fs48\\c&H00E0FFFF&\\3c&H00000000&\\bord2}%s",
            result->translated, result->text);
        wl->translations_injected++;
        const char *tag =
            result->kind == MP_TRANSLATION_RESULT_REUSED
                ? "translated-reused" : "translated";
        MP_INFO(wl, "%s #%d: %.40s%s\n", tag,
                wl->translations_injected, result->translated,
                strlen(result->translated) > 40 ? "..." : "");
        wl_feed_subtitle_text(wl, body, result->pts,
                              result->duration, tag);
        talloc_free(body);
        wl->first_translation_failure_logged = false;
        return;
    }

    const char *tag = "original-after-translate-fail";
    switch (result->kind) {
    case MP_TRANSLATION_RESULT_FALLBACK_SHORT:
        tag = "original-too-short";
        break;
    case MP_TRANSLATION_RESULT_FALLBACK_LOOP:
        tag = "original-repeat-loop";
        break;
    case MP_TRANSLATION_RESULT_FALLBACK_RPM:
        tag = "original-rpm-cap";
        break;
    case MP_TRANSLATION_RESULT_FALLBACK_BUDGET:
        tag = "original-budget";
        break;
    case MP_TRANSLATION_RESULT_FALLBACK_QUEUE:
        tag = "original-queue-overflow";
        break;
    case MP_TRANSLATION_RESULT_FALLBACK_LATE:
        return;
    case MP_TRANSLATION_RESULT_FALLBACK_FAILURE:
    default:
        if (result->error && !wl->first_translation_failure_logged) {
            MP_WARN(wl, "translation failed: %s (showing original)\n",
                    result->error);
            wl->first_translation_failure_logged = true;
        }
        break;
    }
    wl_feed_subtitle_text(wl, result->text, result->pts,
                          result->duration, tag);
}

void whisper_lookahead_drain_results(struct MPContext *mpctx)
{
    struct whisper_lookahead *wl = mpctx->whisper_lookahead;
    if (!wl || !mpctx->translation ||
        !wl->primary_stream || !wl->primary_demuxer)
    {
        return;
    }
    mp_translation_drain(mpctx->translation,
                         MP_TRANSLATION_SOURCE_WHISPER,
                         accept_translation_result, wl);
}

// ---------- Subtitle injection ----------

// Build the ASS line for `body` and feed it as a subtitle packet for the
// currently published primary sub stream. Used by both the synchronous
// (no-translator) sink path and the asynchronous (translator drain) core
// path. Caller already filtered out NULL/empty text and validated that
// wl->primary_stream / primary_demuxer are non-NULL.
//
// `kind` is a short tag (no spaces) describing which path produced this
// subtitle. It is logged at INFO so the operator can tell from the log
// whether a given subtitle came from the translator, was a slack-skip
// fallback, or a translation failure.
static void wl_feed_subtitle_text(struct whisper_lookahead *wl,
                                  const char *body,
                                  double pts, double dur,
                                  const char *kind)
{
    if (!wl->primary_stream || !wl->primary_demuxer || !body || !body[0])
        return;

    // Clamp pathological per-segment durations from af_whisper. See
    // WT_MAX_SUBTITLE_DUR_S for rationale.
    bool dur_clamped = false;
    if (isfinite(dur) && dur > WT_MAX_SUBTITLE_DUR_S) {
        dur_clamped = true;
        dur = WT_MAX_SUBTITLE_DUR_S;
    } else if (!isfinite(dur) || dur < 0.0) {
        dur = WT_MAX_SUBTITLE_DUR_S;
        dur_clamped = true;
    }

    char *ass_line = talloc_asprintf(wl,
        "%d,0,Default,,0,0,0,,%s",
        wl->subtitles_injected, body);
    size_t ass_len = strlen(ass_line);
    struct demux_packet *dp = new_demux_packet_from(
        wl->primary_demuxer->packet_pool,
        (void *)ass_line, ass_len);
    if (dp) {
        dp->pts = pts;
        dp->dts = pts;
        dp->duration = dur;
        dp->sub_duration = dur;

        demuxer_feed_af_sub(wl->primary_stream, dp);
        wl->subtitles_injected++;
        MP_INFO(wl, "subtitle #%d @ %.3f (dur=%.1f%s) [%s]: %.40s%s\n",
                wl->subtitles_injected, pts, dur,
                dur_clamped ? ",clamped" : "",
                kind ? kind : "?",
                body, strlen(body) > 40 ? "..." : "");

        mp_wakeup_core(wl->mpctx);
    }
    talloc_free(ass_line);
}

// Inject a single subtitle with the given text, pts, and duration. PTS is in
// the player's timeline domain (seconds). V3.1 produces accurate PTS so no
// shift hack is required.
//
// `producer_gen` is the generation tag the worker carried when this segment
// was produced. `producer_graph_gen` is the corresponding recognition-graph
// generation. The two are split so that a translator-only invalidate (which
// bumps snap.generation but NOT snap.graph_generation) does not cause us to
// throw away recognition work that's still valid for the same audio:
//
//   - If producer_graph_gen != current snap.graph_generation → audio chain
//     was reset (seek / chain change) under us; the segment refers to audio
//     that is no longer relevant. Drop it.
//   - Otherwise the audio is still the live audio. Submit it to the shared
//     translation scheduler, whose source generation rejects later stale work.
//
// Three paths:
//   ① No translator → feed original synchronously here.
//   ⑤ Translator configured but slack too tight → feed original here too.
//   Otherwise → enqueue to the shared scheduler; drain feeds (translated or
//   original fallback) on the core thread.
static void inject_subtitle(struct whisper_lookahead *wl,
                            const char *text, double pts, double dur,
                            uint64_t producer_gen,
                            uint64_t producer_graph_gen)
{
    if (!wl->primary_stream || !wl->primary_demuxer || !text || !text[0])
        return;

    // Generation gate: only true graph resets (seek / audio-chain change)
    // make a recognized segment stale. Translator-only bumps leave the
    // audio intact and we want to keep the recognition work.
    uint64_t cur_graph_gen;
    mp_mutex_lock(&wl->snap_lock);
    cur_graph_gen = wl->snap.graph_generation;
    mp_mutex_unlock(&wl->snap_lock);
    if (producer_graph_gen != cur_graph_gen) {
        MP_INFO(wl, "drop stale segment (producer_graph_gen=%llu cur_graph_gen=%llu)\n",
                (unsigned long long)producer_graph_gen,
                (unsigned long long)cur_graph_gen);
        return;
    }
    (void)producer_gen;

    struct mp_translation *translation =
        mpctx_get_translation(wl->mpctx);
    enum mp_translation_submit_result submitted =
        mp_translation_submit(
            translation, MP_TRANSLATION_SOURCE_WHISPER,
            ++wl->translation_cue_id, 1, text, pts, dur,
            MP_TRANSLATION_FILTER_SHORT |
            MP_TRANSLATION_FILTER_REPEAT);
    if (submitted == MP_TRANSLATION_SUBMIT_QUEUED)
        return;

    const char *tag = submitted == MP_TRANSLATION_SUBMIT_TOO_LATE
        ? "original-slack-skip" : "original-no-translator";
    wl_feed_subtitle_text(wl, text, pts, dur, tag);
}

// Parse JSON segments array: [{"s":ms,"e":ms,"t":"text"}, ...]
// `pts_offset` is the timeline pts of the first audio sample fed to whisper
// in the current "session" (i.e. since last filter graph reset). Whisper's
// per-segment timestamps are relative to that origin.
static void process_whisper_segments(struct whisper_lookahead *wl,
                                     const char *json, double pts_offset,
                                     uint64_t producer_gen,
                                     uint64_t producer_graph_gen)
{
    if (!json || json[0] != '[')
        return;

    const char *p = json + 1;
    while (*p) {
        while (*p && *p != '{') {
            if (*p == ']') return;
            p++;
        }
        if (!*p) break;
        p++;

        int64_t s_ms = -1, e_ms = -1;
        char text_buf[4096] = {0};

        while (*p && *p != '}') {
            while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\r' || *p == '\t')
                p++;
            if (*p == '}') break;
            if (*p != '"') { p++; continue; }

            p++;
            char key = *p;
            while (*p && *p != ':') p++;
            if (!*p) break;
            p++;
            while (*p == ' ') p++;

            if (key == 's' || key == 'e') {
                char *end;
                int64_t val = strtoll(p, &end, 10);
                if (key == 's') s_ms = val;
                else            e_ms = val;
                p = end;
            } else if (key == 't') {
                if (*p != '"') { p++; continue; }
                p++;
                size_t ti = 0;
                while (*p && *p != '"' && ti < sizeof(text_buf) - 1) {
                    if (*p == '\\' && *(p + 1)) {
                        p++;
                        switch (*p) {
                        case '"':  text_buf[ti++] = '"'; break;
                        case '\\': text_buf[ti++] = '\\'; break;
                        case 'n':  text_buf[ti++] = '\n'; break;
                        case 'r':  text_buf[ti++] = '\r'; break;
                        case 't':  text_buf[ti++] = '\t'; break;
                        case 'u':
                            if (*(p+1) && *(p+2) && *(p+3) && *(p+4))
                                p += 4;
                            break;
                        default: text_buf[ti++] = *p; break;
                        }
                    } else {
                        text_buf[ti++] = *p;
                    }
                    p++;
                }
                text_buf[ti] = '\0';
                if (*p == '"') p++;
            }
        }
        if (*p == '}') p++;

        if (s_ms >= 0 && e_ms > s_ms && text_buf[0]) {
            // Whisper filter's segment timestamps come from the input frame
            // pts it sees. Since we feed mp_aframe with timeline pts (and
            // aresample is configured to preserve pts), s_ms / e_ms are
            // already in mpv's timeline domain — do NOT add the per-session
            // origin offset.
            (void)pts_offset;
            char *norm = wl_normalize_text(NULL, text_buf);
            if (!norm) {
                // Garbage / hallucination; skip.
                continue;
            }
            if (wl_recent_seen(wl, norm, s_ms, e_ms)) {
                talloc_free(norm);
                continue;
            }
            double pts = s_ms / 1000.0;
            double dur = (e_ms - s_ms) / 1000.0;
            inject_subtitle(wl, norm, pts, dur, producer_gen, producer_graph_gen);
            talloc_free(norm);
        }
    }
}

// ---------- Sink filter ----------

struct sink_priv {
    struct whisper_lookahead *wl;
    double session_origin_pts; // pts of first sample of current graph session
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
            MP_INFO(wl, "sink: EOF (with-meta=%d injected=%d)\n",
                    wl->frames_with_meta, wl->subtitles_injected);
            mp_frame_unref(&frame);
            return;
        }

        if (frame.type != MP_FRAME_AUDIO) {
            mp_frame_unref(&frame);
            continue;
        }

        struct mp_aframe *af = frame.data;

        // Latch session origin from the very first frame after a reset. The
        // graph's aresample first_pts settings normalize the timestamps to a
        // local 0-based domain; we add `session_origin_pts` back in
        // process_whisper_segments() to recover the player timeline pts.
        if (p->session_origin_pts == MP_NOPTS_VALUE) {
            double pts = mp_aframe_get_pts(af);
            // mp_aframe pts here is the *graph-input* pts we set on the
            // worker side — i.e. the timeline pts of the first decoded
            // sample. That's exactly the origin we need.
            if (isfinite(pts))
                p->session_origin_pts = pts;
        }

        AVFrame *avf = mp_aframe_get_raw_avframe(af);
        if (avf && avf->metadata) {
            if (wl->frames_with_meta == 0) {
                MP_INFO(wl, "sink: first frame with metadata, keys:\n");
                const AVDictionaryEntry *t = NULL;
                while ((t = av_dict_get(avf->metadata, "", t, AV_DICT_IGNORE_SUFFIX)))
                    MP_INFO(wl, "  '%s' = '%s'\n", t->key, t->value);
            }
            wl->frames_with_meta++;

            const AVDictionaryEntry *e =
                av_dict_get(avf->metadata, "lavfi.whisper.segments", NULL, 0);
            const char *segments_json = e ? e->value : NULL;
            if (segments_json && segments_json[0]) {
                bool changed = !wl->last_text ||
                               strcmp(wl->last_text, segments_json) != 0;
                if (changed) {
                    talloc_free(wl->last_text);
                    wl->last_text = talloc_strdup(wl, segments_json);
                    double origin = p->session_origin_pts != MP_NOPTS_VALUE
                                        ? p->session_origin_pts : 0;
                    process_whisper_segments(wl, segments_json, origin,
                                             wl->worker_generation,
                                             wl->worker_graph_generation);
                }
            }
        }

        mp_frame_unref(&frame);
    }
}

static void sink_reset(struct mp_filter *f)
{
    struct sink_priv *p = f->priv;
    p->session_origin_pts = MP_NOPTS_VALUE;
}

static const struct mp_filter_info sink_filter_info = {
    .name = "whisper_sink",
    .priv_size = sizeof(struct sink_priv),
    .process = sink_process,
    .reset = sink_reset,
};

// ---------- Source filter (queue → lavfi) ----------

struct wl_source_priv {
    struct whisper_lookahead *wl;
};

static void wl_source_process(struct mp_filter *f)
{
    struct wl_source_priv *sp = f->priv;
    struct whisper_lookahead *wl = sp->wl;

    if (!mp_pin_in_needs_data(f->ppins[0]))
        return;

    mp_mutex_lock(&wl->queue_lock);

    // If the worker requested a reset, propagate it to the entire graph and
    // discard everything queued under the old generation (the worker only
    // queues frames tagged with its own current generation, so anything
    // present here that doesn't match must be stale).
    bool need_reset = wl->reset_pending;
    wl->reset_pending = false;

    // Drop stale-generation queued items. The "current" generation here is
    // the snap.generation under snap_lock, but for a cheap check we accept
    // anything whose generation matches the head's freshly-tagged value
    // (worker bumps its own generation mirror before pushing).
    // Here we use a coarser rule: just trust queued items in FIFO order;
    // generation mismatches are handled at push time by clearing the queue.

    struct mp_frame frame = MP_NO_FRAME;
    if (wl->num_queue > 0) {
        frame = wl->queue[0].f;
        memmove(wl->queue, wl->queue + 1,
                (wl->num_queue - 1) * sizeof(*wl->queue));
        wl->num_queue--;
        if (frame.type == MP_FRAME_AUDIO)
            wl->queued_dur -= mp_aframe_duration(frame.data);
    }

    bool format_changed = false;
    if (frame.type == MP_FRAME_AUDIO) {
        struct mp_aframe *af = frame.data;
        int fmt = mp_aframe_get_format(af);
        int rate = mp_aframe_get_rate(af);
        if (wl->last_format && (fmt != wl->last_format || rate != wl->last_rate))
            format_changed = true;
        wl->last_format = fmt;
        wl->last_rate = rate;
    }

    // Wake the worker if it was blocked on the queue being full.
    mp_cond_broadcast(&wl->queue_cv);
    mp_mutex_unlock(&wl->queue_lock);

    if (need_reset || format_changed) {
        // Reset the entire root (incl. lavfi → recreates the whisper filter,
        // which clears its VAD state and per-segment counter).
        mp_filter_reset(wl->root_filter);
        // After a reset all the pending pin requests are gone; we need a
        // fresh wakeup so the framework re-runs us next graph-run pass.
        mp_filter_internal_mark_progress(f);
        if (frame.type != MP_FRAME_NONE)
            mp_frame_unref(&frame);
        return;
    }

    if (frame.type != MP_FRAME_NONE) {
        mp_pin_in_write(f->ppins[0], frame);
        mp_filter_internal_mark_progress(f);
    }
}

static const struct mp_filter_info wl_source_info = {
    .name = "whisper_src",
    .priv_size = sizeof(struct wl_source_priv),
    .process = wl_source_process,
};

// ---------- Decoder helpers ----------

static void hash16(uint8_t *out, const uint8_t *data, int size)
{
    memset(out, 0, 16);
    int n = size < 16 ? size : 16;
    if (data && n > 0)
        memcpy(out, data, n);
}

static void free_decoder(struct whisper_lookahead *wl)
{
    if (wl->avctx) {
        avcodec_free_context(&wl->avctx);
        wl->avctx = NULL;
    }
    wl->worker_codec_id = AV_CODEC_ID_NONE;
    wl->worker_extradata_size = 0;
    memset(wl->worker_extradata_hash, 0, sizeof(wl->worker_extradata_hash));
}

// Build a fresh AVCodecContext from `c` (an mp_codec_params snapshot).
static bool build_decoder(struct whisper_lookahead *wl,
                          const struct mp_codec_params *c)
{
    free_decoder(wl);
    if (!c || !c->codec)
        return false;

    AVCodecParameters *par = mp_codec_params_to_av(c);
    if (!par) {
        MP_ERR(wl, "decoder: mp_codec_params_to_av failed\n");
        return false;
    }

    const AVCodec *codec = avcodec_find_decoder(par->codec_id);
    if (!codec) {
        MP_ERR(wl, "decoder: no libavcodec decoder for codec id %d (%s)\n",
               par->codec_id, c->codec ? c->codec : "?");
        avcodec_parameters_free(&par);
        return false;
    }

    AVCodecContext *avctx = avcodec_alloc_context3(codec);
    if (!avctx) {
        avcodec_parameters_free(&par);
        return false;
    }

    if (avcodec_parameters_to_context(avctx, par) < 0) {
        MP_ERR(wl, "decoder: avcodec_parameters_to_context failed\n");
        avcodec_free_context(&avctx);
        avcodec_parameters_free(&par);
        return false;
    }

    wl->pkt_tb = mp_get_codec_timebase(c);
    avctx->pkt_timebase = wl->pkt_tb;
    avctx->thread_count = 1; // small audio decode; avoid extra threads
    avctx->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;
    avctx->err_recognition = AV_EF_CRCCHECK | AV_EF_BITSTREAM;

    if (avcodec_open2(avctx, codec, NULL) < 0) {
        MP_ERR(wl, "decoder: avcodec_open2 failed\n");
        avcodec_free_context(&avctx);
        avcodec_parameters_free(&par);
        return false;
    }
    avcodec_parameters_free(&par);

    wl->avctx = avctx;
    wl->worker_codec_id = codec->id;
    wl->worker_extradata_size = avctx->extradata_size;
    hash16(wl->worker_extradata_hash, avctx->extradata, avctx->extradata_size);

    MP_INFO(wl, "decoder: opened %s (tb=%d/%d, sr=%d, ch=%d)\n",
            codec->name, wl->pkt_tb.num, wl->pkt_tb.den,
            avctx->sample_rate, avctx->ch_layout.nb_channels);
    return true;
}

static bool codec_changed(struct whisper_lookahead *wl,
                          const struct wl_snap *snap)
{
    if (!wl->avctx)
        return true;
    if (snap->codec_id != wl->worker_codec_id)
        return true;
    if (snap->extradata_size != wl->worker_extradata_size)
        return true;
    if (memcmp(snap->extradata_hash, wl->worker_extradata_hash, 16) != 0)
        return true;
    return false;
}

// ---------- Visit callback (collects packets while demux lock is held) ----------

struct collect {
    struct demux_packet **pkts;
    int num;
    int max;            // soft cap; further packets are dropped to keep the
                        // demux lock duration bounded
    bool truncated;
    void *talloc_parent;
};

static void on_packet(void *ctx, struct demux_packet *dp)
{
    struct collect *c = ctx;
    if (c->num >= c->max) {
        c->truncated = true;
        talloc_free(dp);
        return;
    }
    talloc_steal(c->talloc_parent, dp);
    MP_TARRAY_APPEND(c->talloc_parent, c->pkts, c->num, dp);
}

// ---------- Worker: snapshot, visit, decode, push ----------

// Read snapshot under snap_lock. Returns false if not ready.
static bool worker_get_snap(struct whisper_lookahead *wl, struct wl_snap *out)
{
    mp_mutex_lock(&wl->snap_lock);
    *out = wl->snap;
    mp_mutex_unlock(&wl->snap_lock);
    return out->valid && out->demuxer && out->audio_sh;
}

// Wait for room in the queue. Wakes on consume / terminate / generation
// change. Returns false if the worker should bail (terminate or gen change).
static bool worker_wait_for_room(struct whisper_lookahead *wl, double need_dur)
{
    mp_mutex_lock(&wl->queue_lock);
    while (!atomic_load(&wl->terminate) &&
           (wl->num_queue + 1 > MAX_QUEUE_FRAMES ||
            wl->queued_dur + need_dur > MAX_QUEUE_SECONDS))
    {
        mp_cond_timedwait(&wl->queue_cv, &wl->queue_lock,
                          MP_TIME_MS_TO_NS(500));
        // Loop also breaks if generation moved (worker_loop checks below).
        // Don't drop the lock here unnecessarily.
        if (atomic_load(&wl->terminate))
            break;
    }
    bool ok = !atomic_load(&wl->terminate);
    mp_mutex_unlock(&wl->queue_lock);
    return ok;
}

static void worker_push_aframe(struct whisper_lookahead *wl,
                               struct mp_aframe *af, uint64_t gen)
{
    struct mp_frame f = { MP_FRAME_AUDIO, af };
    struct frame_item it = { f, gen };
    mp_mutex_lock(&wl->queue_lock);
    MP_TARRAY_APPEND(wl, wl->queue, wl->num_queue, it);
    wl->queued_dur += mp_aframe_duration(af);
    mp_mutex_unlock(&wl->queue_lock);
}

static void worker_signal_reset(struct whisper_lookahead *wl)
{
    mp_mutex_lock(&wl->queue_lock);
    // Drop everything already queued (stale post-reset).
    for (int i = 0; i < wl->num_queue; i++)
        mp_frame_unref(&wl->queue[i].f);
    wl->num_queue = 0;
    wl->queued_dur = 0;
    wl->last_format = 0;
    wl->last_rate = 0;
    wl->reset_pending = true;
    mp_cond_broadcast(&wl->queue_cv);
    mp_mutex_unlock(&wl->queue_lock);
    // Make sure the source filter actually gets re-scheduled.
    mp_filter_wakeup(wl->source);
    mp_dispatch_interrupt(wl->graph_dispatch);
}

static void worker_decode_packet(struct whisper_lookahead *wl,
                                 struct demux_packet *dp, uint64_t gen)
{
    AVPacket *avp = av_packet_alloc();
    if (!avp)
        return;
    mp_set_av_packet(avp, dp, &wl->pkt_tb);

    int err = avcodec_send_packet(wl->avctx, avp);
    mp_free_av_packet(&avp);
    if (err < 0 && err != AVERROR(EAGAIN)) {
        // Many partial-stream / mid-keyframe errors are recoverable for
        // audio. Log at debug level only; keep going.
        MP_DBG(wl, "decoder: send_packet err=%d (pts=%.3f)\n", err, dp->pts);
        return;
    }

    for (;;) {
        if (atomic_load(&wl->terminate))
            return;
        AVFrame *avf = av_frame_alloc();
        if (!avf)
            return;
        err = avcodec_receive_frame(wl->avctx, avf);
        if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) {
            av_frame_free(&avf);
            return;
        }
        if (err < 0) {
            MP_DBG(wl, "decoder: receive_frame err=%d\n", err);
            av_frame_free(&avf);
            return;
        }
        wl->frames_decoded++;

        int64_t avp_pts = avf->pts;
        // mp_aframe_from_avframe uses av_frame_ref internally, so the caller
        // still owns avf and must free it.
        struct mp_aframe *af = mp_aframe_from_avframe(avf);
        av_frame_free(&avf);
        if (!af)
            return;

        // mp_aframe_from_avframe does not propagate the timestamp; set it in
        // the player timeline domain. Fall back to interpolation if the
        // decoder dropped pts (some codecs do for the first few frames).
        double pts_dec = mp_pts_from_av(avp_pts, &wl->pkt_tb);
        if (!isfinite(pts_dec))
            pts_dec = wl->worker_next_pts;
        mp_aframe_set_pts(af, pts_dec);

        double end = mp_aframe_end_pts(af);
        if (isfinite(end))
            wl->worker_next_pts = end;

        // Backpressure (interruptible).
        double dur = mp_aframe_duration(af);
        if (!worker_wait_for_room(wl, isfinite(dur) ? dur : 0.02)) {
            talloc_free(af);
            return;
        }

        worker_push_aframe(wl, af, gen);
        wl->worker_graph_seen_audio = true;
        mp_filter_wakeup(wl->source);
        mp_filter_wakeup(wl->sink);
        mp_dispatch_interrupt(wl->graph_dispatch);
    }
}

static MP_THREAD_VOID wl_thread(void *ptr)
{
    struct whisper_lookahead *wl = ptr;
    mp_thread_set_name("whisper/la");

    MP_INFO(wl, "thread: started\n");
    wl->worker_last_done = MP_NOPTS_VALUE;
    wl->worker_last_dts = MP_NOPTS_VALUE;
    mp_mutex_lock(&wl->snap_lock);
    wl->pub_processed_end = MP_NOPTS_VALUE;
    mp_mutex_unlock(&wl->snap_lock);
    wl->worker_next_pts = MP_NOPTS_VALUE;
    wl->worker_generation = 0;
    wl->worker_graph_generation = 0;
    wl->worker_graph_seen_audio = false;

    while (!atomic_load(&wl->terminate)) {
        struct wl_snap snap;
        if (!worker_get_snap(wl, &snap)) {
            mp_dispatch_queue_process(wl->graph_dispatch, WORKER_TICK_SEC);
            // Still drive the graph (sink may have pending pulls from prior
            // pushes); cheap if there's nothing to do.
            while (mp_filter_graph_run(wl->root_filter)) {}
            continue;
        }

        bool gen_change = (snap.generation != wl->worker_generation);
        bool graph_change = (snap.graph_generation != wl->worker_graph_generation);
        // worker_generation == 0 / worker_graph_generation == 0 are initial
        // sentinels before any iteration ran (bump_*_generation() start at 1).
        // Likewise, worker_graph_seen_audio == false means the lavfi/whisper
        // graph has not yet consumed any audio (either we're at startup, or
        // a previous reset cleared it and no frames have been pushed since).
        // In both cases there is no stale graph state to discard, and firing
        // worker_signal_reset() would call mp_filter_reset(root_filter),
        // which destroys the af_whisper instance (free_graph) and forces a
        // second whisper_init_from_file + CUDA backend init on the next
        // graph run — wasting ~3 GB VRAM (the new context overlaps the
        // old one before being collected) and ~10 s of startup time. With
        // large-v3 on a 6 GB GPU this is enough to exhaust VRAM and stall
        // both video decoding and recognition.
        bool initial_start = (wl->worker_graph_generation == 0);
        bool graph_dirty = wl->worker_graph_seen_audio;
        bool codec_change = graph_change && !initial_start &&
                            codec_changed(wl, &snap);
        if (gen_change) {
            // Always adopt the new generation tag so frames/tasks the worker
            // produces from now on carry the up-to-date generation, even on
            // translator-only bumps that don't touch the graph.
            wl->worker_generation = snap.generation;
        }
        if (graph_change) {
            MP_INFO(wl, "graph gen change: %llu -> %llu (codec_change=%d, initial=%d, graph_dirty=%d)\n",
                    (unsigned long long)wl->worker_graph_generation,
                    (unsigned long long)snap.graph_generation,
                    (int)codec_change,
                    (int)initial_start,
                    (int)graph_dirty);
            wl->worker_graph_generation = snap.graph_generation;
            wl->worker_last_done = MP_NOPTS_VALUE;
            wl->worker_last_dts = MP_NOPTS_VALUE;
            wl->worker_next_pts = MP_NOPTS_VALUE;
            mp_mutex_lock(&wl->snap_lock);
            wl->pub_processed_end = MP_NOPTS_VALUE;
            mp_mutex_unlock(&wl->snap_lock);
            if (codec_change) {
                free_decoder(wl);
            } else if (wl->avctx) {
                avcodec_flush_buffers(wl->avctx);
            }
            // Skip the filter-graph reset when there's nothing to reset.
            // After a real reset the graph is empty again, so clear the
            // dirty flag too.
            if (graph_dirty) {
                worker_signal_reset(wl);
                wl->worker_graph_seen_audio = false;
            }
        } else if (gen_change) {
            // Translator-only bump: keep recognizer/graph intact. Just log
            // it so the timing is visible alongside the wt pipeline's
            // generation-mismatch drops.
            MP_INFO(wl, "translator gen change: -> %llu (graph kept)\n",
                    (unsigned long long)snap.generation);
        }

        if (!wl->avctx && !build_decoder(wl, snap.audio_sh->codec)) {
            // Couldn't build a decoder; back off and try again.
            mp_dispatch_queue_process(wl->graph_dispatch, 1.0);
            continue;
        }

        // Decide chunk window.
        double start, end;
        bool pb_known = snap.playback_pts != MP_NOPTS_VALUE &&
                        isfinite(snap.playback_pts);
        bool cs_known = snap.cache_start != MP_NOPTS_VALUE &&
                        isfinite(snap.cache_start);
        bool ce_known = snap.cache_end != MP_NOPTS_VALUE &&
                        isfinite(snap.cache_end);

        if (wl->worker_last_done == MP_NOPTS_VALUE) {
            // First chunk: prefer to anchor at playback (so the first whisper
            // window is centered on what the user is about to hear), but if
            // playback hasn't started latching pts yet (typical at file open,
            // before the AO has produced its first sample), fall back to
            // cache_start so we never block waiting for AO.
            double anchor;
            if (pb_known)
                anchor = snap.playback_pts;
            else if (cs_known)
                anchor = snap.cache_start;
            else {
                // Nothing actionable yet; sleep briefly and retry.
                while (mp_filter_graph_run(wl->root_filter)) {}
                mp_dispatch_queue_process(wl->graph_dispatch, WORKER_TICK_SEC);
                continue;
            }
            start = anchor - 1.0;
            if (cs_known && start < snap.cache_start)
                start = snap.cache_start;
            end = start + FIRST_CHUNK_SECONDS;
        } else {
            start = wl->worker_last_done;
            end = start + CHUNK_SECONDS;
        }

        /* If the cache window has moved past worker_last_done (e.g. the
         * demuxer hit EOF then was forced to seek to refill, or the cache
         * evicted old data while the worker was busy), the data we wanted
         * is no longer reachable.  Jump forward to cache_start so the
         * worker doesn't sit forever requesting an interval that the
         * demuxer can't return any packets for.  Reset the dts dedup
         * cursor too — the new region's dts values are unrelated to what
         * we last consumed. */
        if (cs_known && isfinite(start) && start < snap.cache_start) {
            MP_INFO(wl, "skip: worker_last_done=%.3f below cache_start=%.3f"
                        " (likely post-EOF cache refill); jumping forward\n",
                    start, snap.cache_start);
            start = snap.cache_start;
            end = start + (wl->worker_last_done == MP_NOPTS_VALUE
                           ? FIRST_CHUNK_SECONDS : CHUNK_SECONDS);
            wl->worker_last_dts = MP_NOPTS_VALUE;
        }

        // Cap end by what's actually cached and by the lookahead bound.
        // When an async translator pipeline is active with a finite
        // cost-guard horizon, tighten the cap so recognition doesn't run
        // far past what the translator will translate (see commentary
        // above LOOKAHEAD_MAX_SEC).
        if (ce_known && end > snap.cache_end)
            end = snap.cache_end;
        double effective_lookahead = LOOKAHEAD_MAX_SEC;
        struct mp_translation *translation =
            mpctx_get_translation(wl->mpctx);
        if (translation && mp_translation_has_backend(translation)) {
            struct mp_translation_limits limits;
            mp_translation_get_limits(translation, &limits);
            if (limits.enabled && limits.horizon_sec > 0) {
                int horizon_sec = limits.horizon_sec;
                double horizon_cap = horizon_sec * LOOKAHEAD_TRANSLATE_MARGIN;
                if (horizon_cap < LOOKAHEAD_MIN_TRANSLATE_SEC)
                    horizon_cap = LOOKAHEAD_MIN_TRANSLATE_SEC;
                if (horizon_cap < effective_lookahead)
                    effective_lookahead = horizon_cap;
            }
        }
        double cap = pb_known ? snap.playback_pts + effective_lookahead : INFINITY;
        if (end > cap) end = cap;

        if (!isfinite(start) || !isfinite(end) || end - start < MIN_CHUNK_SECONDS) {
            MP_DBG(wl, "chunk: idle (pb=%.3f cache=[%.3f,%.3f] last_done=%.3f"
                       " start=%.3f end=%.3f)\n",
                   snap.playback_pts, snap.cache_start, snap.cache_end,
                   wl->worker_last_done, start, end);
            /* While we have processed nothing yet, surface the same diagnostic
             * at INFO level (rate-limited to once every ~5 s) so we can tell
             * from a user's regular log why the worker is starving. */
            if (wl->chunks_visited == 0) {
                int64_t now = mp_time_ns();
                if (now - wl->last_starve_log_ns > 5LL * 1000 * 1000 * 1000) {
                    MP_INFO(wl, "starving: pb=%.3f cache=[%.3f,%.3f]"
                                " last_done=%.3f start=%.3f end=%.3f\n",
                            snap.playback_pts, snap.cache_start, snap.cache_end,
                            wl->worker_last_done, start, end);
                    wl->last_starve_log_ns = now;
                }
            }
            // Drive the graph in case the sink still has work.
            while (mp_filter_graph_run(wl->root_filter)) {}
            mp_dispatch_queue_process(wl->graph_dispatch, WORKER_TICK_SEC);
            continue;
        }
        MP_DBG(wl, "chunk: [%.3f,%.3f] pb=%.3f last_done=%.3f\n",
               start, end, snap.playback_pts, wl->worker_last_done);

        // Visit + collect.
        struct collect c = {
            .max = 8192,
            .talloc_parent = NULL,
        };
        c.talloc_parent = talloc_new(NULL);
        double actual_s = MP_NOPTS_VALUE, actual_e = MP_NOPTS_VALUE;
        bool any = demux_cache_visit_packets(snap.demuxer, snap.audio_sh,
                                             start, end,
                                             &actual_s, &actual_e,
                                             on_packet, &c);
        if (!any || c.num == 0) {
            talloc_free(c.talloc_parent);
            if (wl->chunks_visited == 0) {
                int64_t now = mp_time_ns();
                if (now - wl->last_starve_log_ns > 5LL * 1000 * 1000 * 1000) {
                    MP_INFO(wl, "starving: cache visit returned empty for"
                                " [%.3f,%.3f] (pb=%.3f cache=[%.3f,%.3f] any=%d num=%d)\n",
                            start, end, snap.playback_pts,
                            snap.cache_start, snap.cache_end,
                            (int)any, c.num);
                    wl->last_starve_log_ns = now;
                }
            }
            // Probably outraced cache; small wait.
            mp_dispatch_queue_process(wl->graph_dispatch, WORKER_TICK_SEC);
            continue;
        }
        wl->chunks_visited++;

        // Recheck graph generation: if a graph-resetting bump (seek /
        // audio-chain change) moved it while we were under demux lock,
        // skip processing this batch — the worker_loop top will replay.
        // Translator-only bumps (which move snap.generation but not
        // snap.graph_generation) leave recognition untouched and don't
        // need a replay.
        struct wl_snap snap2;
        worker_get_snap(wl, &snap2);
        if (snap2.graph_generation != wl->worker_graph_generation) {
            for (int i = 0; i < c.num; i++)
                talloc_free(c.pkts[i]);
            talloc_free(c.talloc_parent);
            continue;
        }

        // Decode each packet, dropping duplicates by dts (the visit always
        // restarts from a keyframe at-or-before `start`, which on a fresh
        // chunk continuation will overlap the tail of the previous one).
        for (int i = 0; i < c.num && !atomic_load(&wl->terminate); i++) {
            struct demux_packet *dp = c.pkts[i];
            wl->packets_visited++;
            double dts = dp->dts;
            if (!isfinite(dts))
                dts = dp->pts;
            if (wl->worker_last_dts != MP_NOPTS_VALUE &&
                isfinite(dts) && dts <= wl->worker_last_dts + 1e-6)
            {
                wl->packets_skipped++;
                continue;
            }
            if (isfinite(dts))
                wl->worker_last_dts = dts;

            worker_decode_packet(wl, dp, wl->worker_generation);
        }
        for (int i = 0; i < c.num; i++)
            talloc_free(c.pkts[i]);
        talloc_free(c.talloc_parent);

        if (isfinite(actual_e)) {
            wl->worker_last_done = actual_e;
            // Publish so whisper_lookahead_seek() can short-circuit small
            // refresh seeks that land inside processed territory.
            mp_mutex_lock(&wl->snap_lock);
            wl->pub_processed_end = actual_e;
            mp_mutex_unlock(&wl->snap_lock);
        }

        if (c.truncated) {
            MP_WARN(wl, "visit truncated at %d packets; advancing anyway\n",
                    c.num);
        }

        // Pump the filter graph after each chunk.
        while (mp_filter_graph_run(wl->root_filter)) {}
    }

    MP_INFO(wl, "thread: exiting (chunks=%d pkts=%d skip=%d frames=%d subs=%d)\n",
            wl->chunks_visited, wl->packets_visited, wl->packets_skipped,
            wl->frames_decoded, wl->subtitles_injected);
    free_decoder(wl);
    MP_THREAD_RETURN();
}

static void wakeup_lookahead(void *ctx)
{
    struct whisper_lookahead *wl = ctx;
    mp_dispatch_interrupt(wl->graph_dispatch);
}

static void onlock_lookahead(void *ctx)
{
    struct whisper_lookahead *wl = ctx;
    mp_filter_graph_interrupt(wl->root_filter);
}

// ---------- Init thread (heavy: build filter graph) ----------

static MP_THREAD_VOID init_thread_fn(void *ptr)
{
    struct whisper_lookahead *wl = ptr;
    mp_thread_set_name("whisper/init");

    MP_INFO(wl, "init: starting\n");

    wl->root_filter = mp_filter_create_root(wl->mpctx->global);
    if (!wl->root_filter) {
        MP_ERR(wl, "init: failed to create filter root\n");
        goto fail;
    }
    wl->graph_dispatch = mp_dispatch_create(wl);
    mp_filter_graph_set_wakeup_cb(wl->root_filter, wakeup_lookahead, wl);
    mp_dispatch_set_onlock_fn(wl->graph_dispatch, onlock_lookahead, wl);

    wl->source = mp_filter_create(wl->root_filter, &wl_source_info);
    if (!wl->source) {
        MP_ERR(wl, "init: failed to create source filter\n");
        goto fail;
    }
    ((struct wl_source_priv *)wl->source->priv)->wl = wl;
    mp_filter_add_pin(wl->source, MP_PIN_OUT, "out");

    // Parse whisper opts; pull translator opts out of the bag.
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
    MP_TARRAY_APPEND(wl, filter_opts, num_opts, NULL);

    MP_INFO(wl, "init: creating whisper lavfi with %d option pairs\n",
            (num_opts - 1) / 2);
    for (int i = 0; i + 1 < num_opts && filter_opts[i]; i += 2)
        MP_INFO(wl, "  %s = %s\n", filter_opts[i], filter_opts[i + 1]);

    // Build the chain as two single-filter mp_lavfi instances. This bypasses
    // libavfilter's graph parser entirely, so option values may freely
    // contain ':' (Windows paths), commas, brackets etc.
    char **resample_opts = NULL;
    int num_resample_opts = 0;
    MP_TARRAY_APPEND(wl, resample_opts, num_resample_opts, talloc_strdup(wl, "async"));
    MP_TARRAY_APPEND(wl, resample_opts, num_resample_opts, talloc_strdup(wl, "1"));
    MP_TARRAY_APPEND(wl, resample_opts, num_resample_opts, talloc_strdup(wl, "out_sample_rate"));
    MP_TARRAY_APPEND(wl, resample_opts, num_resample_opts, talloc_strdup(wl, "16000"));
    MP_TARRAY_APPEND(wl, resample_opts, num_resample_opts, talloc_strdup(wl, "out_sample_fmt"));
    MP_TARRAY_APPEND(wl, resample_opts, num_resample_opts, talloc_strdup(wl, "s16"));
    MP_TARRAY_APPEND(wl, resample_opts, num_resample_opts, talloc_strdup(wl, "out_chlayout"));
    MP_TARRAY_APPEND(wl, resample_opts, num_resample_opts, talloc_strdup(wl, "mono"));
    MP_TARRAY_APPEND(wl, resample_opts, num_resample_opts, NULL);

    wl->lavfi_resample= mp_lavfi_create_filter(wl->root_filter, MP_FRAME_AUDIO,
                                                true, NULL, NULL,
                                                "aresample", resample_opts);
    if (!wl->lavfi_resample) {
        MP_ERR(wl, "init: failed to create aresample filter\n");
        goto fail;
    }

    wl->lavfi = mp_lavfi_create_filter(wl->root_filter, MP_FRAME_AUDIO, true,
                                       NULL, NULL, "whisper", filter_opts);
    if (!wl->lavfi) {
        MP_ERR(wl, "init: failed to create whisper filter (is the whisper "
                   "filter available in your ffmpeg build?)\n");
        goto fail;
    }

    wl->sink = mp_filter_create(wl->root_filter, &sink_filter_info);
    if (!wl->sink) {
        MP_ERR(wl, "init: failed to create sink\n");
        goto fail;
    }
    ((struct sink_priv *)wl->sink->priv)->wl = wl;
    ((struct sink_priv *)wl->sink->priv)->session_origin_pts = MP_NOPTS_VALUE;
    mp_filter_add_pin(wl->sink, MP_PIN_IN, "in");

    // source -> aresample -> whisper -> sink
    mp_pin_connect(wl->lavfi_resample->f->pins[0], wl->source->pins[0]);
    mp_pin_connect(wl->lavfi->f->pins[0], wl->lavfi_resample->f->pins[1]);
    mp_pin_connect(wl->sink->pins[0], wl->lavfi->f->pins[1]);

    MP_INFO(wl, "init: pipeline connected\n");

    struct mp_translation *translation =
        mpctx_get_translation(wl->mpctx);
    if (!translation ||
        mp_translation_configure_legacy_whisper(
            translation, translate_provider,
            whisper_language ? whisper_language : "auto",
            translate_to, wl->mpctx->whisper_ai_translate_json,
            wl->mpctx->whisper_translate_limits_json) < 0)
    {
        MP_WARN(wl, "init: translation scheduler unavailable\n");
    }

    if (mp_thread_create(&wl->thread, wl_thread, wl)) {
        MP_ERR(wl, "init: failed to create worker thread\n");
        goto fail;
    }
    wl->thread_valid = true;

    wl->init_ok = true;
    MP_INFO(wl, "init: pipeline started\n");
    atomic_store(&wl->init_done, true);
    mp_wakeup_core(wl->mpctx);
    MP_THREAD_RETURN();

fail:
    if (wl->root_filter) {
        talloc_free(wl->root_filter);
        wl->root_filter = NULL;
    }
    wl->init_ok = false;
    atomic_store(&wl->init_done, true);
    mp_wakeup_core(wl->mpctx);
    MP_THREAD_RETURN();
}

// ---------- Snapshot publication (called from the playloop) ----------

// Snapshot codec identity from the live sh_stream (sh_stream pointers are
// stable for the demuxer's lifetime, but the underlying codec params are
// effectively immutable for the duration of a track being selected, so this
// is fine to read off-lock from the playloop).
static void snap_set_codec(struct wl_snap *s, struct sh_stream *sh)
{
    s->codec_id = AV_CODEC_ID_NONE;
    s->extradata_size = 0;
    memset(s->extradata_hash, 0, 16);
    if (!sh || !sh->codec || !sh->codec->codec)
        return;
    s->codec_id = mp_codec_to_av_codec_id(sh->codec->codec);
    s->extradata_size = sh->codec->extradata_size;
    hash16(s->extradata_hash, sh->codec->extradata, sh->codec->extradata_size);
}

void whisper_lookahead_publish(struct MPContext *mpctx)
{
    struct whisper_lookahead *wl = mpctx->whisper_lookahead;
    if (!wl)
        return;

    struct wl_snap s = { 0 };
    if (mpctx->ao_chain && mpctx->ao_chain->track &&
        mpctx->ao_chain->track->stream &&
        mpctx->ao_chain->track->demuxer)
    {
        struct track *t = mpctx->ao_chain->track;
        s.demuxer = t->demuxer;
        s.audio_sh = t->stream;
        s.sub_demuxer = t->demuxer;
        s.sub_stream = t->stream;
        s.playback_pts = mpctx->playback_pts;
        snap_set_codec(&s, t->stream);

        struct demux_reader_state rs;
        demux_get_reader_state(t->demuxer, &rs);
        s.cache_start = rs.ts_per_stream[STREAM_AUDIO].reader;
        s.cache_end = rs.ts_per_stream[STREAM_AUDIO].end;
        if (!isfinite(s.cache_start)) s.cache_start = rs.ts_info.reader;
        if (!isfinite(s.cache_end))   s.cache_end   = rs.ts_info.end;

        s.valid = true;
    }

    mp_mutex_lock(&wl->snap_lock);
    s.generation = wl->snap.generation; // preserved
    s.graph_generation = wl->snap.graph_generation; // preserved
    wl->snap = s;
    // Keep injection pointers in sync with the published snapshot.
    wl->primary_stream = s.sub_stream;
    wl->primary_demuxer = s.sub_demuxer;
    mp_mutex_unlock(&wl->snap_lock);

    if (mpctx->translation)
        mp_translation_set_playback_pts(mpctx->translation, s.playback_pts);

    if (wl->graph_dispatch)
        mp_dispatch_interrupt(wl->graph_dispatch);
    mp_mutex_lock(&wl->queue_lock);
    mp_cond_broadcast(&wl->queue_cv);
    mp_mutex_unlock(&wl->queue_lock);
}

// Bump generation. `reset_graph` controls whether the recognition filter
// graph is also invalidated:
//   - true  (seek, audio-chain change): the worker tears down af_whisper
//           and the audio decoder, drops its queue, and starts fresh.
//   - false (translator-only invalidate): only snap.generation moves so
//           the shared scheduler drops in-flight translations / pending tasks
//           tied to the old translator config; the worker keeps decoding
//           and recognizing without touching af_whisper. This avoids
//           burning ~3 GB VRAM and ~10 s of CUDA reinit on large-v3 just
//           because the user toggled an AI-translator option.
static void bump_generation_ex(struct whisper_lookahead *wl, bool reset_graph)
{
    mp_mutex_lock(&wl->snap_lock);
    wl->snap.generation++;
    if (reset_graph)
        wl->snap.graph_generation++;
    mp_mutex_unlock(&wl->snap_lock);

    if (wl->mpctx->translation) {
        mp_translation_invalidate_source(
            wl->mpctx->translation, MP_TRANSLATION_SOURCE_WHISPER);
    }

    // Wake the worker out of any wait it might be in.
    if (wl->graph_dispatch)
        mp_dispatch_interrupt(wl->graph_dispatch);
    mp_mutex_lock(&wl->queue_lock);
    mp_cond_broadcast(&wl->queue_cv);
    mp_mutex_unlock(&wl->queue_lock);
}

// Convenience wrappers. Call sites should pick the right semantic.
static void bump_generation(struct whisper_lookahead *wl)
{
    bump_generation_ex(wl, /*reset_graph*/ true);
}

static void bump_generation_translate_only(struct whisper_lookahead *wl)
{
    bump_generation_ex(wl, /*reset_graph*/ false);
}

// ---------- Public API ----------

void whisper_lookahead_start(struct MPContext *mpctx, const char *whisper_opts)
{
    whisper_lookahead_stop(mpctx);

    if (!mpctx->ao_chain || !mpctx->ao_chain->track ||
        !mpctx->ao_chain->track->stream || !mpctx->ao_chain->track->demuxer)
    {
        MP_ERR(mpctx, "whisper lookahead: no audio track available\n");
        return;
    }

    MP_INFO(mpctx, "whisper lookahead: starting (cache visit, V3.1), opts='%s'\n",
            whisper_opts ? whisper_opts : "");

    struct whisper_lookahead *wl = talloc_zero(NULL, struct whisper_lookahead);
    wl->mpctx = mpctx;
    wl->log = mp_log_new(wl, mpctx->log, "whisper-la");
    wl->whisper_opts = talloc_strdup(wl, whisper_opts ? whisper_opts : "");
    wl->snap.generation = 1;
    wl->snap.graph_generation = 1;
    wl->pub_processed_end = MP_NOPTS_VALUE;
    atomic_store(&wl->init_done, false);
    atomic_store(&wl->terminate, 0);

    mp_mutex_init(&wl->snap_lock);
    mp_mutex_init(&wl->queue_lock);
    mp_cond_init(&wl->queue_cv);

    mpctx->whisper_lookahead = wl;

    // Publish initial snapshot so the worker has something to do as soon as
    // the init thread finishes.
    whisper_lookahead_publish(mpctx);

    if (mp_thread_create(&wl->init_thread, init_thread_fn, wl)) {
        MP_ERR(mpctx, "whisper lookahead: failed to create init thread\n");
        mp_mutex_destroy(&wl->queue_lock);
        mp_mutex_destroy(&wl->snap_lock);
        mp_cond_destroy(&wl->queue_cv);
        mpctx->whisper_lookahead = NULL;
        talloc_free(wl);
        return;
    }
    wl->init_thread_valid = true;
}

void whisper_lookahead_stop(struct MPContext *mpctx)
{
    struct whisper_lookahead *wl = mpctx->whisper_lookahead;
    if (!wl)
        return;

    MP_INFO(mpctx, "whisper lookahead: stopping (chunks=%d pkts=%d frames=%d "
                   "subs=%d trans=%d)\n",
            wl->chunks_visited, wl->packets_visited, wl->frames_decoded,
            wl->subtitles_injected, wl->translations_injected);

    if (wl->init_thread_valid)
        mp_thread_join(wl->init_thread);
    wl->init_thread_valid = false;

    atomic_store(&wl->terminate, 1);
    if (wl->root_filter)
        mp_filter_graph_interrupt(wl->root_filter);
    if (wl->graph_dispatch)
        mp_dispatch_interrupt(wl->graph_dispatch);
    mp_mutex_lock(&wl->queue_lock);
    mp_cond_broadcast(&wl->queue_cv);
    mp_mutex_unlock(&wl->queue_lock);

    if (wl->thread_valid)
        mp_thread_join(wl->thread);
    wl->thread_valid = false;

    mp_translation_stop_legacy_whisper(mpctx->translation);

    // Purge any captions previously fed into the af_sub virtual sub stream
    // and reset the dec_sub renderer cache. Without this, layer-3 stale
    // subtitles (already enqueued for future PTS) keep showing for ~30s
    // after stop / lang restart / disable. Done now that worker threads
    // are joined so no new packets can land in between.
    if (wl->primary_stream)
        demux_clear_af_sub_queue(wl->primary_stream);
    reset_whisper_subtitle_track(mpctx);

    if (wl->root_filter) {
        talloc_free(wl->root_filter);
        wl->root_filter = NULL;
    }

    for (int i = 0; i < wl->num_queue; i++)
        mp_frame_unref(&wl->queue[i].f);
    wl->num_queue = 0;

    mp_cond_destroy(&wl->queue_cv);
    mp_mutex_destroy(&wl->queue_lock);
    mp_mutex_destroy(&wl->snap_lock);

    mpctx->whisper_lookahead = NULL;
    talloc_free(wl);

    MP_INFO(mpctx, "whisper lookahead: stopped\n");
}

void whisper_lookahead_seek(struct MPContext *mpctx, double pts)
{
    struct whisper_lookahead *wl = mpctx->whisper_lookahead;
    if (!wl)
        return;

    // Player issues a seek for many reasons besides user scrubbing:
    // refresh seeks for A-V resync, the demuxer's "adjust seek target"
    // keyframe alignment (a few seconds backward), subtitle track
    // changes, etc.  These can fire several times per minute during
    // normal playback.  Bumping the generation on every one of them
    // throws away the in-flight ~30s whisper inference and forces a
    // restart, which under fast playback or short chunks means
    // subtitles never catch up.
    //
    // Heuristic: if the seek lands inside the region the worker has
    // already processed, the published subtitles for that range are
    // still valid and the worker is currently working *ahead* of the
    // new playback position — there is nothing to throw away. Skip the
    // bump and let the worker keep going.  Only bump for seeks that
    // jump beyond what we've processed (or backwards far enough to
    // leave the processed region).
    double processed = MP_NOPTS_VALUE;
    mp_mutex_lock(&wl->snap_lock);
    processed = wl->pub_processed_end;
    mp_mutex_unlock(&wl->snap_lock);

    // Tolerance: allow the seek target to be slightly past the last
    // processed end (e.g. into the chunk currently in flight).  Keep it
    // small so a real forward jump still triggers a reset.
    const double FORWARD_TOLERANCE = 2.0;

    if (pts != MP_NOPTS_VALUE && processed != MP_NOPTS_VALUE &&
        isfinite(pts) && isfinite(processed) &&
        pts <= processed + FORWARD_TOLERANCE)
    {
        MP_INFO(wl, "seek to %.3f within processed region "
                    "(processed_end=%.3f); keeping in-flight chunk\n",
                pts, processed);
        return;
    }

    // Bump the generation so the worker drops its in-flight chunk, flushes
    // the decoder, resets the lavfi graph (recreating whisper → clearing its
    // VAD), and starts a fresh window from the new playback position.
    bump_generation(wl);

    // Soft refresh seeks (A-V resync, keyframe alignment, etc.) don't run
    // through mpv's reset_subtitle_state path, so they leave layer-3 stale
    // captions in the af_sub queue. Hard seeks already clear the demuxer
    // queues but cost nothing extra here. Only runs on the bump branch
    // (early-return above keeps in-flight chunks for in-region seeks).
    if (wl->primary_stream)
        demux_clear_af_sub_queue(wl->primary_stream);
    reset_whisper_subtitle_track(mpctx);
}

void whisper_lookahead_on_audio_chain_changed(struct MPContext *mpctx)
{
    struct whisper_lookahead *wl = mpctx->whisper_lookahead;
    if (!wl)
        return;

    // Capture the OLD audio stream before publish swaps it. Its af_sub queue
    // still holds captions tied to the previous audio chain; flush them
    // before they end up on the wrong stream's display.
    struct sh_stream *old_audio_sh = NULL;
    mp_mutex_lock(&wl->snap_lock);
    old_audio_sh = wl->primary_stream;
    mp_mutex_unlock(&wl->snap_lock);

    // Bump first, THEN publish: any drain that wakes during this window
    // sees a generation that does not match in-flight tasks, so old
    // translations cannot leak onto the new audio chain.
    bump_generation(wl);
    if (old_audio_sh)
        demux_clear_af_sub_queue(old_audio_sh);
    reset_whisper_subtitle_track(mpctx);
    whisper_lookahead_publish(mpctx);
}

// Force-purge whisper subtitles already published / queued (layers 1, 2, and
// 3 in the lookahead pipeline) without tearing down the recognizer. Use when
// the audio stream is unchanged but the *content* of previously-emitted
// captions is now considered invalid (e.g. user changed translator config).
void whisper_lookahead_invalidate(struct MPContext *mpctx, const char *reason)
{
    struct whisper_lookahead *wl = mpctx->whisper_lookahead;
    if (!wl)
        return;

    MP_INFO(wl, "invalidate: %s\n", reason ? reason : "(no reason)");

    // Layer 1+2: bump translator generation only — any in-flight worker
    // output (whisper segment about to inject_subtitle, or translation result
    // waiting for drain) gets dropped on its way out. Critically we do
    // NOT bump graph_generation here: the audio stream / recognized text
    // are still valid, and a graph reset would force af_whisper to
    // re-init (~3 GB VRAM + ~10 s CUDA reload on large-v3) just because
    // the user toggled an AI-translator option.
    bump_generation_translate_only(wl);

    // Layer 3: drop already-published ASS packets that are sitting in the
    // af_sub demuxer queue + reset the dec_sub renderer cache so currently
    // displayed text disappears.
    struct sh_stream *audio_sh = NULL;
    mp_mutex_lock(&wl->snap_lock);
    audio_sh = wl->primary_stream;
    mp_mutex_unlock(&wl->snap_lock);
    if (audio_sh)
        demux_clear_af_sub_queue(audio_sh);
    reset_whisper_subtitle_track(mpctx);
}

bool whisper_lookahead_opts_match(struct MPContext *mpctx, const char *opts)
{
    struct whisper_lookahead *wl = mpctx->whisper_lookahead;
    if (!wl)
        return false;
    const char *cur = wl->whisper_opts ? wl->whisper_opts : "";
    const char *cmp = opts ? opts : "";
    return strcmp(cur, cmp) == 0;
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

// ---------- AI translate property bridge ----------

void whisper_lookahead_set_ai_translate(struct MPContext *mpctx,
                                        const char *json)
{
    talloc_free(mpctx->whisper_ai_translate_json);
    mpctx->whisper_ai_translate_json =
        (json && json[0]) ? talloc_strdup(mpctx, json) : NULL;

    struct whisper_lookahead *wl = mpctx->whisper_lookahead;
    if (!wl || !atomic_load(&wl->init_done))
        return;
    struct mp_translation *translation = mpctx_get_translation(mpctx);
    if (mp_translation_set_legacy_ai(translation, json)) {
        wl_recent_clear(wl);
        whisper_lookahead_invalidate(mpctx, "ai-translate-changed");
    }
}

char *whisper_lookahead_get_ai_translate_status(struct MPContext *mpctx,
                                                void *ta_parent)
{
    return mp_translation_get_legacy_status(
        mpctx->translation, ta_parent);
}

int whisper_lookahead_set_translate_limits(struct MPContext *mpctx,
                                           const char *json_limits)
{
    struct mp_translation *translation = mpctx_get_translation(mpctx);
    int result = mp_translation_set_legacy_limits(
        translation, json_limits);
    mp_wakeup_core(mpctx);
    return result;
}
