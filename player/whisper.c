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
 *     wt pipeline drops in-flight translation tasks/results, but the
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
#include "whisper_translate.h"

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
#define FIRST_CHUNK_SECONDS  6.0
#define CHUNK_SECONDS       12.0
#define MIN_CHUNK_SECONDS    1.5
#define LOOKAHEAD_MAX_SEC  120.0
#define WORKER_TICK_SEC      0.05  // tighter wakeups during startup ramp-up

struct frame_item {
    struct mp_frame f;
    uint64_t generation;
};

struct wt_pipeline;

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
    // translator + history are accessed by both the sink (worker) thread and
    // the core thread (when the AI translator config is changed via the
    // whisper-ai-translate property). Hold translator_lock for the entire
    // translate call (so config swap waits for in-flight HTTP).
    mp_mutex translator_lock;
    struct whisper_translator *translator;

    // Async AI translation pipeline. Lives for the entire whisper_lookahead
    // lifetime; workers idle when wl->translator is NULL. Created in
    // whisper_lookahead_start(), destroyed in whisper_lookahead_stop().
    struct wt_pipeline *pipeline;

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

// ---------- AI translator config helpers ----------

// Build a wt_openai_config from JSON. Returns true on success and populates
// out_cfg whose strings are talloc children of `parent`.
static bool wl_parse_ai_translate_json(void *parent, struct mp_log *log,
                                       const char *json,
                                       struct wt_openai_config *out_cfg)
{
    memset(out_cfg, 0, sizeof(*out_cfg));
    if (!json || !json[0])
        return false;

    void *tmp = talloc_new(NULL);
    char *src = talloc_strdup(tmp, json);
    char *cursor = src;
    struct mpv_node root = {0};
    if (json_parse(tmp, &root, &cursor, MAX_JSON_DEPTH) < 0 ||
        root.format != MPV_FORMAT_NODE_MAP)
    {
        mp_warn(log, "whisper-ai-translate: invalid JSON\n");
        talloc_free(tmp);
        return false;
    }

    struct mpv_node *n;
    #define GET_STR(key) ( \
        (n = node_map_get(&root, key)) && n->format == MPV_FORMAT_STRING \
            ? talloc_strdup(parent, n->u.string) : NULL )
    #define GET_INT(key, defv) ( \
        (n = node_map_get(&root, key)) && n->format == MPV_FORMAT_INT64 \
            ? (int)n->u.int64 : (defv) )

    out_cfg->endpoint      = GET_STR("endpoint");
    out_cfg->model         = GET_STR("model");
    out_cfg->api_key       = GET_STR("api_key");
    out_cfg->source_lang   = GET_STR("source_lang");
    out_cfg->target_lang   = GET_STR("target_lang");
    out_cfg->system_prompt = GET_STR("system_prompt");
    out_cfg->context_size  = GET_INT("context_size", 0);
    out_cfg->timeout_ms    = GET_INT("timeout_ms", 0);
    out_cfg->max_tokens    = GET_INT("max_tokens", -1);

    #undef GET_STR
    #undef GET_INT

    talloc_free(tmp);
    return out_cfg->endpoint && out_cfg->model && out_cfg->target_lang;
}

// Try to (re)create the OpenAI translator from the configured JSON. Must be
// called with translator_lock held. Returns true if a translator was set.
// On disable (NULL/empty json), the existing translator is destroyed.
static bool wl_apply_ai_translator_locked(struct whisper_lookahead *wl,
                                          const char *json)
{
    if (wl->translator)
        whisper_translator_destroy(&wl->translator);
    wl_recent_clear(wl);

    if (!json || !json[0])
        return false;

    void *tmp = talloc_new(NULL);
    struct wt_openai_config cfg;
    if (!wl_parse_ai_translate_json(tmp, wl->log, json, &cfg)) {
        MP_WARN(wl, "whisper-ai-translate: missing endpoint/model/target_lang\n");
        talloc_free(tmp);
        return false;
    }
    wl->translator = whisper_translator_create_openai(wl, wl->log, &cfg);
    talloc_free(tmp);
    return wl->translator != NULL;
}



// ---------- Async AI translation pipeline ----------
//
// Background:
//   `inject_subtitle()` runs in the lookahead worker (sink filter) thread.
//   With AI translators (OpenAI-compatible), each translation is a 2–5s
//   HTTP round-trip. Doing it inline serializes whisper segments behind
//   network IO, so by the time a translated subtitle reaches the demuxer
//   (`pts + sub_duration < playback_pts`), `dec_sub` silently drops it
//   (sub/dec_sub.c:306). The user sees no subtitle while the log shows
//   "translated #N: ...".
//
// Design:
//   - Sink thread enqueues a task (text + pts + dur + generation snapshot)
//     into wt_pipeline.pending, instead of calling the translator inline.
//   - N (default 3) worker threads pop tasks and run whisper_translate_call.
//   - Workers push results into wt_pipeline.results and wake the core.
//   - The playloop calls whisper_lookahead_drain_results() each tick. The
//     core thread is the *only* writer of subtitle packets; this avoids
//     racing with seek/audio-chain-change.
//   - Generation gating: each task records the current generation at enqueue.
//     If the generation changes (seek / audio chain change / translator
//     swap), drained results with stale generations are dropped.
//
// Fallback / failure model:
//   ① No translator                     → sink thread feeds the original
//                                         text synchronously (legacy path).
//   ② Translation succeeds, on-time     → drain feeds bilingual ASS line.
//   ③ Translation succeeds, but late
//      (pts + dur + EPSILON < now)      → dropped (timing must remain
//                                         accurate; partial subtitles
//                                         disrupt viewing more than missing
//                                         ones).
//   ④ Translation fails / backoff       → drain feeds original text. The
//                                         first error message is logged at
//                                         WARN level; subsequent errors are
//                                         counted but suppressed until a
//                                         success resets the marker.
//   ⑤ Slack < MIN_TRANSLATE_SLACK at
//      enqueue time                     → sink feeds original immediately
//                                         (cheap; avoids burning API calls
//                                         on subtitles that are already too
//                                         close to playback to land in time).
//   ⑥ Result queue overflow             → push the *most-future* pending
//                                         task back as an original-fallback
//                                         result (we never drop the
//                                         soonest-needed task, that one is
//                                         the most likely to actually get
//                                         displayed in time).

#define WT_WORKERS_DEFAULT      3
#define WT_WORKERS_MAX          8
// Pending queue cap. The recognition side (af_whisper) on a fast GPU can
// race far ahead of playback (limited only by the demuxer cache, often a
// few minutes' worth of audio). Tasks beyond `horizon_sec` simply sit in
// pending until the playback cursor catches up, so the cap mainly serves as
// a memory bound. 1024 covers the common case (~10 min of typical 3-5 s
// segments) with comfortable headroom; far-future cleanup below kicks in
// before we ever reach this hard limit.
#define WT_PEND_MAX             1024
// (Removed: WT_PEND_HIGH_WATER + WT_FAR_FUTURE_HORIZON_MULT used to drive a
// silent high-water cleanup in wt_enqueue. That path was retired because
// (a) the worker's non-blocking horizon scan now leaves far-future tasks in
// pending until playback advances, and (b) demoting them to fallback would
// have injected future-PTS subtitle packets ahead of upcoming earlier
// translations and made the out-of-order drop problem worse.)
#define WT_RES_MAX              128
#define MIN_TRANSLATE_SLACK_S   1.0
#define WT_DRAIN_EPSILON_S      0.05

// Cost-protection defaults (apply when limits.enabled and the per-field value
// is non-zero). Tuned for AI translation; legacy google/azure paths share the
// same thresholds because the C# settings layer pre-fills wider defaults for
// those providers before pushing the JSON down.
#define WT_DEFAULT_HORIZON_SEC          60
#define WT_DEFAULT_SEEK_DEBOUNCE_MS     1500
#define WT_DEFAULT_MIN_TEXT_CHARS        2
#define WT_DEFAULT_REUSE_CACHE_CAP      256
#define WT_DEFAULT_REUSE_WINDOW_MS    120000
#define WT_DEFAULT_REPEAT_THRESHOLD       5
#define WT_DEFAULT_REPEAT_WINDOW_MS   30000
#define WT_DEFAULT_RPM_LIMIT              0   // 0 = disabled
#define WT_DEFAULT_SESSION_LIMIT          0   // 0 = unlimited

#define WT_REUSE_CACHE_HARD_CAP         512   // safety bound, regardless of cfg

#define WT_DEFER_SLEEP_MS               200

// Subtitle duration clamp. whisper.cpp occasionally emits a segment whose
// duration spans most of the chunk (~30 s under some VAD-less fallback
// paths in af_whisper), so a single line can squat on screen long after
// later segments have already been transcribed and overlap it. Clamping
// at the inject site bounds the "stuck subtitle" symptom regardless of
// which producer or translator path led here. 10 s is below CHUNK_SECONDS
// (12 s), so any natural segment under normal segmentation passes through.
#define WT_MAX_SUBTITLE_DUR_S          10.0

// Outcome of one task as the worker chose to handle it. Drain uses this to
// pick the right user-visible path (bilingual ASS vs. original-text fallback)
// and to keep the WARN-once "translation failed" log limited to genuine
// provider failures.
enum wt_result_kind {
    WT_RESULT_TRANSLATED = 0,    // success: HTTP returned a translation
    WT_RESULT_REUSED,            // cache hit: reused a prior translation
    WT_RESULT_FALLBACK_FAILURE,  // HTTP/provider failure or no translator
    WT_RESULT_FALLBACK_SHORT,    // text too short, skipped translation
    WT_RESULT_FALLBACK_LOOP,     // hallucination loop detected
    WT_RESULT_FALLBACK_RPM,      // rate-limit / would-miss-deadline
    WT_RESULT_FALLBACK_BUDGET,   // session budget exhausted
    WT_RESULT_FALLBACK_QUEUE,    // queue overflow eviction
};

struct wt_task {
    uint64_t generation;
    int      seq;
    double   pts;
    double   dur;
    char    *text;          // talloc child of task
};

struct wt_result {
    uint64_t generation;
    int      seq;
    double   pts;
    double   dur;
    char    *text;          // original (talloc child of result)
    char    *translated;    // success/reused: talloc child; otherwise NULL
    bool    rate_limited;
    char    *error_brief;   // first-failure-only WARN payload (or NULL)
    enum wt_result_kind kind;
};

// Cost-protection limits, loaded from `whisper-translate-limits` JSON. Treat
// 0/negative on integer fields as "disabled" unless otherwise noted.
struct wt_limits {
    bool enabled;
    int  horizon_sec;             // skip translation for tasks > horizon_sec
                                   // ahead of playback_pts (they ride along
                                   // as future re-pops; on file close they
                                   // stay un-translated → no cost spent)
    int  seek_debounce_ms;        // wall-clock quiet window after generation
                                   // bump before workers start translating
    int  min_text_chars;          // minimum UTF-8 char count to translate
    int  reuse_cache_capacity;    // LRU size for translation reuse cache
    int  reuse_cache_window_ms;   // cache entries older than this are stale
    int  repeat_loop_threshold;   // same-text hits within window → loop
    int  repeat_loop_window_ms;
    int  rpm_limit;               // requests/minute token-bucket cap
                                   // (0 disables bucket, default)
    int  session_request_limit;   // total HTTP calls per pipeline lifetime
                                   // (0 = unlimited)
};

// LRU cache entry for translation reuse / repeat-loop detection. `head` of
// `wt_pipeline.cache` is the most-recently used entry.
struct wt_cache_entry {
    char    *norm_text;
    char    *translated;          // may be NULL while we still need the
                                   // entry only for repeat-loop tracking
    int64_t  inserted_wall_ms;    // when translated was last refreshed
    int64_t  first_seen_wall_ms;  // when we started counting hits
    int      hit_count;           // hits inside [first_seen_wall_ms,
                                   //              first_seen_wall_ms+window]
};

struct wt_pipeline {
    struct whisper_lookahead *wl;   // back ref (no ownership)

    // ---- pending queue (sink producer; workers consumer) ----
    mp_mutex pend_lock;
    mp_cond  pend_cv;
    struct wt_task **pending;
    int pend_num;
    int pend_cap;

    // ---- result queue (workers producer; core consumer) ----
    mp_mutex res_lock;
    struct wt_result **results;
    int res_num;
    int res_cap;

    // ---- worker threads ----
    mp_thread workers[WT_WORKERS_MAX];
    int       worker_count;
    atomic_bool terminate;

    // ---- per-pipeline state (under pend_lock) ----
    int next_seq;
    bool first_failure_logged;

    // ---- cost-protection state (under limits_lock) ----
    mp_mutex limits_lock;
    struct wt_limits cfg;
    // Wall-clock (mp_time_ns()/1e6) gate: while now < bump_quiet_until_ms,
    // workers defer translating. Updated by bump_generation() so connected
    // seek/audio-chain/invalidate paths all benefit without further plumbing.
    int64_t  bump_quiet_until_ms;
    // RPM token bucket. Refilled lazily on each take.
    double   rpm_tokens;
    int64_t  rpm_last_refill_ms;
    // Counters surfaced via whisper-translate-status.
    int      session_req_used;       // committed HTTP issues
    int      horizon_skipped;        // task put-backs (cumulative)
    int      cache_reused;
    int      loop_skipped;
    int      short_skipped;
    int      rpm_skipped;
    int      budget_skipped;
    int      far_future_dropped;   // silent drops by high-water cleanup
    int      queue_overflow;       // hard-cap evictions to fallback
    bool     budget_exhausted_logged;
    // LRU translation-reuse cache (head = most-recent).
    struct wt_cache_entry *cache;
    int      cache_num;
};

// Forward declarations.
static void wl_feed_subtitle_text(struct whisper_lookahead *wl,
                                  const char *body,
                                  double pts, double dur,
                                  const char *kind);
static void wt_push_result(struct wt_pipeline *wt, struct wt_result *r);

// Reads `wl->translator` and bumps its refcount; caller must release.
// Returns NULL if no translator is set.
static struct whisper_translator *wt_acquire_translator(
    struct whisper_lookahead *wl)
{
    mp_mutex_lock(&wl->translator_lock);
    struct whisper_translator *tr = wl->translator
        ? whisper_translator_acquire(wl->translator) : NULL;
    mp_mutex_unlock(&wl->translator_lock);
    return tr;
}

// ---------- Cost-protection helpers ----------

static inline int64_t wt_wall_ms(void)
{
    return mp_time_ns() / (int64_t)1000000;
}

// Default-fill any zero/negative field on `cfg` to a sane built-in. Called
// after wt_pipeline_create and after every limits-JSON push so callers may
// omit fields they don't care about.
static void wt_limits_apply_defaults(struct wt_limits *cfg)
{
    if (cfg->horizon_sec <= 0)
        cfg->horizon_sec = WT_DEFAULT_HORIZON_SEC;
    if (cfg->seek_debounce_ms < 0)
        cfg->seek_debounce_ms = WT_DEFAULT_SEEK_DEBOUNCE_MS;
    if (cfg->min_text_chars < 0)
        cfg->min_text_chars = WT_DEFAULT_MIN_TEXT_CHARS;
    if (cfg->reuse_cache_capacity < 0)
        cfg->reuse_cache_capacity = WT_DEFAULT_REUSE_CACHE_CAP;
    if (cfg->reuse_cache_capacity > WT_REUSE_CACHE_HARD_CAP)
        cfg->reuse_cache_capacity = WT_REUSE_CACHE_HARD_CAP;
    if (cfg->reuse_cache_window_ms < 0)
        cfg->reuse_cache_window_ms = WT_DEFAULT_REUSE_WINDOW_MS;
    if (cfg->repeat_loop_threshold < 0)
        cfg->repeat_loop_threshold = WT_DEFAULT_REPEAT_THRESHOLD;
    if (cfg->repeat_loop_window_ms < 0)
        cfg->repeat_loop_window_ms = WT_DEFAULT_REPEAT_WINDOW_MS;
    if (cfg->rpm_limit < 0)
        cfg->rpm_limit = 0;
    if (cfg->session_request_limit < 0)
        cfg->session_request_limit = 0;
}

// Count UTF-8 codepoints in `s`. Stops at NUL. Used as a cheap "char count"
// proxy for the min-text-chars filter; for the protection threshold here we
// don't need full grapheme awareness.
static int wt_utf8_chars(const char *s)
{
    int n = 0;
    if (!s) return 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        // Count bytes that are NOT UTF-8 continuation bytes (0b10xxxxxx).
        if ((c & 0xC0) != 0x80)
            n++;
    }
    return n;
}

// Find a cache entry matching `norm` and move it to the head of the LRU.
// Returns NULL if not present. Caller must hold `wt->limits_lock`.
static struct wt_cache_entry *wt_cache_lookup(struct wt_pipeline *wt,
                                              const char *norm)
{
    if (!wt->cache || !norm || wt->cache_num == 0)
        return NULL;
    for (int i = 0; i < wt->cache_num; i++) {
        if (strcmp(wt->cache[i].norm_text, norm) == 0) {
            if (i > 0) {
                struct wt_cache_entry tmp = wt->cache[i];
                memmove(&wt->cache[1], &wt->cache[0],
                        sizeof(struct wt_cache_entry) * i);
                wt->cache[0] = tmp;
            }
            return &wt->cache[0];
        }
    }
    return NULL;
}

// Insert (or refresh) a cache entry for `norm` with `translated`. Both
// strings are duplicated as talloc children of `wt->cache`. Evicts the
// least-recently-used entry when capacity is reached. Caller must hold
// `wt->limits_lock`.
static void wt_cache_put(struct wt_pipeline *wt, const char *norm,
                         const char *translated)
{
    int cap = wt->cfg.reuse_cache_capacity;
    if (cap <= 0 || !norm || !translated)
        return;

    struct wt_cache_entry *e = wt_cache_lookup(wt, norm);
    int64_t now_ms = wt_wall_ms();
    if (e) {
        if (e->translated)
            talloc_free(e->translated);
        e->translated = talloc_strdup(wt->cache, translated);
        e->inserted_wall_ms = now_ms;
        return;
    }

    if (!wt->cache) {
        wt->cache = talloc_zero_array(wt, struct wt_cache_entry, cap);
        wt->cache_num = 0;
    }
    if (wt->cache_num < cap) {
        memmove(&wt->cache[1], &wt->cache[0],
                sizeof(struct wt_cache_entry) * wt->cache_num);
        wt->cache_num++;
    } else {
        // Evict tail.
        struct wt_cache_entry *tail = &wt->cache[cap - 1];
        if (tail->norm_text)   talloc_free(tail->norm_text);
        if (tail->translated)  talloc_free(tail->translated);
        memmove(&wt->cache[1], &wt->cache[0],
                sizeof(struct wt_cache_entry) * (cap - 1));
    }
    wt->cache[0] = (struct wt_cache_entry){
        .norm_text          = talloc_strdup(wt->cache, norm),
        .translated         = talloc_strdup(wt->cache, translated),
        .inserted_wall_ms   = now_ms,
        .first_seen_wall_ms = now_ms,
        .hit_count          = 1,
    };
}

// Bump the per-text hit counter, resetting the window when stale. Returns
// true when the new count crosses the loop threshold (caller should fall
// back to original text). Caller must hold `wt->limits_lock`.
static bool wt_repeat_record_hit(struct wt_pipeline *wt, const char *norm)
{
    if (!norm || wt->cfg.repeat_loop_threshold <= 0 ||
        wt->cfg.repeat_loop_window_ms <= 0)
    {
        return false;
    }
    int64_t now_ms = wt_wall_ms();

    // Find or create entry; we reuse the LRU table for repeat tracking too,
    // because the dedup key is the same normalized text. Insertion path here
    // does NOT yet have a translation; translated stays NULL until the
    // worker successfully translates and calls wt_cache_put.
    struct wt_cache_entry *e = wt_cache_lookup(wt, norm);
    if (!e) {
        int cap = wt->cfg.reuse_cache_capacity;
        if (cap <= 0)
            return false;
        if (!wt->cache) {
            wt->cache = talloc_zero_array(wt, struct wt_cache_entry, cap);
            wt->cache_num = 0;
        }
        if (wt->cache_num < cap) {
            memmove(&wt->cache[1], &wt->cache[0],
                    sizeof(struct wt_cache_entry) * wt->cache_num);
            wt->cache_num++;
        } else {
            struct wt_cache_entry *tail = &wt->cache[cap - 1];
            if (tail->norm_text)  talloc_free(tail->norm_text);
            if (tail->translated) talloc_free(tail->translated);
            memmove(&wt->cache[1], &wt->cache[0],
                    sizeof(struct wt_cache_entry) * (cap - 1));
        }
        wt->cache[0] = (struct wt_cache_entry){
            .norm_text          = talloc_strdup(wt->cache, norm),
            .translated         = NULL,
            .inserted_wall_ms   = 0,
            .first_seen_wall_ms = now_ms,
            .hit_count          = 1,
        };
        return false;
    }

    if (now_ms - e->first_seen_wall_ms > wt->cfg.repeat_loop_window_ms) {
        e->first_seen_wall_ms = now_ms;
        e->hit_count = 1;
        return false;
    }
    e->hit_count++;
    return e->hit_count >= wt->cfg.repeat_loop_threshold;
}

// Refill the RPM token bucket lazily. Caller must hold `wt->limits_lock`.
static void wt_refill_tokens(struct wt_pipeline *wt)
{
    int rpm = wt->cfg.rpm_limit;
    if (rpm <= 0)
        return;
    int64_t now = wt_wall_ms();
    if (wt->rpm_last_refill_ms == 0) {
        wt->rpm_last_refill_ms = now;
        wt->rpm_tokens = rpm;
        return;
    }
    double elapsed_ms = (double)(now - wt->rpm_last_refill_ms);
    if (elapsed_ms <= 0)
        return;
    wt->rpm_tokens += elapsed_ms * (rpm / 60000.0);
    if (wt->rpm_tokens > rpm)
        wt->rpm_tokens = rpm;
    wt->rpm_last_refill_ms = now;
}

// Atomic "reserve" of one HTTP slot under both session and RPM caps. Returns
// true if the worker may proceed to call the translator. On failure, the
// caller should produce a fallback (kind set via *out_kind). Caller must
// hold `wt->limits_lock`.
static bool wt_try_reserve(struct wt_pipeline *wt,
                           enum wt_result_kind *out_kind)
{
    if (wt->cfg.session_request_limit > 0 &&
        wt->session_req_used >= wt->cfg.session_request_limit)
    {
        *out_kind = WT_RESULT_FALLBACK_BUDGET;
        wt->budget_skipped++;
        return false;
    }
    wt_refill_tokens(wt);
    if (wt->cfg.rpm_limit > 0) {
        if (wt->rpm_tokens < 1.0) {
            *out_kind = WT_RESULT_FALLBACK_RPM;
            wt->rpm_skipped++;
            return false;
        }
        wt->rpm_tokens -= 1.0;
    }
    wt->session_req_used++;
    return true;
}

// Roll back a reservation when the call returned without actually issuing
// HTTP (e.g. local backoff). Caller must hold `wt->limits_lock`.
static void wt_rollback_reserve(struct wt_pipeline *wt)
{
    if (wt->session_req_used > 0)
        wt->session_req_used--;
    if (wt->cfg.rpm_limit > 0)
        wt->rpm_tokens += 1.0;
}

// Read the latest published playback pts (seconds) from the lookahead snap.
// Returns -INFINITY when not yet known (e.g. before audio starts).
static double wt_get_playback_pts(struct whisper_lookahead *wl)
{
    double pts;
    mp_mutex_lock(&wl->snap_lock);
    pts = wl->snap.playback_pts;
    mp_mutex_unlock(&wl->snap_lock);
    if (!isfinite(pts))
        pts = -INFINITY;
    return pts;
}

// Read the latest committed generation. Workers compare it to a task's
// captured generation to skip work that the user has invalidated by
// seeking, swapping translators, or changing language.
static uint64_t wt_get_generation(struct whisper_lookahead *wl)
{
    uint64_t g;
    mp_mutex_lock(&wl->snap_lock);
    g = wl->snap.generation;
    mp_mutex_unlock(&wl->snap_lock);
    return g;
}

// Sleep for at most `timeout_ms` waiting for new pending or terminate. On
// return the lock is held by neither side. The cv-based wait keeps the
// worker responsive to bump events.
static void wt_worker_sleep(struct wt_pipeline *wt, int timeout_ms)
{
    if (timeout_ms <= 0)
        return;
    int64_t until_ns = mp_time_ns() + (int64_t)timeout_ms * 1000000;
    mp_mutex_lock(&wt->pend_lock);
    if (!atomic_load(&wt->terminate))
        mp_cond_timedwait_until(&wt->pend_cv, &wt->pend_lock, until_ns);
    mp_mutex_unlock(&wt->pend_lock);
}

static MP_THREAD_VOID wt_worker_loop(void *arg)
{
    struct wt_pipeline *wt = arg;
    struct whisper_lookahead *wl = wt->wl;
    mp_thread_set_name("whisper-trans");

    for (;;) {
        if (atomic_load(&wt->terminate))
            break;

        // ---- Snapshot live state outside any pipeline lock ----
        uint64_t cur_gen = wt_get_generation(wl);

        struct wt_limits cfg;
        int64_t bump_quiet_until_ms;
        mp_mutex_lock(&wt->limits_lock);
        cfg = wt->cfg;
        bump_quiet_until_ms = wt->bump_quiet_until_ms;
        mp_mutex_unlock(&wt->limits_lock);

        int64_t now_ms = wt_wall_ms();

        // ① Seek-debounce gate: a global wall-clock pause after the last
        //    generation bump. Park ALL workers without touching pending so
        //    in-flight bursts settle before we charge the translator. The
        //    sleep is bounded so workers stay responsive to terminate /
        //    new generations.
        if (cfg.enabled && cfg.seek_debounce_ms > 0 &&
            now_ms < bump_quiet_until_ms)
        {
            int64_t left = bump_quiet_until_ms - now_ms;
            int sleep_ms = left > WT_DEFER_SLEEP_MS
                                ? WT_DEFER_SLEEP_MS : (int)left;
            wt_worker_sleep(wt, sleep_ms);
            continue;
        }

        double playback_pts = wt_get_playback_pts(wl);
        bool horizon_active = cfg.enabled && cfg.horizon_sec > 0 &&
                              isfinite(playback_pts);
        double horizon_cutoff = horizon_active
            ? playback_pts + (double)cfg.horizon_sec
            : INFINITY;

        // ② Pick first eligible task. Sweep stale-generation entries inline
        //    (so other workers don't keep re-scanning them) and SKIP IN
        //    PLACE far-future tasks — they remain in pending until playback
        //    advances, instead of being bounced out and back in via
        //    putback_head, which used to head-of-line block all N workers
        //    whenever the front of the queue was beyond the horizon.
        struct wt_task *task = NULL;
        bool saw_future = false;

        mp_mutex_lock(&wt->pend_lock);
        while (wt->pend_num == 0 && !atomic_load(&wt->terminate))
            mp_cond_wait(&wt->pend_cv, &wt->pend_lock);
        if (atomic_load(&wt->terminate)) {
            mp_mutex_unlock(&wt->pend_lock);
            break;
        }

        for (int i = 0; i < wt->pend_num; ) {
            struct wt_task *t = wt->pending[i];
            if (t->generation != cur_gen) {
                talloc_free(t);
                memmove(&wt->pending[i], &wt->pending[i + 1],
                        sizeof(struct wt_task *) * (wt->pend_num - i - 1));
                wt->pend_num--;
                continue;
            }
            if (horizon_active && t->pts > horizon_cutoff) {
                saw_future = true;
                i++;
                continue;
            }
            task = t;
            memmove(&wt->pending[i], &wt->pending[i + 1],
                    sizeof(struct wt_task *) * (wt->pend_num - i - 1));
            wt->pend_num--;
            break;
        }
        mp_mutex_unlock(&wt->pend_lock);

        if (!task) {
            // Either pending was wiped by stale sweep, or every remaining
            // entry is far-future. Account once per scan (not per task) so
            // horizon_skipped reflects "deferred work cycles", not pending
            // entry count.
            if (saw_future) {
                mp_mutex_lock(&wt->limits_lock);
                wt->horizon_skipped++;
                mp_mutex_unlock(&wt->limits_lock);
            }
            wt_worker_sleep(wt, WT_DEFER_SLEEP_MS);
            continue;
        }

        // ③ Defense in depth: generation may have advanced between the
        //    snapshot above and the pop. Re-check before doing real work.
        if (task->generation != wt_get_generation(wl)) {
            talloc_free(task);
            continue;
        }

        // ---- From here on the task will produce a result. ----
        // Refresh wall clock; the worker may have parked at the cv above.
        now_ms = wt_wall_ms();

        struct wt_result *r = talloc_zero(NULL, struct wt_result);
        r->generation = task->generation;
        r->seq        = task->seq;
        r->pts        = task->pts;
        r->dur        = task->dur;
        r->text       = talloc_strdup(r, task->text);
        r->kind       = WT_RESULT_FALLBACK_FAILURE;

        // ④ Min-text-chars filter: feed original directly. This also dampens
        //    the cost of single-syllable hallucinations that whisper.cpp
        //    sometimes emits in silence.
        if (cfg.enabled && cfg.min_text_chars > 0 &&
            wt_utf8_chars(task->text) < cfg.min_text_chars)
        {
            mp_mutex_lock(&wt->limits_lock);
            wt->short_skipped++;
            mp_mutex_unlock(&wt->limits_lock);
            r->kind = WT_RESULT_FALLBACK_SHORT;
            talloc_free(task);
            wt_push_result(wt, r);
            continue;
        }

        // ⑤ Cache lookup + repeat-loop tracking.
        char *norm = NULL;
        if (cfg.enabled)
            norm = wl_normalize_text(NULL, task->text);
        if (norm) {
            mp_mutex_lock(&wt->limits_lock);
            struct wt_cache_entry *hit = wt_cache_lookup(wt, norm);
            bool reused = false;
            if (hit && hit->translated &&
                cfg.reuse_cache_window_ms > 0 &&
                (now_ms - hit->inserted_wall_ms) <= cfg.reuse_cache_window_ms)
            {
                r->translated = talloc_strdup(r, hit->translated);
                r->kind = WT_RESULT_REUSED;
                wt->cache_reused++;
                reused = true;
            }
            bool loop = false;
            if (!reused) {
                loop = wt_repeat_record_hit(wt, norm);
                if (loop) {
                    wt->loop_skipped++;
                }
            }
            mp_mutex_unlock(&wt->limits_lock);

            if (reused) {
                talloc_free(norm);
                talloc_free(task);
                wt_push_result(wt, r);
                continue;
            }
            if (loop) {
                r->kind = WT_RESULT_FALLBACK_LOOP;
                talloc_free(norm);
                talloc_free(task);
                wt_push_result(wt, r);
                continue;
            }
        }

        // ⑥ Reserve under session/RPM caps. Failures map to a fallback kind.
        bool reserved = false;
        enum wt_result_kind reject_kind = WT_RESULT_FALLBACK_FAILURE;
        if (cfg.enabled) {
            mp_mutex_lock(&wt->limits_lock);
            reserved = wt_try_reserve(wt, &reject_kind);
            mp_mutex_unlock(&wt->limits_lock);
        } else {
            reserved = true;
        }
        if (!reserved) {
            r->kind = reject_kind;
            talloc_free(norm);
            talloc_free(task);
            wt_push_result(wt, r);
            continue;
        }

        // ⑦ Issue translation (HTTP). Outside limits_lock so concurrent
        //    workers may also reserve / commit while one is on the wire.
        struct whisper_translator *tr = wt_acquire_translator(wl);
        struct wt_call_result call = {0};
        char *translated = NULL;
        if (!tr) {
            r->error_brief = talloc_strdup(r, "no translator");
        } else {
            void *tmp = talloc_new(NULL);
            whisper_translate_call(tr, tmp, task->text, &call);
            if (call.translated)
                translated = talloc_strdup(r, call.translated);
            r->rate_limited = call.rate_limited;
            if (!call.translated && call.error[0])
                r->error_brief = talloc_strdup(r, call.error);
            talloc_free(tmp);
            whisper_translator_release(&tr);
        }

        // ⑧ Commit / rollback. If the provider short-circuited (e.g. local
        //    backoff, build-body failure) it will not have set http_issued —
        //    don't charge the user for it.
        if (cfg.enabled) {
            mp_mutex_lock(&wt->limits_lock);
            if (!call.http_issued)
                wt_rollback_reserve(wt);
            // Refresh cache on success.
            if (translated && norm)
                wt_cache_put(wt, norm, translated);
            mp_mutex_unlock(&wt->limits_lock);
        }

        if (translated) {
            r->translated = translated;
            r->kind = WT_RESULT_TRANSLATED;
        } else {
            r->kind = WT_RESULT_FALLBACK_FAILURE;
        }

        talloc_free(norm);
        talloc_free(task);
        wt_push_result(wt, r);
    }

    MP_THREAD_RETURN();
}

// Push a result. Bounded by WT_RES_MAX; under overflow we drop the oldest
// untranslated entries (worst case: translator hammered + drain stalled,
// playback already moved past). The drain itself decides timing/generation.
static void wt_push_result(struct wt_pipeline *wt, struct wt_result *r)
{
    // Filter stale results before they ever reach the result queue. Without
    // this, a generation bump (seek / language / translator swap) followed
    // by a fresh translation burst can let stale far-future results occupy
    // the WT_RES_MAX hard-cap and evict the new generation's valid entries
    // via the FIFO drop below. Drain still re-checks generation as a final
    // safety net.
    if (r->generation != wt_get_generation(wt->wl)) {
        talloc_free(r);
        return;
    }

    mp_mutex_lock(&wt->res_lock);
    if (wt->res_num >= wt->res_cap) {
        int new_cap = wt->res_cap ? wt->res_cap * 2 : 16;
        if (new_cap > WT_RES_MAX)
            new_cap = WT_RES_MAX;
        if (new_cap > wt->res_cap) {
            wt->results = talloc_realloc(wt, wt->results,
                                          struct wt_result *, new_cap);
            wt->res_cap = new_cap;
        }
    }
    if (wt->res_num >= WT_RES_MAX) {
        // Hard cap; drop the oldest result (it is the most likely already
        // past its display window if drain is this far behind).
        talloc_free(wt->results[0]);
        memmove(&wt->results[0], &wt->results[1],
                sizeof(struct wt_result *) * (wt->res_num - 1));
        wt->res_num--;
    }
    wt->results[wt->res_num++] = r;
    mp_mutex_unlock(&wt->res_lock);
    mp_wakeup_core(wt->wl->mpctx);
}

// Allocate a task and enqueue it. If the pending queue is full, the
// most-future pending task is evicted and demoted to a fallback (original-
// text) result.
static void wt_enqueue(struct wt_pipeline *wt,
                       uint64_t generation,
                       const char *text, double pts, double dur)
{
    struct wt_task *task = talloc_zero(NULL, struct wt_task);
    task->generation = generation;
    task->pts = pts;
    task->dur = dur;
    task->text = talloc_strdup(task, text);

    struct wt_result *evicted = NULL;

    mp_mutex_lock(&wt->pend_lock);
    task->seq = wt->next_seq++;
    if (wt->pend_num >= wt->pend_cap) {
        int new_cap = wt->pend_cap ? wt->pend_cap * 2 : 16;
        if (new_cap > WT_PEND_MAX)
            new_cap = WT_PEND_MAX;
        if (new_cap > wt->pend_cap) {
            wt->pending = talloc_realloc(wt, wt->pending,
                                          struct wt_task *, new_cap);
            wt->pend_cap = new_cap;
        }
    }

    // (Previously: a 75% high-water cleanup silently freed tasks whose pts
    // exceeded `playback + 4 * horizon`. That path was removed because (a)
    // worker-side horizon scanning now leaves far-future tasks in pending
    // until playback advances, and (b) immediately demoting them to fallback
    // would have injected future-PTS subtitle packets ahead of upcoming
    // earlier translations, making the out-of-order drop problem worse.
    // The hard-cap eviction below is the only safety net now; under A's
    // non-blocking horizon gate plus the lower LOOKAHEAD_MAX_SEC it should
    // essentially never fire.)

    if (wt->pend_num >= WT_PEND_MAX) {
        // Pick the largest-pts task to evict (safest to convert to a
        // fallback because it has the most slack remaining for the user
        // to actually see the original text).
        int worst = 0;
        for (int i = 1; i < wt->pend_num; i++) {
            if (wt->pending[i]->pts > wt->pending[worst]->pts)
                worst = i;
        }
        struct wt_task *t = wt->pending[worst];
        memmove(&wt->pending[worst], &wt->pending[worst + 1],
                sizeof(struct wt_task *) * (wt->pend_num - worst - 1));
        wt->pend_num--;

        evicted = talloc_zero(NULL, struct wt_result);
        evicted->generation = t->generation;
        evicted->seq = t->seq;
        evicted->pts = t->pts;
        evicted->dur = t->dur;
        evicted->text = talloc_strdup(evicted, t->text);
        evicted->error_brief = talloc_strdup(evicted, "queue overflow");
        evicted->kind = WT_RESULT_FALLBACK_QUEUE;
        talloc_free(t);

        mp_mutex_lock(&wt->limits_lock);
        wt->queue_overflow++;
        mp_mutex_unlock(&wt->limits_lock);
    }
    wt->pending[wt->pend_num++] = task;
    mp_cond_signal(&wt->pend_cv);
    mp_mutex_unlock(&wt->pend_lock);

    if (evicted)
        wt_push_result(wt, evicted);
}

// Drop all pending tasks (e.g. on seek / generation bump). In-flight tasks
// inside workers are NOT cancelled; their results will be discarded by the
// drain when their generation no longer matches.
static void wt_clear_pending(struct wt_pipeline *wt)
{
    mp_mutex_lock(&wt->pend_lock);
    for (int i = 0; i < wt->pend_num; i++)
        talloc_free(wt->pending[i]);
    wt->pend_num = 0;
    mp_mutex_unlock(&wt->pend_lock);
}

static struct wt_pipeline *wt_pipeline_create(struct whisper_lookahead *wl)
{
    struct wt_pipeline *wt = talloc_zero(wl, struct wt_pipeline);
    wt->wl = wl;
    mp_mutex_init(&wt->pend_lock);
    mp_mutex_init(&wt->res_lock);
    mp_mutex_init(&wt->limits_lock);
    mp_cond_init(&wt->pend_cv);
    atomic_init(&wt->terminate, false);
    wt->pending = talloc_zero_array(wt, struct wt_task *, 16);
    wt->pend_cap = 16;
    wt->results = talloc_zero_array(wt, struct wt_result *, 16);
    wt->res_cap = 16;
    wt->next_seq = 1;

    // Default cost-protection config: enabled but with the bucket/budget
    // disabled (rpm_limit=0, session_request_limit=0). C# pushes the real
    // values via whisper-translate-limits before playback meaningfully
    // begins; until then the horizon and seek-debounce already apply.
    wt->cfg = (struct wt_limits){ .enabled = true };
    wt_limits_apply_defaults(&wt->cfg);
    wt->bump_quiet_until_ms = 0;
    wt->rpm_tokens = 0;
    wt->rpm_last_refill_ms = 0;

    int n = WT_WORKERS_DEFAULT;
    if (n < 1) n = 1;
    if (n > WT_WORKERS_MAX) n = WT_WORKERS_MAX;
    for (int i = 0; i < n; i++) {
        if (mp_thread_create(&wt->workers[i], wt_worker_loop, wt) == 0)
            wt->worker_count++;
        else
            MP_WARN(wl, "wt_pipeline: failed to spawn worker %d\n", i);
    }
    if (wt->worker_count == 0) {
        MP_ERR(wl, "wt_pipeline: no workers; AI translation disabled\n");
        // Tear down so callers don't enqueue tasks that nobody will drain.
        mp_cond_destroy(&wt->pend_cv);
        mp_mutex_destroy(&wt->pend_lock);
        mp_mutex_destroy(&wt->res_lock);
        mp_mutex_destroy(&wt->limits_lock);
        talloc_free(wt);
        return NULL;
    }
    MP_INFO(wl, "wt_pipeline: started with %d worker(s)\n", wt->worker_count);
    return wt;
}

static void wt_pipeline_destroy(struct wt_pipeline **ptr)
{
    if (!ptr || !*ptr)
        return;
    struct wt_pipeline *wt = *ptr;
    *ptr = NULL;

    atomic_store(&wt->terminate, true);
    mp_mutex_lock(&wt->pend_lock);
    mp_cond_broadcast(&wt->pend_cv);
    mp_mutex_unlock(&wt->pend_lock);

    for (int i = 0; i < wt->worker_count; i++)
        mp_thread_join(wt->workers[i]);

    for (int i = 0; i < wt->pend_num; i++)
        talloc_free(wt->pending[i]);
    for (int i = 0; i < wt->res_num; i++)
        talloc_free(wt->results[i]);
    // Cache strings are talloc children of `wt`, freed automatically.
    wt->cache = NULL;
    wt->cache_num = 0;

    mp_cond_destroy(&wt->pend_cv);
    mp_mutex_destroy(&wt->pend_lock);
    mp_mutex_destroy(&wt->res_lock);
    mp_mutex_destroy(&wt->limits_lock);
    talloc_free(wt);
}

// Drain ready translation results on the core thread and feed them as
// subtitle packets. Called from the playloop. Safe to call when wl/pipeline
// is NULL.
void whisper_lookahead_drain_results(struct MPContext *mpctx)
{
    struct whisper_lookahead *wl = mpctx->whisper_lookahead;
    if (!wl || !wl->pipeline || !wl->primary_stream || !wl->primary_demuxer)
        return;
    struct wt_pipeline *wt = wl->pipeline;

    // Snapshot results.
    struct wt_result **batch = NULL;
    int n = 0;
    mp_mutex_lock(&wt->res_lock);
    if (wt->res_num > 0) {
        batch = talloc_array(NULL, struct wt_result *, wt->res_num);
        memcpy(batch, wt->results, sizeof(*batch) * wt->res_num);
        n = wt->res_num;
        wt->res_num = 0;
    }
    mp_mutex_unlock(&wt->res_lock);
    if (n == 0)
        return;

    // Sort by seq so ASS ReadOrder remains monotonic.
    for (int i = 1; i < n; i++) {
        struct wt_result *cur = batch[i];
        int j = i - 1;
        while (j >= 0 && batch[j]->seq > cur->seq) {
            batch[j + 1] = batch[j];
            j--;
        }
        batch[j + 1] = cur;
    }

    // Latest generation + playback time (single thread; no lock for these).
    uint64_t cur_gen;
    double now_pts;
    mp_mutex_lock(&wl->snap_lock);
    cur_gen = wl->snap.generation;
    now_pts = wl->snap.playback_pts;
    mp_mutex_unlock(&wl->snap_lock);
    if (!isfinite(now_pts))
        now_pts = -INFINITY;

    bool any_success = false;
    for (int i = 0; i < n; i++) {
        struct wt_result *r = batch[i];

        // ⑤ Generation mismatch (seek / audio chain change / translator
        // swap happened after enqueue): silently drop.
        if (r->generation != cur_gen) {
            talloc_free(r);
            continue;
        }

        // ③ Already past the display window: drop.
        if (r->pts + r->dur + WT_DRAIN_EPSILON_S < now_pts) {
            MP_INFO(wl, "translation arrived late, dropped (pts=%.3f "
                        "dur=%.2f, now=%.3f)\n", r->pts, r->dur, now_pts);
            talloc_free(r);
            continue;
        }

        if (r->translated) {
            // ② Success / reuse path: bilingual ASS line.
            char *body = talloc_asprintf(NULL,
                "{\\fs72\\c&H00FFFFFF&\\3c&H00000000&\\bord3}%s"
                "\\N{\\fs48\\c&H00E0FFFF&\\3c&H00000000&\\bord2}%s",
                r->translated, r->text);
            wl->translations_injected++;
            const char *tag = r->kind == WT_RESULT_REUSED ? "translated-reused"
                                                          : "translated";
            MP_INFO(wl, "%s #%d: %.40s%s\n",
                    tag,
                    wl->translations_injected,
                    r->translated,
                    strlen(r->translated) > 40 ? "..." : "");
            wl_feed_subtitle_text(wl, body, r->pts, r->dur, tag);
            talloc_free(body);
            any_success = true;
        } else {
            // Fallback path: feed original. Only genuine HTTP/provider
            // failures get the first-failure WARN; throttling fallbacks
            // (short / loop / rpm / budget / queue) are expected and stay
            // verbose to avoid log spam under heavy use.
            const char *kind_tag = "original-after-translate-fail";
            switch (r->kind) {
            case WT_RESULT_FALLBACK_SHORT:
                kind_tag = "original-too-short"; break;
            case WT_RESULT_FALLBACK_LOOP:
                kind_tag = "original-repeat-loop"; break;
            case WT_RESULT_FALLBACK_RPM:
                kind_tag = "original-rpm-cap"; break;
            case WT_RESULT_FALLBACK_BUDGET:
                kind_tag = "original-budget"; break;
            case WT_RESULT_FALLBACK_QUEUE:
                kind_tag = "original-queue-overflow"; break;
            case WT_RESULT_FALLBACK_FAILURE:
            default:
                kind_tag = "original-after-translate-fail"; break;
            }
            if (r->kind == WT_RESULT_FALLBACK_FAILURE &&
                !wt->first_failure_logged && r->error_brief)
            {
                MP_WARN(wl, "translation failed: %s (showing original; "
                            "subsequent failures suppressed until next "
                            "success)\n", r->error_brief);
                mp_mutex_lock(&wt->pend_lock);
                wt->first_failure_logged = true;
                mp_mutex_unlock(&wt->pend_lock);
            } else if (r->kind != WT_RESULT_FALLBACK_FAILURE) {
                MP_VERBOSE(wl, "fallback (%s) pts=%.2f dur=%.2f\n",
                           kind_tag, r->pts, r->dur);
            }
            wl_feed_subtitle_text(wl, r->text, r->pts, r->dur, kind_tag);
        }

        talloc_free(r);
    }
    talloc_free(batch);

    // Clear the suppression flag once any translation succeeded so a new
    // failure burst will be logged again.
    if (any_success) {
        mp_mutex_lock(&wt->pend_lock);
        wt->first_failure_logged = false;
        mp_mutex_unlock(&wt->pend_lock);
    }
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
//   - Otherwise the audio is still the live audio. Re-tag the segment with
//     the CURRENT snap.generation when we hand it to wt_enqueue, so the
//     wt pipeline associates it with the live translator config; any
//     subsequent translator-only bump will then drop it correctly.
//
// Three paths:
//   ① No translator → feed original synchronously here.
//   ⑤ Translator configured but slack too tight → feed original here too.
//   Otherwise → enqueue to wt_pipeline; drain feeds (translated or original
//   fallback) on the core thread.
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
    uint64_t cur_gen, cur_graph_gen;
    double now_pts;
    mp_mutex_lock(&wl->snap_lock);
    cur_gen = wl->snap.generation;
    cur_graph_gen = wl->snap.graph_generation;
    now_pts = wl->snap.playback_pts;
    mp_mutex_unlock(&wl->snap_lock);
    if (producer_graph_gen != cur_graph_gen) {
        MP_INFO(wl, "drop stale segment (producer_graph_gen=%llu cur_graph_gen=%llu)\n",
                (unsigned long long)producer_graph_gen,
                (unsigned long long)cur_graph_gen);
        return;
    }
    (void)producer_gen; // superseded by cur_gen below for wt tagging
    if (!isfinite(now_pts))
        now_pts = -INFINITY;

    bool have_translator;
    mp_mutex_lock(&wl->translator_lock);
    have_translator = wl->translator != NULL;
    mp_mutex_unlock(&wl->translator_lock);

    if (!have_translator || !wl->pipeline) {
        // ① Plain feed (no AI translation in play).
        wl_feed_subtitle_text(wl, text, pts, dur,
                              "original-no-translator");
        return;
    }

    // ⑤ Slack check at enqueue time; avoid burning API tokens on subtitles
    // that are already too close to playback to land in time.
    double slack = pts + dur - now_pts;
    if (isfinite(now_pts) && slack < MIN_TRANSLATE_SLACK_S) {
        char tag[64];
        snprintf(tag, sizeof(tag), "original-slack-skip(%.2fs)", slack);
        wl_feed_subtitle_text(wl, text, pts, dur, tag);
        return;
    }

    // Tag the wt task with the LIVE generation so the wt pipeline drops it
    // correctly if a future translator-only bump fires while it's queued.
    wt_enqueue(wl->pipeline, cur_gen, text, pts, dur);
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
        if (ce_known && end > snap.cache_end)
            end = snap.cache_end;
        double cap = pb_known ? snap.playback_pts + LOOKAHEAD_MAX_SEC : INFINITY;
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

    // Translator selection priority:
    //   1) AI (OpenAI-compatible) if mpctx->whisper_ai_translate_json is set;
    //   2) otherwise legacy translate_to + translate_provider (google/azure)
    //      from the whisper-lookahead opts string.
    mp_mutex_lock(&wl->translator_lock);
    const char *ai_json = wl->mpctx->whisper_ai_translate_json;
    if (ai_json && ai_json[0]) {
        if (wl_apply_ai_translator_locked(wl, ai_json)) {
            MP_INFO(wl, "init: AI translator enabled\n");
        } else {
            MP_WARN(wl, "init: AI translator config invalid; falling back\n");
        }
    }
    if (!wl->translator && translate_to && translate_to[0] &&
        translate_provider != WT_PROVIDER_NONE)
    {
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
    mp_mutex_unlock(&wl->translator_lock);

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
//           the wt pipeline drops in-flight translations / pending tasks
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

    // Update the seek-debounce gate so workers skip translation while the
    // user keeps banging the timeline. Each bump pushes the quiet window
    // forward; once the user holds still for `seek_debounce_ms`, workers
    // release.
    if (wl->pipeline) {
        struct wt_pipeline *wt = wl->pipeline;
        mp_mutex_lock(&wt->limits_lock);
        if (wt->cfg.enabled && wt->cfg.seek_debounce_ms > 0) {
            int64_t until = wt_wall_ms() + wt->cfg.seek_debounce_ms;
            if (until > wt->bump_quiet_until_ms)
                wt->bump_quiet_until_ms = until;
        }
        mp_mutex_unlock(&wt->limits_lock);

        // Wake any workers parked in mp_cond_timedwait so they re-evaluate
        // the gates immediately (otherwise they sleep up to 200ms longer).
        mp_mutex_lock(&wt->pend_lock);
        mp_cond_broadcast(&wt->pend_cv);
        mp_mutex_unlock(&wt->pend_lock);
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
    mp_mutex_init(&wl->translator_lock);
    mp_cond_init(&wl->queue_cv);

    wl->pipeline = wt_pipeline_create(wl);

    mpctx->whisper_lookahead = wl;

    // Apply previously-configured limits (set before lookahead start by C#).
    if (mpctx->whisper_translate_limits_json &&
        mpctx->whisper_translate_limits_json[0])
    {
        whisper_lookahead_set_translate_limits(
            mpctx, mpctx->whisper_translate_limits_json);
    }

    // Publish initial snapshot so the worker has something to do as soon as
    // the init thread finishes.
    whisper_lookahead_publish(mpctx);

    if (mp_thread_create(&wl->init_thread, init_thread_fn, wl)) {
        MP_ERR(mpctx, "whisper lookahead: failed to create init thread\n");
        mp_mutex_destroy(&wl->queue_lock);
        mp_mutex_destroy(&wl->snap_lock);
        mp_mutex_destroy(&wl->translator_lock);
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

    // Tear down translation pipeline before the translator: workers may
    // hold acquired refs on the translator; destroy() blocks for them.
    wt_pipeline_destroy(&wl->pipeline);

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

    mp_mutex_lock(&wl->translator_lock);
    whisper_translator_destroy(&wl->translator);
    mp_mutex_unlock(&wl->translator_lock);

    mp_cond_destroy(&wl->queue_cv);
    mp_mutex_destroy(&wl->queue_lock);
    mp_mutex_destroy(&wl->snap_lock);
    mp_mutex_destroy(&wl->translator_lock);

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

    // Drop pending translation tasks queued before the seek; they would
    // produce subtitles for now-stale positions. In-flight translations
    // inside workers will simply be discarded by the drain.
    if (wl->pipeline)
        wt_clear_pending(wl->pipeline);

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
    if (wl->pipeline)
        wt_clear_pending(wl->pipeline);
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
    // output (whisper segment about to inject_subtitle, or wt_result
    // waiting for drain) gets dropped on its way out, and wt_clear_pending
    // drops not-yet-sent translation tasks immediately. Critically we do
    // NOT bump graph_generation here: the audio stream / recognized text
    // are still valid, and a graph reset would force af_whisper to
    // re-init (~3 GB VRAM + ~10 s CUDA reload on large-v3) just because
    // the user toggled an AI-translator option.
    bump_generation_translate_only(wl);
    if (wl->pipeline)
        wt_clear_pending(wl->pipeline);

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
    // Cache config on mpctx so a future whisper_lookahead_start() picks it up.
    talloc_free(mpctx->whisper_ai_translate_json);
    mpctx->whisper_ai_translate_json =
        (json && json[0]) ? talloc_strdup(mpctx, json) : NULL;

    struct whisper_lookahead *wl = mpctx->whisper_lookahead;
    if (!wl)
        return;
    // Only swap the live translator after init completes; otherwise init_thread
    // will pick up the cached JSON itself.
    if (!atomic_load(&wl->init_done))
        return;

    mp_mutex_lock(&wl->translator_lock);
    if (json && json[0]) {
        if (!wl_apply_ai_translator_locked(wl, json))
            MP_WARN(wl, "whisper-ai-translate: invalid config; AI disabled\n");
    } else {
        // Disable AI translator entirely (legacy provider is NOT auto-restored).
        whisper_translator_destroy(&wl->translator);
        wl_recent_clear(wl);
        MP_INFO(wl, "whisper-ai-translate: disabled\n");
    }
    mp_mutex_unlock(&wl->translator_lock);

    // Translator identity changed: drop pipeline state AND visible/queued
    // captions. Without the visible-caption purge, subtitles previously
    // emitted under the OLD translator (already shown or sitting in the
    // af_sub queue with future PTS) keep showing for ~30s.
    whisper_lookahead_invalidate(mpctx, "ai-translate-changed");
}

// Returns a JSON string (talloc child of `ta_parent`) describing the current
// AI translator status, or NULL if there is no live AI translator.
char *whisper_lookahead_get_ai_translate_status(struct MPContext *mpctx,
                                                void *ta_parent)
{
    struct whisper_lookahead *wl = mpctx->whisper_lookahead;
    if (!wl)
        return NULL;

    char *out = NULL;
    mp_mutex_lock(&wl->translator_lock);
    if (wl->translator) {
        struct wt_status st = {0};
        whisper_translator_get_status(wl->translator, &st);
        void *tmp = talloc_new(NULL);
        struct mpv_node root = {0};
        node_init(&root, MPV_FORMAT_NODE_MAP, NULL);
        talloc_steal(tmp, root.u.list);

        node_map_add_flag(&root, "enabled", st.enabled);
        node_map_add_flag(&root, "paused", st.paused);
        node_map_add_int64(&root, "fail_count", st.fail_count);
        node_map_add_int64(&root, "retry_after_ms", st.retry_after_ms);
        node_map_add_string(&root, "last_error", st.last_error);

        // Throttle counters from the wt_pipeline. These stay valid even if
        // the underlying translator is paused, so they don't depend on
        // whisper_translator_get_status.
        if (wl->pipeline) {
            struct wt_pipeline *wt = wl->pipeline;
            mp_mutex_lock(&wt->limits_lock);
            int session_used   = wt->session_req_used;
            int session_limit  = wt->cfg.session_request_limit;
            int rpm_limit      = wt->cfg.rpm_limit;
            double rpm_tokens  = wt->rpm_tokens;
            int horizon_skip   = wt->horizon_skipped;
            int cache_reused   = wt->cache_reused;
            int loop_skip      = wt->loop_skipped;
            int short_skip     = wt->short_skipped;
            int rpm_skip       = wt->rpm_skipped;
            int budget_skip    = wt->budget_skipped;
            int far_dropped    = wt->far_future_dropped;
            int q_overflow     = wt->queue_overflow;
            int horizon_sec    = wt->cfg.horizon_sec;
            int debounce_ms    = wt->cfg.seek_debounce_ms;
            int reuse_cap      = wt->cfg.reuse_cache_capacity;
            int cache_num      = wt->cache_num;
            bool budget_done   = session_limit > 0 &&
                                 session_used >= session_limit;
            mp_mutex_unlock(&wt->limits_lock);

            node_map_add_int64(&root, "session_req_used", session_used);
            node_map_add_int64(&root, "session_request_limit", session_limit);
            node_map_add_int64(&root, "rpm_limit", rpm_limit);
            node_map_add_int64(&root, "rpm_tokens",
                               (int64_t)(rpm_tokens + 0.5));
            node_map_add_int64(&root, "horizon_skipped", horizon_skip);
            node_map_add_int64(&root, "cache_reused", cache_reused);
            node_map_add_int64(&root, "loop_skipped", loop_skip);
            node_map_add_int64(&root, "short_skipped", short_skip);
            node_map_add_int64(&root, "rpm_skipped", rpm_skip);
            node_map_add_int64(&root, "budget_skipped", budget_skip);
            node_map_add_int64(&root, "far_future_dropped", far_dropped);
            node_map_add_int64(&root, "queue_overflow", q_overflow);
            node_map_add_int64(&root, "horizon_sec", horizon_sec);
            node_map_add_int64(&root, "seek_debounce_ms", debounce_ms);
            node_map_add_int64(&root, "reuse_cache_capacity", reuse_cap);
            node_map_add_int64(&root, "reuse_cache_size", cache_num);
            node_map_add_string(&root, "pause_reason",
                                budget_done ? "budget_exhausted" : "");
        }

        char *buf = NULL;
        if (json_write(&buf, &root) >= 0 && buf)
            out = talloc_strdup(ta_parent, buf);
        talloc_free(buf);
        talloc_free(tmp);
    }
    mp_mutex_unlock(&wl->translator_lock);
    return out;
}

// Apply a JSON limits payload to the live pipeline. Unknown keys are
// ignored; missing keys keep their current value (caller-side merge is
// not required). On failure, returns -1 and leaves config untouched.
//
// Counters are NOT reset; users may tighten the cap mid-session and the
// already-spent budget continues to apply.
int whisper_lookahead_set_translate_limits(struct MPContext *mpctx,
                                           const char *json_limits)
{
    struct whisper_lookahead *wl = mpctx->whisper_lookahead;
    if (!wl || !wl->pipeline || !json_limits || !json_limits[0])
        return -1;

    void *tmp = talloc_new(NULL);
    struct mpv_node root = {0};
    char *cursor = talloc_strdup(tmp, json_limits);
    if (json_parse(tmp, &root, &cursor, MAX_JSON_DEPTH) < 0 ||
        root.format != MPV_FORMAT_NODE_MAP)
    {
        talloc_free(tmp);
        return -1;
    }

    struct wt_pipeline *wt = wl->pipeline;
    mp_mutex_lock(&wt->limits_lock);
    struct wt_limits next = wt->cfg;
    for (int i = 0; i < root.u.list->num; i++) {
        const char *k = root.u.list->keys[i];
        struct mpv_node *v = &root.u.list->values[i];
        int64_t iv = 0;
        bool has_int = false, has_bool = false, bv = false;
        if (v->format == MPV_FORMAT_INT64) {
            iv = v->u.int64; has_int = true;
        } else if (v->format == MPV_FORMAT_DOUBLE) {
            iv = (int64_t)v->u.double_; has_int = true;
        } else if (v->format == MPV_FORMAT_FLAG) {
            bv = v->u.flag; has_bool = true;
        }
        if (!has_int && !has_bool)
            continue;
        if (strcmp(k, "enabled") == 0 && has_bool)
            next.enabled = bv;
        else if (strcmp(k, "horizon_sec") == 0 && has_int)
            next.horizon_sec = (int)iv;
        else if (strcmp(k, "seek_debounce_ms") == 0 && has_int)
            next.seek_debounce_ms = (int)iv;
        else if (strcmp(k, "min_text_chars") == 0 && has_int)
            next.min_text_chars = (int)iv;
        else if (strcmp(k, "reuse_cache_capacity") == 0 && has_int)
            next.reuse_cache_capacity = (int)iv;
        else if (strcmp(k, "reuse_cache_window_ms") == 0 && has_int)
            next.reuse_cache_window_ms = (int)iv;
        else if (strcmp(k, "repeat_loop_threshold") == 0 && has_int)
            next.repeat_loop_threshold = (int)iv;
        else if (strcmp(k, "repeat_loop_window_ms") == 0 && has_int)
            next.repeat_loop_window_ms = (int)iv;
        else if (strcmp(k, "rpm_limit") == 0 && has_int)
            next.rpm_limit = (int)iv;
        else if (strcmp(k, "session_request_limit") == 0 && has_int)
            next.session_request_limit = (int)iv;
    }
    wt_limits_apply_defaults(&next);
    wt->cfg = next;
    // Tighter rpm bucket: cap tokens to the new ceiling so a smaller cap
    // takes effect immediately.
    if (wt->rpm_tokens > next.rpm_limit)
        wt->rpm_tokens = next.rpm_limit;
    mp_mutex_unlock(&wt->limits_lock);

    talloc_free(tmp);
    mp_wakeup_core(mpctx);
    return 0;
}
