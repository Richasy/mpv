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

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <mpv/client.h>

#include "config.h"
#include "mpv_talloc.h"

#include "common/common.h"
#include "common/msg.h"
#include "demux/demux.h"
#include "demux/packet.h"
#include "misc/json.h"
#include "misc/node.h"
#include "options/path.h"
#include "sub/dec_sub.h"
#include "sub/ocr_engine.h"
#include "sub/ocr_policy.h"

#include "command.h"
#include "core.h"
#include "sub_translate.h"
#include "sub_ocr.h"
#include "translation.h"

#define SUB_TRANSLATE_PAST_WINDOW 30.0
#define SUB_TRANSLATE_SCAN_MARGIN 5.0

enum cue_state {
    CUE_WAITING = 0,
    CUE_DEFERRED,
    CUE_PENDING,
    CUE_TRANSLATED,
    CUE_EXPIRED,
    CUE_FAILED,
};

struct translated_cue {
    uint64_t id;
    uint64_t revision;
    int read_order;
    double start;
    double duration;
    char *text;
    size_t ass_primary_end;
    char *translated;
    uint64_t request_id;
    struct sub_translate_span *spans;
    int num_spans;
    int next_span;
    char *assembled;
    char *request_text;
    enum cue_state state;
    bool already_target;
};

struct ocr_source_cue {
    uint64_t id;
    double start, duration;
    bool submitted, failed, end_known, policy_observed;
    struct mp_ocr_result *recognized;
};

struct sub_translate_state {
    struct MPContext *mpctx;
    struct mp_log *log;
    bool enabled;
    struct track *source_track;
    struct dec_sub *source_decoder;
    bool source_attached;
    bool secondary_blocked;
    bool replace;
    bool bitmap;
    struct sh_stream *output_stream;
    struct track *output_track;
    struct translated_cue **cues;
    int num_cues;
    int next_read_order;
    uint64_t next_request_id;
    int translated_count;
    double scan_until;
    bool needs_scan;
    bool needs_rebuild;
    char *error;
    char *unsupported;
    char *last_status;
    char *ocr_config;
    char *ocr_model, *ocr_dictionary, *ocr_runtime;
    char *ocr_mode, *ocr_source_lang;
    char *ocr_effective_source, *ocr_target_lang;
    char *ocr_selection;
    bool ocr_ambiguous;
    int ocr_recognized;
    struct sub_ocr_worker *ocr_worker;
    struct ocr_source_cue **ocr_cues;
    int num_ocr_cues;
    bool ocr_reselect;
    struct mp_ocr_policy_context ocr_policy;
};

static void reset_ocr_work(struct sub_translate_state *state);
static void process_ocr(struct sub_translate_state *state);
static void redraw_replacement(struct sub_translate_state *state);
#if HAVE_SUB_OCR
static void on_bitmap_cue(void *ctx, const struct sub_bitmap_cue *source);
static bool select_bitmap_text(struct sub_translate_state *state,
                               struct ocr_source_cue *cue, char **text,
                               bool *already_target);
#endif

static struct sub_translate_state *get_state(struct MPContext *mpctx,
                                             bool create)
{
    if (!mpctx->sub_translate && create) {
        struct sub_translate_state *state =
            talloc_zero(mpctx, struct sub_translate_state);
        state->mpctx = mpctx;
        state->log = mp_log_new(state, mpctx->log, "sub-translate");
        state->scan_until = -INFINITY;
        mpctx->sub_translate = state;
    }
    return mpctx->sub_translate;
}

static void notify_status_if_changed(struct sub_translate_state *state)
{
    if (!state)
        return;
    char *status = sub_translate_get_status(state->mpctx, NULL);
    if (!state->last_status ||
        strcmp(state->last_status, status) != 0)
    {
        talloc_free(state->last_status);
        state->last_status = talloc_strdup(state, status);
        mp_notify_property(state->mpctx, "sub-translate-status");
    }
    talloc_free(status);
}

static void replace_message(char **field, void *parent, const char *message)
{
    talloc_free(*field);
    *field = message && message[0] ? talloc_strdup(parent, message) : NULL;
}

static char *sanitize_error(void *parent, const char *error)
{
    if (!error || !error[0])
        return NULL;
    if (strstr(error, "http://") || strstr(error, "https://") ||
        strstr(error, "api_key"))
    {
        return talloc_strdup(parent, "translation provider error");
    }
    return talloc_asprintf(parent, "%.160s", error);
}

static void set_error(struct sub_translate_state *state, const char *error)
{
    char *safe = sanitize_error(NULL, error);
    replace_message(&state->error, state, safe);
    talloc_free(safe);
}

static void clear_cues(struct sub_translate_state *state)
{
    for (int n = 0; n < state->num_cues; n++)
        talloc_free(state->cues[n]);
    TA_FREEP(&state->cues);
    state->num_cues = 0;
    state->next_read_order = 0;
    state->translated_count = 0;
    state->needs_rebuild = false;
}

static bool owns_secondary(struct sub_translate_state *state,
                           struct track *track)
{
    return track && state->output_stream &&
           track->stream == state->output_stream;
}

static struct track *find_output_track(struct sub_translate_state *state)
{
    if (!state->output_stream)
        return NULL;
    for (int n = 0; n < state->mpctx->num_tracks; n++) {
        struct track *track = state->mpctx->tracks[n];
        if (track->stream == state->output_stream)
            return track;
    }
    return NULL;
}

static bool secondary_conflict(struct sub_translate_state *state)
{
    if (state->replace)
        return false;
    struct track *secondary =
        state->mpctx->current_track[1][STREAM_SUB];
    return secondary && !owns_secondary(state, secondary);
}

static void clear_output(struct sub_translate_state *state, bool deselect)
{
    if (state->bitmap && state->source_decoder) {
        sub_control(state->source_decoder, SD_CTRL_SET_BITMAP_REPLACEMENT, NULL);
        redraw_replacement(state);
    }
    if (state->mpctx->demuxer)
        demux_clear_translated_sub_queue(state->mpctx->demuxer);
    reset_translated_subtitle_track(state->mpctx);

    struct track *secondary =
        state->mpctx->current_track[1][STREAM_SUB];
    if (deselect && owns_secondary(state, secondary))
        mp_switch_track_n(state->mpctx, 1, STREAM_SUB, NULL, 0);
}

static void redraw_replacement(struct sub_translate_state *state)
{
    if ((state->replace || state->bitmap) && state->source_track) {
        state->source_track->redraw_subs = true;
        osd_changed(state->mpctx->osd);
    }
}

static void detach_source(struct sub_translate_state *state)
{
    if (state->source_decoder) {
        sub_control(state->source_decoder, SD_CTRL_SET_TEXT_REPLACEMENT, NULL);
        sub_control(state->source_decoder, SD_CTRL_SET_BITMAP_REPLACEMENT, NULL);
        redraw_replacement(state);
        sub_set_text_cue_callback(state->source_decoder, NULL, NULL);
        sub_set_bitmap_cue_callback(state->source_decoder, NULL, NULL);
    }
    state->source_track = NULL;
    state->source_decoder = NULL;
    state->source_attached = false;
    state->replace = false;
    state->bitmap = false;
    state->secondary_blocked = false;
    state->scan_until = -INFINITY;
    state->needs_scan = false;
    if (state->mpctx->translation) {
        mp_translation_unregister_source(
            state->mpctx->translation,
            MP_TRANSLATION_SOURCE_SUBTITLE);
    }
}

static void reset_source_work(struct sub_translate_state *state,
                              bool deselect_output)
{
    if (state->source_decoder) {
        sub_control(state->source_decoder, SD_CTRL_SET_TEXT_REPLACEMENT, NULL);
        sub_control(state->source_decoder, SD_CTRL_SET_BITMAP_REPLACEMENT, NULL);
        redraw_replacement(state);
    }
    reset_ocr_work(state);
    if (state->mpctx->translation) {
        mp_translation_invalidate_source(
            state->mpctx->translation,
            MP_TRANSLATION_SOURCE_SUBTITLE);
    }
    clear_cues(state);
    clear_output(state, deselect_output);
    state->scan_until = -INFINITY;
    state->needs_scan = true;
}

static struct translated_cue *find_cue(
    struct sub_translate_state *state, uint64_t id)
{
    for (int n = 0; n < state->num_cues; n++) {
        if (state->cues[n]->id == id)
            return state->cues[n];
    }
    return NULL;
}

static bool cue_in_window(struct sub_translate_state *state,
                          const struct sub_text_cue *cue)
{
    double playback = state->mpctx->playback_pts;
    if (!isfinite(playback))
        playback = 0;
    struct mp_translation_limits limits;
    mp_translation_get_limits(state->mpctx->translation, &limits);
    double horizon = limits.horizon_sec > 0
        ? limits.horizon_sec : 60;
    double end = cue->start + cue->duration;
    return cue->start <= playback + horizon &&
           end >= playback - SUB_TRANSLATE_PAST_WINDOW;
}

static bool cue_already_ended(struct sub_translate_state *state,
                              const struct sub_text_cue *cue)
{
    double playback = state->mpctx->playback_pts;
    double end = cue->start + cue->duration;
    return isfinite(playback) && isfinite(cue->start) &&
           isfinite(cue->duration) && cue->duration >= 0 &&
           isfinite(end) && end < playback;
}

static enum mp_translation_submit_result submit_cue(
    struct sub_translate_state *state, struct translated_cue *cue)
{
    if (cue->already_target)
        return MP_TRANSLATION_SUBMIT_QUEUED;
    enum mp_translation_submit_result result =
        mp_translation_submit(
            state->mpctx->translation,
            MP_TRANSLATION_SOURCE_SUBTITLE,
            cue->request_id, cue->revision,
            state->replace ? cue->request_text : cue->text,
            cue->start, cue->duration, 0);
    if (result == MP_TRANSLATION_SUBMIT_QUEUED) {
        cue->state = CUE_PENDING;
        return result;
    }
    if (result == MP_TRANSLATION_SUBMIT_BACKPRESSURE)
        cue->state = CUE_DEFERRED;
    else if (result == MP_TRANSLATION_SUBMIT_TOO_LATE)
        cue->state = CUE_EXPIRED;
    else
        cue->state = CUE_WAITING;
    if (result == MP_TRANSLATION_SUBMIT_NO_BACKEND) {
        set_error(state, "sub-translate-config is disabled");
    } else if (result == MP_TRANSLATION_SUBMIT_TOO_LATE) {
        set_error(state, "translation skipped because the cue is too late");
    } else if (result == MP_TRANSLATION_SUBMIT_INACTIVE) {
        set_error(state, "subtitle translation source is inactive");
    }
    return result;
}

static void prepare_span(struct sub_translate_state *state,
                         struct translated_cue *cue)
{
    struct sub_translate_span span = cue->spans[cue->next_span];
    talloc_free(cue->request_text);
    cue->request_text = talloc_strndup(cue, cue->text + span.start, span.length);
    cue->request_id = ++state->next_request_id;
}

static void accept_source_cue(void *ctx, const struct sub_text_cue *source,
                              bool already_target)
{
    struct sub_translate_state *state = ctx;
    if (!state->enabled || !state->source_decoder ||
        state->secondary_blocked ||
        !cue_in_window(state, source) ||
        cue_already_ended(state, source))
    {
        return;
    }

    const char *text = state->replace ? source->ass : source->text;
    if (!text)
        return;
    struct translated_cue *cue = find_cue(state, source->id);
    if (!cue) {
        cue = talloc_zero(NULL, struct translated_cue);
        cue->id = source->id;
        cue->revision = 1;
        cue->read_order = state->next_read_order++;
        MP_TARRAY_APPEND(state, state->cues, state->num_cues, cue);
    } else if (cue->start == source->start &&
               cue->duration == source->duration &&
               cue->ass_primary_end == source->ass_primary_end &&
               strcmp(cue->text, text) == 0 &&
               cue->already_target == already_target)
    {
        if (cue->state == CUE_WAITING)
            submit_cue(state, cue);
        if (state->replace && cue->state == CUE_TRANSLATED)
            state->needs_rebuild = true;
        notify_status_if_changed(state);
        return;
    } else {
        cue->revision++;
        if (cue->state == CUE_TRANSLATED) {
            state->needs_rebuild = true;
            state->translated_count--;
        }
        talloc_free(cue->translated);
        cue->translated = NULL;
    }

    cue->start = source->start;
    cue->duration = source->duration;
    cue->ass_primary_end = source->ass_primary_end;
    cue->already_target = already_target;
    talloc_free(cue->text);
    cue->text = talloc_strdup(cue, text);
    cue->request_id = ++state->next_request_id;
    if (already_target) {
        cue->translated = talloc_strdup(cue, text);
        cue->state = CUE_TRANSLATED;
        state->translated_count++;
        state->needs_rebuild = true;
        notify_status_if_changed(state);
        return;
    }
    if (state->replace) {
        TA_FREEP(&cue->spans);
        TA_FREEP(&cue->assembled);
        cue->num_spans = sub_translate_ass_spans(text, NULL, 0);
        cue->next_span = 0;
        if (cue->num_spans <= 0) {
            cue->state = CUE_FAILED;
            return;
        }
        cue->spans = talloc_array(cue, struct sub_translate_span, cue->num_spans);
        sub_translate_ass_spans(text, cue->spans, cue->num_spans);
        if (cue->ass_primary_end) {
            int count = 0;
            while (count < cue->num_spans &&
                   cue->spans[count].start + cue->spans[count].length <=
                       cue->ass_primary_end)
                count++;
            cue->num_spans = count;
            if (!count) {
                cue->state = CUE_FAILED;
                return;
            }
        }
        cue->assembled = talloc_strdup(cue, "");
        prepare_span(state, cue);
    }
    cue->state = CUE_WAITING;
    submit_cue(state, cue);
    notify_status_if_changed(state);
}

static void on_text_cue(void *ctx, const struct sub_text_cue *source)
{
    accept_source_cue(ctx, source, false);
}

static bool feed_cue(struct sub_translate_state *state,
                     struct translated_cue *cue)
{
    if (!state->mpctx->demuxer || !cue->translated ||
        !state->output_track || !state->output_track->d_sub)
    {
        return false;
    }
    if (secondary_conflict(state)) {
        set_error(state, "secondary subtitle track is already selected");
        return false;
    }

    double pts;
    double duration;
    if (!sub_map_player_cue_to_subtitle(
            state->output_track->d_sub, cue->start, cue->duration,
            &pts, &duration))
    {
        set_error(state, "translated subtitle timing is unavailable");
        return false;
    }

    char *escaped = sub_translate_escape_ass(NULL, cue->translated);
    char *line = talloc_asprintf(
        NULL, "%d,0,Default,,0,0,0,,%s",
        cue->read_order, escaped);
    talloc_free(escaped);
    struct demux_packet *packet = new_demux_packet_from(
        state->mpctx->demuxer->packet_pool,
        line, strlen(line));
    if (!packet) {
        talloc_free(line);
        set_error(state, "translated subtitle packet allocation failed");
        return false;
    }
    packet->pts = pts;
    packet->dts = pts;
    packet->duration = duration;
    packet->sub_duration = duration;
    demuxer_feed_translated_sub(state->mpctx->demuxer, packet);
    talloc_free(line);
    return true;
}

static void rebuild_output(struct sub_translate_state *state)
{
    if (state->replace) {
        sub_control(state->source_decoder, SD_CTRL_SET_TEXT_REPLACEMENT, NULL);
        for (int n = 0; n < state->num_cues; n++) {
            struct translated_cue *cue = state->cues[n];
            if (cue->state != CUE_TRANSLATED)
                continue;
            struct sub_text_replacement replacement = {
                .id = cue->id,
                .source = cue->text,
                .text = cue->translated,
            };
            sub_control(state->source_decoder, SD_CTRL_SET_TEXT_REPLACEMENT,
                        &replacement);
        }
        redraw_replacement(state);
        state->needs_rebuild = false;
        return;
    }
    if (!state->output_stream && state->mpctx->demuxer) {
        state->output_stream =
            demuxer_ensure_translated_sub(state->mpctx->demuxer);
    }
    if (!state->output_stream) {
        set_error(state, "translated subtitle track is unavailable");
        state->needs_rebuild = true;
        return;
    }
    if (!state->output_track)
        state->output_track = find_output_track(state);
    if (!state->output_track || !state->output_track->d_sub) {
        state->needs_rebuild = true;
        return;
    }
    clear_output(state, false);
    if (state->bitmap)
        sub_control(state->source_decoder, SD_CTRL_SET_BITMAP_REPLACEMENT, NULL);
    for (int n = 0; n < state->num_cues; n++) {
        if (state->cues[n]->state == CUE_TRANSLATED) {
            bool fed = feed_cue(state, state->cues[n]);
            if (fed && state->bitmap)
                sub_control(state->source_decoder, SD_CTRL_SET_BITMAP_REPLACEMENT,
                            &state->cues[n]->id);
        }
    }
    redraw_replacement(state);
    state->needs_rebuild = false;
}

static void accept_result(
    void *ctx, const struct mp_translation_result *result)
{
    struct sub_translate_state *state = ctx;
    struct translated_cue *cue = NULL;
    for (int n = 0; n < state->num_cues; n++) {
        if (state->cues[n]->request_id == result->cue_id) {
            cue = state->cues[n];
            break;
        }
    }
    if (!cue || cue->revision != result->revision)
        return;
    if (state->bitmap && isfinite(state->mpctx->playback_pts) &&
        cue->start + cue->duration < state->mpctx->playback_pts)
    {
        cue->state = CUE_EXPIRED;
        return;
    }
    cue->state = CUE_WAITING;
    if (!result->translated) {
        if (state->replace)
            cue->state = CUE_FAILED;
        if (result->kind == MP_TRANSLATION_RESULT_FALLBACK_FAILURE)
            set_error(state, result->error
                ? result->error : "translation provider failed");
        else if (result->kind == MP_TRANSLATION_RESULT_FALLBACK_RPM ||
                 result->kind == MP_TRANSLATION_RESULT_FALLBACK_BUDGET ||
                 result->kind == MP_TRANSLATION_RESULT_FALLBACK_QUEUE)
            set_error(state, "translation request skipped by limits");
        else if (result->kind == MP_TRANSLATION_RESULT_FALLBACK_LATE) {
            cue->state = CUE_EXPIRED;
            set_error(state, "translation arrived after cue end");
        }
        return;
    }

    if (state->replace) {
        if (!result->translated[0]) {
            cue->state = CUE_FAILED;
            set_error(state, "translation provider returned empty text");
            return;
        }
        struct sub_translate_span span = cue->spans[cue->next_span];
        size_t previous_end = cue->next_span
            ? cue->spans[cue->next_span - 1].start +
              cue->spans[cue->next_span - 1].length : 0;
        cue->assembled = talloc_strndup_append(
            cue->assembled, cue->text + previous_end, span.start - previous_end);
        char *escaped = sub_translate_escape_ass(NULL, result->translated);
        cue->assembled = talloc_strdup_append(cue->assembled, escaped);
        talloc_free(escaped);
        if (++cue->next_span < cue->num_spans) {
            prepare_span(state, cue);
            submit_cue(state, cue);
            return;
        }
        cue->assembled = talloc_strdup_append(
            cue->assembled, cue->text + span.start + span.length);
    }
    talloc_free(cue->translated);
    cue->translated = talloc_strdup(
        cue, state->replace ? cue->assembled : result->translated);
    cue->state = CUE_TRANSLATED;
    state->translated_count++;
    replace_message(&state->error, state, NULL);
    state->needs_rebuild = true;
}

static void prune_cues(struct sub_translate_state *state, double playback)
{
    for (int n = 0; n < state->num_cues; ) {
        struct translated_cue *cue = state->cues[n];
        if (cue->start + cue->duration <
            playback - SUB_TRANSLATE_PAST_WINDOW)
        {
            if (state->replace) {
                struct sub_text_replacement replacement = {.id = cue->id};
                sub_control(state->source_decoder, SD_CTRL_SET_TEXT_REPLACEMENT,
                            &replacement);
                if (cue->state == CUE_TRANSLATED)
                    state->translated_count--;
            }
            talloc_free(cue);
            MP_TARRAY_REMOVE_AT(state->cues, state->num_cues, n);
        } else {
            n++;
        }
    }
}

static void retry_deferred_cues(struct sub_translate_state *state)
{
    for (int n = 0; n < state->num_cues; n++) {
        struct translated_cue *cue = state->cues[n];
        if (cue->state != CUE_DEFERRED)
            continue;
        if (submit_cue(state, cue) ==
            MP_TRANSLATION_SUBMIT_BACKPRESSURE)
        {
            break;
        }
    }
}

static bool source_ready(struct sub_translate_state *state)
{
    return state->enabled && state->source_track &&
           state->source_decoder && state->source_attached &&
           !state->secondary_blocked && !state->unsupported;
}

static void reset_ocr_work(struct sub_translate_state *state)
{
#if HAVE_SUB_OCR
    sub_ocr_worker_invalidate(state->ocr_worker);
#endif
    for (int n = 0; n < state->num_ocr_cues; n++)
        talloc_free(state->ocr_cues[n]);
    TA_FREEP(&state->ocr_cues);
    state->num_ocr_cues = 0;
    state->ocr_recognized = 0;
    state->ocr_ambiguous = false;
    state->ocr_reselect = false;
    state->ocr_policy = (struct mp_ocr_policy_context){0};
    replace_message(&state->ocr_selection, state, NULL);
}

#if HAVE_SUB_OCR
static struct ocr_source_cue *find_ocr_cue(struct sub_translate_state *state,
                                         uint64_t id)
{
    for (int n = 0; n < state->num_ocr_cues; n++) {
        if (state->ocr_cues[n]->id == id)
            return state->ocr_cues[n];
    }
    return NULL;
}

static void on_bitmap_cue(void *ctx, const struct sub_bitmap_cue *source)
{
    struct sub_translate_state *state = ctx;
    if (!source_ready(state) || !state->bitmap || !state->ocr_worker)
        return;
    struct sub_text_cue timing = {
        .id = source->id, .start = source->start, .duration = source->duration,
    };
    struct ocr_source_cue *cue = find_ocr_cue(state, source->id);
    if (!cue && (!cue_in_window(state, &timing) ||
                 cue_already_ended(state, &timing)))
        return;
    if (!cue) {
        if (state->num_ocr_cues >= 256) {
            set_error(state, "bitmap OCR event cache is full");
            return;
        }
        cue = talloc_zero(NULL, struct ocr_source_cue);
        cue->id = source->id;
        MP_TARRAY_APPEND(state, state->ocr_cues, state->num_ocr_cues, cue);
    }
    if (cue->start != source->start || cue->duration != source->duration ||
        cue->end_known != source->end_known)
    {
        state->ocr_reselect = true;
        struct translated_cue *output = find_cue(state, cue->id);
        if (output) {
            output->start = source->start;
            output->duration = source->duration;
            state->needs_rebuild = true;
        }
    }
    cue->start = source->start;
    cue->duration = source->duration;
    cue->end_known = source->end_known;
    if (!cue->submitted && !cue->failed && !cue->recognized &&
        !cue_already_ended(state, &timing))
    {
        enum sub_ocr_submit_result result =
            sub_ocr_worker_submit(state->ocr_worker, source);
        cue->submitted = result == SUB_OCR_QUEUED;
        if (result == SUB_OCR_INVALID) {
            cue->failed = true;
            set_error(state, "bitmap OCR rejected an invalid or oversized event");
        }
    }
}
#endif

static void process_ocr(struct sub_translate_state *state)
{
#if HAVE_SUB_OCR
    if (!state->bitmap || !state->ocr_worker || !source_ready(state))
        return;
    double playback = state->mpctx->playback_pts;
    if (!isfinite(playback))
        playback = 0;
    for (int n = 0; n < state->num_ocr_cues; ) {
        struct ocr_source_cue *cue = state->ocr_cues[n];
        if (cue->start + cue->duration < playback - SUB_TRANSLATE_PAST_WINDOW) {
            talloc_free(cue);
            MP_TARRAY_REMOVE_AT(state->ocr_cues, state->num_ocr_cues, n);
        } else {
            n++;
        }
    }
    struct mp_translation_limits limits;
    mp_translation_get_limits(state->mpctx->translation, &limits);
    sub_emit_bitmap_cues(state->source_decoder,
        playback - SUB_TRANSLATE_PAST_WINDOW,
        playback + (limits.horizon_sec > 0 ? limits.horizon_sec : 60));
    bool changed = state->ocr_reselect;
    state->ocr_reselect = false;
    struct sub_ocr_completion *completion;
    while ((completion = sub_ocr_worker_poll(state->ocr_worker))) {
        struct ocr_source_cue *cue = find_ocr_cue(state, completion->id);
        if (cue) {
            cue->submitted = false;
            if (completion->result->error || !completion->result->num_lines) {
                cue->failed = true;
                set_error(state, completion->result->error
                    ? completion->result->error : "bitmap OCR found no text");
            } else {
                cue->recognized = talloc_steal(cue, completion->result);
                state->ocr_recognized++;
                changed = true;
            }
        }
        talloc_free(completion);
    }
    if (changed) {
        for (int n = 0; n < state->num_ocr_cues; n++) {
            struct ocr_source_cue *cue = state->ocr_cues[n];
            if (cue->recognized && !cue->policy_observed && cue->end_known) {
                char *text = NULL;
                bool already_target = false;
                select_bitmap_text(state, cue, &text, &already_target);
                talloc_free(text);
            }
        }
        state->ocr_ambiguous = false;
        for (int n = 0; n < state->num_ocr_cues; n++) {
            struct ocr_source_cue *cue = state->ocr_cues[n];
            if (!cue->recognized)
                continue;
            char *text = NULL;
            bool already_target = false;
            if (select_bitmap_text(state, cue, &text, &already_target) &&
                cue->start + cue->duration >= playback)
            {
                struct sub_text_cue source = {
                    .id = cue->id, .start = cue->start,
                    .duration = cue->duration, .text = text,
                };
                accept_source_cue(state, &source, already_target);
            }
            talloc_free(text);
        }
    }
#endif
}

#if HAVE_SUB_OCR
static bool select_bitmap_text(struct sub_translate_state *state,
                               struct ocr_source_cue *cue, char **text,
                               bool *already_target)
{
    if (cue->recognized->num_lines > MP_OCR_POLICY_MAX_LINES) {
        set_error(state, "bitmap OCR returned too many text lines");
        return false;
    }
    struct mp_ocr_policy_line lines[MP_OCR_POLICY_MAX_LINES];
    size_t capacity = 1;
    for (int n = 0; n < cue->recognized->num_lines; n++) {
        const struct mp_ocr_line *line = &cue->recognized->lines[n];
        size_t length = strlen(line->text);
        if (capacity >= 65536 || length >= 65536 - capacity) {
            set_error(state, "bitmap OCR text exceeds the event limit");
            return false;
        }
        capacity += length + 1;
        lines[n] = (struct mp_ocr_policy_line){
            .text = line->text, .x = line->x, .y = line->y,
            .w = line->w, .h = line->h, .region = line->region,
            .confidence = line->confidence,
        };
    }
    enum mp_ocr_selection_mode mode = MP_OCR_SELECT_AUTO;
    if (state->ocr_mode && !strcmp(state->ocr_mode, "source"))
        mode = MP_OCR_SELECT_SOURCE;
    else if (state->ocr_mode && !strcmp(state->ocr_mode, "full"))
        mode = MP_OCR_SELECT_FULL;
    struct mp_ocr_policy_input input = {
        .lines = lines, .num_lines = cue->recognized->num_lines,
        .cue_id = cue->id, .revision = 1,
        .start = cue->start, .duration = cue->duration,
        .mode = mode, .source_lang = state->ocr_effective_source,
        .target_lang = state->ocr_target_lang,
        .track_lang = state->source_track ? state->source_track->lang : NULL,
    };
    struct mp_ocr_policy_context query = state->ocr_policy;
    bool observe = !cue->policy_observed && cue->end_known;
    input.reevaluate = !observe;
    struct mp_ocr_policy_context *context = observe ? &state->ocr_policy : &query;
    struct mp_ocr_policy_result selection;
    *text = talloc_size(NULL, capacity);
    if (!mp_ocr_policy_select(context, &input, *text, capacity,
                              NULL, 0, &selection))
    {
        set_error(state, mp_ocr_policy_reason_string(selection.reason));
        TA_FREEP(text);
        return false;
    }
    cue->policy_observed |= observe;
    if (!(*text)[0]) {
        set_error(state, "bitmap OCR selected empty text");
        TA_FREEP(text);
        return false;
    }
    *already_target = selection.already_target;
    double playback = state->mpctx->playback_pts;
    if (!isfinite(playback) || cue->start + cue->duration >= playback) {
        state->ocr_ambiguous |= selection.ambiguous;
        replace_message(&state->ocr_selection, state,
                        mp_ocr_policy_reason_string(selection.reason));
    }
    return true;
}

static void refresh_ocr_languages(struct sub_translate_state *state)
{
    void *tmp = talloc_new(NULL);
    char *json = sub_translate_get_config(state->mpctx, tmp);
    char *cursor = json;
    struct mpv_node root = {0};
    const char *source = "auto", *target = "";
    if (json && json_parse(tmp, &root, &cursor, MAX_JSON_DEPTH) >= 0 &&
        root.format == MPV_FORMAT_NODE_MAP)
    {
        struct mpv_node *s = node_map_get(&root, "source_lang");
        struct mpv_node *t = node_map_get(&root, "target_lang");
        if (s && s->format == MPV_FORMAT_STRING)
            source = s->u.string;
        if (t && t->format == MPV_FORMAT_STRING)
            target = t->u.string;
        struct mpv_node *provider = node_map_get(&root, "provider");
        struct mpv_node *ai = node_map_get(&root, "ai");
        if (provider && provider->format == MPV_FORMAT_STRING &&
            !strcmp(provider->u.string, "ai") && ai &&
            ai->format == MPV_FORMAT_NODE_MAP)
        {
            s = node_map_get(ai, "source_lang");
            t = node_map_get(ai, "target_lang");
            if (s && s->format == MPV_FORMAT_STRING && s->u.string[0])
                source = s->u.string;
            if (t && t->format == MPV_FORMAT_STRING && t->u.string[0])
                target = t->u.string;
        }
    }
    if (state->ocr_source_lang && strcmp(state->ocr_source_lang, "auto"))
        source = state->ocr_source_lang;
    replace_message(&state->ocr_effective_source, state, source);
    replace_message(&state->ocr_target_lang, state, target);
    talloc_free(tmp);
}
#endif

static void attach_current_source(struct sub_translate_state *state)
{
    struct MPContext *mpctx = state->mpctx;
    struct track *track = mpctx->current_track[0][STREAM_SUB];
    bool same_source =
        track == state->source_track &&
        (!track || track->d_sub == state->source_decoder);
    if (same_source) {
        if (!track) {
            struct mp_translation *translation =
                mpctx_get_translation(mpctx);
            if (!mp_translation_common_config_enabled(translation))
                set_error(state,
                    mp_translation_common_config_explicit(translation)
                        ? "sub-translate-config is disabled"
                        : "sub-translate-config is not configured");
            else
                replace_message(&state->error, state, NULL);
            return;
        }
        if (state->source_attached || state->unsupported)
            return;
        struct mp_translation *translation =
            mpctx_get_translation(mpctx);
        if (!mp_translation_common_config_enabled(translation) ||
            secondary_conflict(state))
        {
            return;
        }
    }

    detach_source(state);
    reset_source_work(state, true);
    replace_message(&state->error, state, NULL);
    replace_message(&state->unsupported, state, NULL);
    if (!state->enabled)
        return;

    if (track && track->d_sub) {
        state->source_track = track;
        state->source_decoder = track->d_sub;
        const char *codec = track->stream && track->stream->codec
            ? track->stream->codec->codec : NULL;
        state->replace = codec &&
            (!strcmp(codec, "ass") || !strcmp(codec, "ssa"));
        const char *profile =
            track->stream && track->stream->codec
                ? track->stream->codec->codec_profile : NULL;
        if (sub_translate_is_generated_profile(profile)) {
            replace_message(&state->unsupported, state,
                            "generated subtitle tracks are excluded");
            return;
        }
    }

    struct mp_translation *translation = mpctx_get_translation(mpctx);
    if (!mp_translation_common_config_explicit(translation) ||
        !mp_translation_common_config_enabled(translation))
    {
        set_error(state,
            mp_translation_common_config_explicit(translation)
                ? "sub-translate-config is disabled"
                : "sub-translate-config is not configured");
        return;
    }
    if (!track || !track->d_sub)
        return;
    if (secondary_conflict(state)) {
        set_error(state, "secondary subtitle track is already selected");
        return;
    }
    if (!sub_set_text_cue_callback(track->d_sub, on_text_cue, state)) {
#if HAVE_SUB_OCR
        if (!state->ocr_model || !state->ocr_dictionary) {
            replace_message(&state->unsupported, state,
                            "selected subtitle track is bitmap-based; configure sub-ocr-config");
            return;
        }
        if (!sub_set_bitmap_cue_callback(track->d_sub, on_bitmap_cue, state)) {
            replace_message(&state->unsupported, state,
                            "selected subtitle decoder has no bitmap OCR tap");
            return;
        }
        if (!state->ocr_worker)
            state->ocr_worker = sub_ocr_worker_create(
                state->log, state->ocr_model, state->ocr_dictionary,
                state->ocr_runtime, mp_wakeup_core_cb, mpctx);
        if (!state->ocr_worker) {
            sub_set_bitmap_cue_callback(track->d_sub, NULL, NULL);
            set_error(state, "could not start bitmap OCR worker");
            return;
        }
        state->bitmap = true;
        refresh_ocr_languages(state);
#else
        replace_message(&state->unsupported, state,
                        "selected subtitle track is bitmap-based; OCR is unavailable in this build");
        return;
#endif
    }

    mp_translation_register_source(
        translation, MP_TRANSLATION_SOURCE_SUBTITLE);
    state->source_attached = true;
    state->secondary_blocked = false;
    state->needs_scan = true;
}

bool sub_translate_get_enabled(struct MPContext *mpctx)
{
    struct sub_translate_state *state = get_state(mpctx, false);
    return state && state->enabled;
}

void sub_translate_set_enabled(struct MPContext *mpctx, bool enabled)
{
    struct sub_translate_state *state = get_state(mpctx, true);
    if (!state || state->enabled == enabled)
        return;
    state->enabled = enabled;
    replace_message(&state->error, state, NULL);
    replace_message(&state->unsupported, state, NULL);
    if (!enabled) {
        detach_source(state);
        reset_source_work(state, true);
    } else {
        attach_current_source(state);
    }
    notify_status_if_changed(state);
    mp_wakeup_core(mpctx);
}

int sub_translate_set_config(struct MPContext *mpctx, const char *json,
                             char **error)
{
    struct mp_translation *translation = mpctx_get_translation(mpctx);
    if (!translation)
        return -1;
    if (mp_translation_set_common_config(translation, json, error) < 0)
        return -1;

    if (mpctx->whisper_lookahead)
        whisper_lookahead_invalidate(mpctx, "sub-translate-config-changed");
    struct sub_translate_state *state = get_state(mpctx, false);
    if (state) {
        detach_source(state);
        reset_source_work(state, true);
        replace_message(&state->error, state, NULL);
        replace_message(&state->unsupported, state, NULL);
        if (state->enabled)
            attach_current_source(state);
        notify_status_if_changed(state);
    }
    mp_wakeup_core(mpctx);
    return 0;
}

char *sub_translate_get_config(struct MPContext *mpctx, void *talloc_parent)
{
    return mp_translation_get_common_config(
        mpctx ? mpctx->translation : NULL, talloc_parent);
}

int sub_translate_set_ocr_config(struct MPContext *mpctx, const char *json,
                                 char **error)
{
    if (!json || strlen(json) > 16384) {
        *error = talloc_strdup(NULL, "OCR configuration exceeds 16384 bytes");
        return -1;
    }
    void *tmp = talloc_new(NULL);
    struct mpv_node root = {0};
    char *cursor = (char *)json;
    const char *message = NULL;
    const char *model = NULL, *dictionary = NULL, *runtime = NULL;
    const char *mode = "auto", *language = "auto";
    bool enabled = json[0] != '\0';
    if (enabled) {
        if (!json_validate_strict(json, MAX_JSON_DEPTH) ||
            json_parse(tmp, &root, &cursor, MAX_JSON_DEPTH) < 0 ||
            root.format != MPV_FORMAT_NODE_MAP)
        {
            message = "OCR configuration must be a JSON object";
            goto invalid;
        }
        for (int n = 0; n < root.u.list->num; n++) {
            const char *key = root.u.list->keys[n];
            struct mpv_node *value = &root.u.list->values[n];
            for (int i = 0; i < n; i++) {
                if (!strcmp(key, root.u.list->keys[i])) {
                    message = "duplicate OCR configuration field";
                    goto invalid;
                }
            }
            if (value->format != MPV_FORMAT_STRING || !value->u.string[0]) {
                message = "OCR configuration values must be nonempty strings";
                goto invalid;
            }
            if (!strcmp(key, "model"))
                model = value->u.string;
            else if (!strcmp(key, "dictionary"))
                dictionary = value->u.string;
            else if (!strcmp(key, "runtime"))
                runtime = value->u.string;
            else if (!strcmp(key, "mode"))
                mode = value->u.string;
            else if (!strcmp(key, "source_lang"))
                language = value->u.string;
            else {
                message = "unknown OCR configuration field";
                goto invalid;
            }
        }
        enabled = root.u.list->num > 0;
        if (enabled && (!model || !dictionary)) {
            message = "OCR model and dictionary are required";
            goto invalid;
        }
        if (strcmp(mode, "auto") && strcmp(mode, "source") &&
            strcmp(mode, "full"))
        {
            message = "OCR mode must be auto, source or full";
            goto invalid;
        }
        if (!strcmp(mode, "source") && !strcmp(language, "auto")) {
            message = "source mode requires an explicit OCR source_lang";
            goto invalid;
        }
        if (strlen(language) > 63) {
            message = "OCR source language is too long";
            goto invalid;
        }
    }
#if !HAVE_SUB_OCR
    if (enabled) {
        message = "bitmap OCR is unavailable in this build";
        goto invalid;
    }
#endif
    struct sub_translate_state *state = get_state(mpctx, true);
    detach_source(state);
    reset_source_work(state, true);
#if HAVE_SUB_OCR
    sub_ocr_worker_destroy(state->ocr_worker);
    state->ocr_worker = NULL;
#endif
    TA_FREEP(&state->ocr_model);
    TA_FREEP(&state->ocr_dictionary);
    TA_FREEP(&state->ocr_runtime);
    if (enabled) {
        state->ocr_model = mp_normalize_user_path(state, mpctx->global, model);
        state->ocr_dictionary = mp_normalize_user_path(
            state, mpctx->global, dictionary);
        if (runtime)
            state->ocr_runtime = mp_normalize_user_path(
                state, mpctx->global, runtime);
    }
    replace_message(&state->ocr_mode, state, mode);
    replace_message(&state->ocr_source_lang, state, language);
    replace_message(&state->ocr_config, state, enabled ? json : "{}");
    replace_message(&state->error, state, NULL);
    replace_message(&state->unsupported, state, NULL);
    if (state->enabled)
        attach_current_source(state);
    notify_status_if_changed(state);
    mp_notify_property(mpctx, "sub-ocr-config");
    mp_wakeup_core(mpctx);
    talloc_free(tmp);
    return 0;

invalid:
    *error = talloc_strdup(NULL, message);
    talloc_free(tmp);
    return -1;
}

char *sub_translate_get_ocr_config(struct MPContext *mpctx, void *talloc_parent)
{
    struct sub_translate_state *state = get_state(mpctx, false);
    return talloc_strdup(talloc_parent,
        state && state->ocr_config ? state->ocr_config : "{}");
}

void sub_translate_update(struct MPContext *mpctx)
{
    struct sub_translate_state *state = get_state(mpctx, false);
    if (mpctx->translation) {
        mp_translation_set_playback_pts(
            mpctx->translation, mpctx->playback_pts);
    }
    if (!state || !state->enabled)
        return;

    attach_current_source(state);
    if (state->output_stream && !state->output_track)
        state->output_track = find_output_track(state);
    if (state->output_track && !state->replace) {
        struct track *secondary = mpctx->current_track[1][STREAM_SUB];
        if (!secondary)
            mp_switch_track_n(mpctx, 1, STREAM_SUB,
                              state->output_track, 0);
        else if (!owns_secondary(state, secondary))
            set_error(state, "secondary subtitle track is already selected");
    }

    bool conflict = secondary_conflict(state);
    if (conflict && !state->secondary_blocked) {
        state->secondary_blocked = true;
        mp_translation_invalidate_source(
            mpctx->translation, MP_TRANSLATION_SOURCE_SUBTITLE);
        clear_output(state, false);
        state->needs_rebuild = true;
        set_error(state, "secondary subtitle track is already selected");
    } else if (!conflict && state->secondary_blocked) {
        state->secondary_blocked = false;
        replace_message(&state->error, state, NULL);
        state->needs_scan = true;
    }

    if (!source_ready(state)) {
        notify_status_if_changed(state);
        return;
    }

    mp_translation_drain(mpctx->translation,
                         MP_TRANSLATION_SOURCE_SUBTITLE,
                         accept_result, state);
    process_ocr(state);
    retry_deferred_cues(state);
    if (state->needs_rebuild)
        rebuild_output(state);

    double playback = mpctx->playback_pts;
    if (!isfinite(playback))
        playback = 0;
    prune_cues(state, playback);

    struct mp_translation_limits limits;
    mp_translation_get_limits(mpctx->translation, &limits);
    double horizon = limits.horizon_sec > 0
        ? limits.horizon_sec : 60;
    double wanted_end = playback + horizon;
    if (!state->bitmap && (state->needs_scan ||
        wanted_end > state->scan_until - SUB_TRANSLATE_SCAN_MARGIN)
    ) {
        state->needs_scan = false;
        state->scan_until = wanted_end;
        sub_emit_text_cues(state->source_decoder,
                           playback - SUB_TRANSLATE_PAST_WINDOW,
                           wanted_end);
    }
    notify_status_if_changed(state);
}

void sub_translate_seek(struct MPContext *mpctx)
{
    struct sub_translate_state *state = get_state(mpctx, false);
    if (!state || !state->enabled)
        return;
    reset_source_work(state, false);
    notify_status_if_changed(state);
}

void sub_translate_on_sub_reinit(struct MPContext *mpctx,
                                 struct track *track)
{
    struct sub_translate_state *state = get_state(mpctx, false);
    if (state && state->enabled &&
        mpctx->current_track[0][STREAM_SUB] == track)
    {
        attach_current_source(state);
        notify_status_if_changed(state);
    }
}

void sub_translate_on_sub_uninit(struct MPContext *mpctx,
                                 struct track *track)
{
    struct sub_translate_state *state = get_state(mpctx, false);
    if (!state || track != state->source_track)
        return;
    detach_source(state);
    reset_source_work(state, false);
    notify_status_if_changed(state);
}

void sub_translate_stop_file(struct MPContext *mpctx)
{
    struct sub_translate_state *state = get_state(mpctx, false);
    if (!state)
        return;
    detach_source(state);
    reset_source_work(state, false);
    state->output_stream = NULL;
    state->output_track = NULL;
    replace_message(&state->error, state, NULL);
    replace_message(&state->unsupported, state, NULL);
    notify_status_if_changed(state);
}

void sub_translate_destroy(struct MPContext *mpctx)
{
    if (!mpctx || !mpctx->sub_translate)
        return;
    sub_translate_stop_file(mpctx);
#if HAVE_SUB_OCR
    sub_ocr_worker_destroy(mpctx->sub_translate->ocr_worker);
#endif
    talloc_free(mpctx->sub_translate);
    mpctx->sub_translate = NULL;
}

char *sub_translate_get_status(struct MPContext *mpctx, void *talloc_parent)
{
    struct sub_translate_state *state = get_state(mpctx, false);
    const char *name = "disabled";
    int source_sid = -1;
    int output_sid = -1;
    int translated = 0;
    int pending = 0;
    const char *error = NULL;

    if (state) {
        const char *profile =
            state->source_track && state->source_track->stream &&
            state->source_track->stream->codec
                ? state->source_track->stream->codec->codec_profile : NULL;
        if (state->source_track &&
            !sub_translate_is_generated_profile(profile))
            source_sid = state->source_track->user_tid;
        if (state->output_track && !state->replace)
            output_sid = state->output_track->user_tid;
        translated = state->translated_count;
        if (mpctx->translation) {
            pending = mp_translation_pending_count(
                mpctx->translation,
                MP_TRANSLATION_SOURCE_SUBTITLE);
        }
        for (int n = 0; n < state->num_cues; n++)
            pending += state->cues[n]->state == CUE_DEFERRED;
#if HAVE_SUB_OCR
        pending += sub_ocr_worker_pending(state->ocr_worker);
#endif
        error = state->error ? state->error : state->unsupported;
        if (!state->enabled) {
            name = "disabled";
        } else if (state->unsupported) {
            name = "unsupported";
        } else if (state->error) {
            name = "error";
        } else if (pending > 0) {
            name = translated > 0 ? "active" : "translating";
        } else if (translated > 0 && (state->replace || state->output_track)) {
            name = "active";
        } else {
            name = "idle";
        }
    }

    void *tmp = talloc_new(NULL);
    struct mpv_node root = {0};
    node_init(&root, MPV_FORMAT_NODE_MAP, NULL);
    talloc_steal(tmp, root.u.list);
    node_map_add_string(&root, "state", name);
    node_map_add_string(&root, "presentation",
                        state && state->bitmap ? "bitmap-text" :
                        state && state->replace ? "replace" : "companion");
    node_map_add_flag(&root, "ocr_available", HAVE_SUB_OCR);
    node_map_add_int64(&root, "ocr_recognized", state ? state->ocr_recognized : 0);
    node_map_add_flag(&root, "ocr_ambiguous", state && state->ocr_ambiguous);
    if (state && state->ocr_selection)
        node_map_add_string(&root, "ocr_selection", state->ocr_selection);
    else
        node_map_add(&root, "ocr_selection", MPV_FORMAT_NONE);
    if (source_sid >= 0)
        node_map_add_int64(&root, "source_sid", source_sid);
    else
        node_map_add(&root, "source_sid", MPV_FORMAT_NONE);
    if (output_sid >= 0)
        node_map_add_int64(&root, "output_sid", output_sid);
    else
        node_map_add(&root, "output_sid", MPV_FORMAT_NONE);
    node_map_add_int64(&root, "translated", translated);
    node_map_add_int64(&root, "pending", pending);
    if (error)
        node_map_add_string(&root, "error", error);
    else
        node_map_add(&root, "error", MPV_FORMAT_NONE);

    char *serialized = NULL;
    char *result = NULL;
    if (json_write(&serialized, &root) >= 0 && serialized)
        result = talloc_strdup(talloc_parent, serialized);
    talloc_free(serialized);
    talloc_free(tmp);
    return result ? result : talloc_strdup(
        talloc_parent,
        "{\"state\":\"error\",\"presentation\":\"companion\",\"source_sid\":null,"
        "\"output_sid\":null,\"translated\":0,\"pending\":0,"
        "\"error\":\"status serialization failed\"}");
}
