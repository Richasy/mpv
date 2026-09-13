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

#include "mpv_talloc.h"

#include "common/common.h"
#include "common/msg.h"
#include "demux/demux.h"
#include "demux/packet.h"
#include "misc/json.h"
#include "misc/node.h"
#include "sub/dec_sub.h"

#include "command.h"
#include "core.h"
#include "sub_translate.h"
#include "translation.h"

#define SUB_TRANSLATE_PAST_WINDOW 30.0
#define SUB_TRANSLATE_SCAN_MARGIN 5.0

enum cue_state {
    CUE_WAITING = 0,
    CUE_PENDING,
    CUE_TRANSLATED,
};

struct translated_cue {
    uint64_t id;
    uint64_t revision;
    int read_order;
    double start;
    double duration;
    char *text;
    char *translated;
    enum cue_state state;
};

struct sub_translate_state {
    struct MPContext *mpctx;
    struct mp_log *log;
    bool enabled;
    struct track *source_track;
    struct dec_sub *source_decoder;
    bool source_attached;
    bool secondary_blocked;
    struct sh_stream *output_stream;
    struct track *output_track;
    struct translated_cue **cues;
    int num_cues;
    int next_read_order;
    int translated_count;
    double scan_until;
    bool needs_scan;
    bool needs_rebuild;
    char *error;
    char *unsupported;
    char *last_status;
};

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
    struct track *secondary =
        state->mpctx->current_track[1][STREAM_SUB];
    return secondary && !owns_secondary(state, secondary);
}

static void clear_output(struct sub_translate_state *state, bool deselect)
{
    if (state->mpctx->demuxer)
        demux_clear_translated_sub_queue(state->mpctx->demuxer);
    reset_translated_subtitle_track(state->mpctx);

    struct track *secondary =
        state->mpctx->current_track[1][STREAM_SUB];
    if (deselect && owns_secondary(state, secondary))
        mp_switch_track_n(state->mpctx, 1, STREAM_SUB, NULL, 0);
}

static void detach_source(struct sub_translate_state *state)
{
    if (state->source_decoder) {
        sub_set_text_cue_callback(state->source_decoder, NULL, NULL);
    }
    state->source_track = NULL;
    state->source_decoder = NULL;
    state->source_attached = false;
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

static void submit_cue(struct sub_translate_state *state,
                       struct translated_cue *cue)
{
    enum mp_translation_submit_result result =
        mp_translation_submit(
            state->mpctx->translation,
            MP_TRANSLATION_SOURCE_SUBTITLE,
            cue->id, cue->revision, cue->text,
            cue->start, cue->duration, 0);
    if (result == MP_TRANSLATION_SUBMIT_QUEUED) {
        cue->state = CUE_PENDING;
        return;
    }
    cue->state = CUE_WAITING;
    if (result == MP_TRANSLATION_SUBMIT_NO_BACKEND)
        set_error(state, "sub-translate-config is disabled");
    else if (result == MP_TRANSLATION_SUBMIT_TOO_LATE)
        set_error(state, "translation skipped because the cue is too late");
    else if (result == MP_TRANSLATION_SUBMIT_INACTIVE)
        set_error(state, "subtitle translation source is inactive");
}

static void on_text_cue(void *ctx, const struct sub_text_cue *source)
{
    struct sub_translate_state *state = ctx;
    if (!state->enabled || !state->source_decoder ||
        state->secondary_blocked ||
        !cue_in_window(state, source))
    {
        return;
    }

    struct translated_cue *cue = find_cue(state, source->id);
    if (!cue) {
        cue = talloc_zero(NULL, struct translated_cue);
        cue->id = source->id;
        cue->revision = 1;
        cue->read_order = state->next_read_order++;
        MP_TARRAY_APPEND(state, state->cues, state->num_cues, cue);
    } else if (cue->start == source->start &&
               cue->duration == source->duration &&
               strcmp(cue->text, source->text) == 0)
    {
        if (cue->state == CUE_WAITING)
            submit_cue(state, cue);
        notify_status_if_changed(state);
        return;
    } else {
        cue->revision++;
        if (cue->state == CUE_TRANSLATED)
            state->needs_rebuild = true;
        talloc_free(cue->translated);
        cue->translated = NULL;
    }

    cue->start = source->start;
    cue->duration = source->duration;
    talloc_free(cue->text);
    cue->text = talloc_strdup(cue, source->text);
    cue->state = CUE_WAITING;
    submit_cue(state, cue);
    notify_status_if_changed(state);
}

static void feed_cue(struct sub_translate_state *state,
                     struct translated_cue *cue)
{
    if (!state->mpctx->demuxer || !cue->translated)
        return;
    if (secondary_conflict(state)) {
        set_error(state, "secondary subtitle track is already selected");
        return;
    }

    state->output_stream =
        demuxer_ensure_translated_sub(state->mpctx->demuxer);
    if (!state->output_stream) {
        set_error(state, "translated subtitle track is unavailable");
        return;
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
        return;
    }
    packet->pts = cue->start;
    packet->dts = cue->start;
    packet->duration = cue->duration;
    packet->sub_duration = cue->duration;
    demuxer_feed_translated_sub(state->mpctx->demuxer, packet);
    talloc_free(line);
}

static void rebuild_output(struct sub_translate_state *state)
{
    clear_output(state, false);
    for (int n = 0; n < state->num_cues; n++) {
        if (state->cues[n]->state == CUE_TRANSLATED)
            feed_cue(state, state->cues[n]);
    }
    state->needs_rebuild = false;
}

static void accept_result(
    void *ctx, const struct mp_translation_result *result)
{
    struct sub_translate_state *state = ctx;
    struct translated_cue *cue = find_cue(state, result->cue_id);
    if (!cue || cue->revision != result->revision)
        return;
    cue->state = CUE_WAITING;
    if (!result->translated) {
        if (result->kind == MP_TRANSLATION_RESULT_FALLBACK_FAILURE)
            set_error(state, result->error
                ? result->error : "translation provider failed");
        else if (result->kind == MP_TRANSLATION_RESULT_FALLBACK_RPM ||
                 result->kind == MP_TRANSLATION_RESULT_FALLBACK_BUDGET ||
                 result->kind == MP_TRANSLATION_RESULT_FALLBACK_QUEUE)
            set_error(state, "translation request skipped by limits");
        else if (result->kind == MP_TRANSLATION_RESULT_FALLBACK_LATE)
            set_error(state, "translation arrived after cue end");
        return;
    }

    talloc_free(cue->translated);
    cue->translated = talloc_strdup(cue, result->translated);
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
            talloc_free(cue);
            MP_TARRAY_REMOVE_AT(state->cues, state->num_cues, n);
        } else {
            n++;
        }
    }
}

static bool source_ready(struct sub_translate_state *state)
{
    return state->enabled && state->source_track &&
           state->source_decoder && state->source_attached &&
           !state->secondary_blocked && !state->unsupported;
}

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
    reset_source_work(state, false);
    replace_message(&state->error, state, NULL);
    replace_message(&state->unsupported, state, NULL);
    if (!state->enabled)
        return;

    if (track && track->d_sub) {
        state->source_track = track;
        state->source_decoder = track->d_sub;
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
        replace_message(&state->unsupported, state,
                        "selected subtitle track is bitmap-based");
        return;
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
    if (state->output_track) {
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
    if (state->needs_scan ||
        wanted_end > state->scan_until - SUB_TRANSLATE_SCAN_MARGIN)
    {
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
        if (state->output_track)
            output_sid = state->output_track->user_tid;
        translated = state->translated_count;
        if (mpctx->translation) {
            pending = mp_translation_pending_count(
                mpctx->translation,
                MP_TRANSLATION_SOURCE_SUBTITLE);
        }
        error = state->error ? state->error : state->unsupported;
        if (!state->enabled) {
            name = "disabled";
        } else if (state->unsupported) {
            name = "unsupported";
        } else if (state->error) {
            name = "error";
        } else if (pending > 0) {
            name = translated > 0 ? "active" : "translating";
        } else if (translated > 0 && state->output_track) {
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
        "{\"state\":\"error\",\"source_sid\":null,"
        "\"output_sid\":null,\"translated\":0,\"pending\":0,"
        "\"error\":\"status serialization failed\"}");
}
