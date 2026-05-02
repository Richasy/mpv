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
 *   - On audio chain change / seek / start, snap.generation is bumped under
 *     snap_lock. The worker discards in-flight work that started under an
 *     older generation, rebuilds the AVCodecContext if codec params changed,
 *     resets the lavfi graph (which re-creates whisper, clearing its VAD),
 *     and resumes from the new playback position.
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
// LOOKAHEAD_MAX_SEC is intentionally large (10 minutes): the natural
// backpressure during normal playback comes from MAX_QUEUE_SECONDS (30 s)
// and from cache_end (the demuxer rarely buffers more than a few minutes
// ahead anyway).  Keeping the cap loose ensures that:
//   1) when the player is PAUSED, the worker keeps draining cached audio
//      into whisper instead of going idle right at playback_pts + 60 s
//      (otherwise subtitles "freeze" during pause, then resume slowly);
//   2) when the user seeks deep into the file and the cache initially has
//      only a small window, the worker doesn't artificially throttle past
//      what's already cached.
#define FIRST_CHUNK_SECONDS  6.0
#define CHUNK_SECONDS       12.0
#define MIN_CHUNK_SECONDS    1.5
#define LOOKAHEAD_MAX_SEC  600.0
#define WORKER_TICK_SEC      0.05  // tighter wakeups during startup ramp-up

struct frame_item {
    struct mp_frame f;
    uint64_t generation;
};

struct wt_pipeline;

// Snapshot of core-thread state visible to the worker. Updated by
// whisper_lookahead_publish(); read by the worker under snap_lock.
struct wl_snap {
    uint64_t generation;
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
    double worker_last_done;           // last visited end-pts (NOPTS = none)
    double worker_last_dts;            // last decoded packet's dts (dedupe)
    double worker_next_pts;            // interpolated frame pts fallback
    int chunks_visited;
    int packets_visited;
    int64_t last_starve_log_ns;  /* rate-limit for INFO-level starvation diag */
    int packets_skipped;
    int frames_decoded;
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
#define WT_PEND_MAX             64
#define WT_RES_MAX              128
#define MIN_TRANSLATE_SLACK_S   1.0
#define WT_DRAIN_EPSILON_S      0.05

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
    char    *translated;    // success: talloc child; failure: NULL
    bool    rate_limited;
    char    *error_brief;   // first-failure-only WARN payload (or NULL)
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
};

// Forward declarations.
static void wl_feed_subtitle_text(struct whisper_lookahead *wl,
                                  const char *body,
                                  double pts, double dur);
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

static MP_THREAD_VOID wt_worker_loop(void *arg)
{
    struct wt_pipeline *wt = arg;
    struct whisper_lookahead *wl = wt->wl;
    mp_thread_set_name("whisper-trans");

    for (;;) {
        // Wait for a task or terminate.
        mp_mutex_lock(&wt->pend_lock);
        while (!atomic_load(&wt->terminate) && wt->pend_num == 0)
            mp_cond_wait(&wt->pend_cv, &wt->pend_lock);
        if (atomic_load(&wt->terminate)) {
            mp_mutex_unlock(&wt->pend_lock);
            break;
        }
        // Pop the front (oldest = most-needed-soon).
        struct wt_task *task = wt->pending[0];
        memmove(&wt->pending[0], &wt->pending[1],
                sizeof(struct wt_task *) * (wt->pend_num - 1));
        wt->pend_num--;
        mp_mutex_unlock(&wt->pend_lock);

        // Build a result. Owned by talloc(NULL); pushed into the queue.
        struct wt_result *r = talloc_zero(NULL, struct wt_result);
        r->generation = task->generation;
        r->seq        = task->seq;
        r->pts        = task->pts;
        r->dur        = task->dur;
        r->text       = talloc_strdup(r, task->text);

        struct whisper_translator *tr = wt_acquire_translator(wl);
        if (!tr) {
            r->error_brief = talloc_strdup(r, "no translator");
        } else {
            struct wt_call_result call;
            char *out = NULL;
            void *tmp = talloc_new(NULL);
            whisper_translate_call(tr, tmp, task->text, &call);
            if (call.translated)
                out = talloc_strdup(r, call.translated);
            r->translated = out;
            r->rate_limited = call.rate_limited;
            if (!call.translated && call.error[0])
                r->error_brief = talloc_strdup(r, call.error);
            talloc_free(tmp);
            whisper_translator_release(&tr);
        }

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
        talloc_free(t);
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
    mp_cond_init(&wt->pend_cv);
    atomic_init(&wt->terminate, false);
    wt->pending = talloc_zero_array(wt, struct wt_task *, 16);
    wt->pend_cap = 16;
    wt->results = talloc_zero_array(wt, struct wt_result *, 16);
    wt->res_cap = 16;
    wt->next_seq = 1;

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

    mp_cond_destroy(&wt->pend_cv);
    mp_mutex_destroy(&wt->pend_lock);
    mp_mutex_destroy(&wt->res_lock);
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
            // ② Success path: bilingual ASS line.
            char *body = talloc_asprintf(NULL,
                "{\\fs72\\c&H00FFFFFF&\\3c&H00000000&\\bord3}%s"
                "\\N{\\fs48\\c&H00E0FFFF&\\3c&H00000000&\\bord2}%s",
                r->translated, r->text);
            wl->translations_injected++;
            MP_INFO(wl, "translated #%d: %.40s%s\n",
                    wl->translations_injected,
                    r->translated,
                    strlen(r->translated) > 40 ? "..." : "");
            wl_feed_subtitle_text(wl, body, r->pts, r->dur);
            talloc_free(body);
            any_success = true;
        } else {
            // ④ Failure path: feed original; first error WARNed.
            if (!wt->first_failure_logged && r->error_brief) {
                MP_WARN(wl, "translation failed: %s (showing original; "
                            "subsequent failures suppressed until next "
                            "success)\n", r->error_brief);
                mp_mutex_lock(&wt->pend_lock);
                wt->first_failure_logged = true;
                mp_mutex_unlock(&wt->pend_lock);
            }
            wl_feed_subtitle_text(wl, r->text, r->pts, r->dur);
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
static void wl_feed_subtitle_text(struct whisper_lookahead *wl,
                                  const char *body,
                                  double pts, double dur)
{
    if (!wl->primary_stream || !wl->primary_demuxer || !body || !body[0])
        return;

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
        MP_INFO(wl, "subtitle #%d @ %.3f (dur=%.1f): %.40s%s\n",
                wl->subtitles_injected, pts, dur, body,
                strlen(body) > 40 ? "..." : "");

        mp_wakeup_core(wl->mpctx);
    }
    talloc_free(ass_line);
}

// Inject a single subtitle with the given text, pts, and duration. PTS is in
// the player's timeline domain (seconds). V3.1 produces accurate PTS so no
// shift hack is required.
//
// Three paths:
//   ① No translator → feed original synchronously here.
//   ⑤ Translator configured but slack too tight → feed original here too.
//   Otherwise → enqueue to wt_pipeline; drain feeds (translated or original
//   fallback) on the core thread.
static void inject_subtitle(struct whisper_lookahead *wl,
                            const char *text, double pts, double dur)
{
    if (!wl->primary_stream || !wl->primary_demuxer || !text || !text[0])
        return;

    bool have_translator;
    mp_mutex_lock(&wl->translator_lock);
    have_translator = wl->translator != NULL;
    mp_mutex_unlock(&wl->translator_lock);

    if (!have_translator || !wl->pipeline) {
        // ① Plain feed (no AI translation in play).
        wl_feed_subtitle_text(wl, text, pts, dur);
        return;
    }

    // ⑤ Slack check at enqueue time; avoid burning API tokens on subtitles
    // that are already too close to playback to land in time.
    uint64_t gen;
    double now_pts;
    mp_mutex_lock(&wl->snap_lock);
    gen = wl->snap.generation;
    now_pts = wl->snap.playback_pts;
    mp_mutex_unlock(&wl->snap_lock);
    if (!isfinite(now_pts))
        now_pts = -INFINITY;

    double slack = pts + dur - now_pts;
    if (isfinite(now_pts) && slack < MIN_TRANSLATE_SLACK_S) {
        MP_INFO(wl, "translate skipped (slack=%.2fs < %.2fs); "
                    "showing original\n", slack, MIN_TRANSLATE_SLACK_S);
        wl_feed_subtitle_text(wl, text, pts, dur);
        return;
    }

    wt_enqueue(wl->pipeline, gen, text, pts, dur);
}

// Parse JSON segments array: [{"s":ms,"e":ms,"t":"text"}, ...]
// `pts_offset` is the timeline pts of the first audio sample fed to whisper
// in the current "session" (i.e. since last filter graph reset). Whisper's
// per-segment timestamps are relative to that origin.
static void process_whisper_segments(struct whisper_lookahead *wl,
                                     const char *json, double pts_offset)
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
            inject_subtitle(wl, norm, pts, dur);
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
                    process_whisper_segments(wl, segments_json, origin);
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
        bool codec_change = gen_change && codec_changed(wl, &snap);
        if (gen_change) {
            MP_INFO(wl, "gen change: %llu -> %llu (codec_change=%d)\n",
                    (unsigned long long)wl->worker_generation,
                    (unsigned long long)snap.generation,
                    (int)codec_change);
            wl->worker_generation = snap.generation;
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
            worker_signal_reset(wl);
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

        // Recheck generation: if it moved while we were under demux lock,
        // skip processing this batch — the worker_loop top will replay.
        struct wl_snap snap2;
        worker_get_snap(wl, &snap2);
        if (snap2.generation != wl->worker_generation) {
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

static void bump_generation(struct whisper_lookahead *wl)
{
    mp_mutex_lock(&wl->snap_lock);
    wl->snap.generation++;
    mp_mutex_unlock(&wl->snap_lock);

    // Wake the worker out of any wait it might be in.
    if (wl->graph_dispatch)
        mp_dispatch_interrupt(wl->graph_dispatch);
    mp_mutex_lock(&wl->queue_lock);
    mp_cond_broadcast(&wl->queue_cv);
    mp_mutex_unlock(&wl->queue_lock);
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
    wl->pub_processed_end = MP_NOPTS_VALUE;
    atomic_store(&wl->init_done, false);
    atomic_store(&wl->terminate, 0);

    mp_mutex_init(&wl->snap_lock);
    mp_mutex_init(&wl->queue_lock);
    mp_mutex_init(&wl->translator_lock);
    mp_cond_init(&wl->queue_cv);

    wl->pipeline = wt_pipeline_create(wl);

    mpctx->whisper_lookahead = wl;

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
}

void whisper_lookahead_on_audio_chain_changed(struct MPContext *mpctx)
{
    struct whisper_lookahead *wl = mpctx->whisper_lookahead;
    if (!wl)
        return;
    // Bump first, THEN publish: any drain that wakes during this window
    // sees a generation that does not match in-flight tasks, so old
    // translations cannot leak onto the new audio chain.
    bump_generation(wl);
    whisper_lookahead_publish(mpctx);
    if (wl->pipeline)
        wt_clear_pending(wl->pipeline);
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

    // Translator identity changed: drop any pending tasks and bump the
    // generation so in-flight worker results are discarded by the drain.
    if (wl->pipeline)
        wt_clear_pending(wl->pipeline);
    bump_generation(wl);
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

        char *buf = NULL;
        if (json_write(&buf, &root) >= 0 && buf)
            out = talloc_strdup(ta_parent, buf);
        talloc_free(buf);
        talloc_free(tmp);
    }
    mp_mutex_unlock(&wl->translator_lock);
    return out;
}
