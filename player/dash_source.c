/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <math.h>
#include <stdatomic.h>
#include <string.h>

#include "common/global.h"
#include "misc/dispatch.h"
#include "mpv_talloc.h"
#include "osdep/threads.h"
#include "player/core.h"
#include "video/out/display_surface.h"

#include "dash_source.h"

#if HAVE_LIBCURL
#include <curl/curl.h>

struct mp_dash_source_state {
    mp_mutex lock;
    struct MPContext *mpctx;
    char *video_url;
    char *audio_url;
    char *user_agent;
    char *referer;
    bool allow_loopback_http;
    uint64_t baseline_present_serial;
    uint32_t validated_ranges;
    mpv_dash_source_status status;
};

static _Atomic uint64_t dash_generation_seed;

static uint64_t allocate_generation(void)
{
    uint64_t previous = atomic_load(&dash_generation_seed);
    do {
        if (previous == UINT64_MAX)
            return 0;
    } while (!atomic_compare_exchange_weak(&dash_generation_seed, &previous,
                                           previous + 1));
    return previous + 1;
}

static void state_destructor(void *ptr)
{
    struct mp_dash_source_state *state = ptr;
    mp_mutex_destroy(&state->lock);
}

void mp_dash_source_init(struct mpv_global *global, struct MPContext *mpctx)
{
    struct mp_dash_source_state *state =
        talloc_zero(global, struct mp_dash_source_state);
    state->mpctx = mpctx;
    mp_mutex_init(&state->lock);
    talloc_set_destructor(state, state_destructor);
    global->dash_source = state;
}

static bool safe_header_value(const char *value, size_t limit)
{
    if (!value)
        return true;
    size_t i = 0;
    for (; value[i] && i <= limit; i++) {
        unsigned char c = value[i];
        if (c < 0x20 || c == 0x7f)
            return false;
    }
    return i <= limit;
}

static bool valid_url(const char *url, bool allow_loopback)
{
    if (!url || !safe_header_value(url, 16384) || !url[0] ||
        strchr(url, '#') || strchr(url, '\\'))
        return false;
    for (const unsigned char *p = (const unsigned char *)url; *p; p++) {
        if (*p <= 0x20 || *p >= 0x7f || *p == '"' || *p == '<' ||
            *p == '>' || *p == '{' || *p == '}' || *p == '|')
            return false;
    }
    if (strncmp(url, "https://", 8) &&
        (!allow_loopback || strncmp(url, "http://", 7)))
        return false;

    bool result = false;
    CURLU *parsed = curl_url();
    char *scheme = NULL, *host = NULL, *user = NULL, *password = NULL;
    char *port = NULL;
    if (!parsed || curl_url_set(parsed, CURLUPART_URL, url, 0) ||
        curl_url_get(parsed, CURLUPART_SCHEME, &scheme, 0) ||
        curl_url_get(parsed, CURLUPART_HOST, &host, 0) ||
        curl_url_get(parsed, CURLUPART_USER, &user, 0) != CURLUE_NO_USER ||
        curl_url_get(parsed, CURLUPART_PASSWORD, &password, 0) != CURLUE_NO_PASSWORD)
        goto done;

    if (strcmp(scheme, "https") == 0 && host[0]) {
        result = true;
    } else if (allow_loopback && strcmp(scheme, "http") == 0 &&
               (!strcmp(host, "127.0.0.1") || !strcmp(host, "[::1]") ||
                !strcmp(host, "::1")) &&
               curl_url_get(parsed, CURLUPART_PORT, &port, 0) == CURLUE_OK)
    {
        result = port[0] && strcmp(port, "0") != 0;
    }
done:
    curl_free(port);
    curl_free(password);
    curl_free(user);
    curl_free(host);
    curl_free(scheme);
    curl_url_cleanup(parsed);
    return result;
}

static bool safe_codec(const char *codec)
{
    if (!codec || !codec[0])
        return false;
    size_t i = 0;
    for (; codec[i] && i < 64; i++) {
        unsigned char c = codec[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-' ||
              c == '_' || c == '+' || c == ','))
            return false;
    }
    return i < 64;
}

static int validate_track(const mpv_dash_track *track, bool video,
                          bool allow_loopback)
{
    if (track->struct_size != sizeof(*track) ||
        (track->flags & ~MPV_DASH_TRACK_SEGMENT_BASE) ||
        !valid_url(track->url, allow_loopback) ||
        !track->mime_type ||
        strcmp(track->mime_type, video ? "video/mp4" : "audio/mp4") ||
        !safe_codec(track->codec) || track->quality < 0 ||
        track->width < 0 || track->height < 0 ||
        (!video && (track->width || track->height)) ||
        (video && (!!track->width != !!track->height)))
        return MPV_ERROR_INVALID_PARAMETER;

    const mpv_dash_byte_range *init = &track->initialization;
    const mpv_dash_byte_range *index = &track->index;
    if (track->flags & MPV_DASH_TRACK_SEGMENT_BASE) {
        if (init->start < 0 || init->end < init->start ||
            index->start <= init->end || index->end < index->start ||
            index->end == INT64_MAX)
            return MPV_ERROR_INVALID_PARAMETER;
        return MPV_ERROR_UNSUPPORTED;
    }
    if (init->start || init->end || index->start || index->end)
        return MPV_ERROR_INVALID_PARAMETER;
    return MPV_ERROR_SUCCESS;
}

int mp_dash_source_validate(const mpv_dash_source *source)
{
    if (!source || source->struct_size != sizeof(*source) ||
        source->api_version != MPV_DASH_SOURCE_API_VERSION ||
        source->reserved ||
        (source->flags & ~MPV_DASH_SOURCE_ALLOW_LOOPBACK_HTTP) ||
        !isfinite(source->duration_seconds) || source->duration_seconds < 0 ||
        !safe_header_value(source->user_agent, 512) ||
        !safe_header_value(source->referer, 2048) ||
        (source->referer && !valid_url(source->referer, false)))
        return MPV_ERROR_INVALID_PARAMETER;

    bool loopback = !!(source->flags & MPV_DASH_SOURCE_ALLOW_LOOPBACK_HTTP);
    int video = validate_track(&source->video, true, loopback);
    int audio = validate_track(&source->audio, false, loopback);
    if (video == MPV_ERROR_INVALID_PARAMETER ||
        audio == MPV_ERROR_INVALID_PARAMETER)
        return MPV_ERROR_INVALID_PARAMETER;
    if (video == MPV_ERROR_UNSUPPORTED || audio == MPV_ERROR_UNSUPPORTED)
        return MPV_ERROR_UNSUPPORTED;
    return MPV_ERROR_SUCCESS;
}

int mp_dash_source_begin(struct mpv_global *global, const mpv_dash_source *source)
{
    struct mp_dash_source_state *state = global->dash_source;
    struct vo_display_surface_frame_snapshot previous =
        vo_display_surface_last_frame(state->mpctx->display_surface);
    mp_mutex_lock(&state->lock);
    if (state->status.generation) {
        mp_mutex_unlock(&state->lock);
        return MPV_ERROR_UNSUPPORTED;
    }
    uint64_t generation = allocate_generation();
    if (!generation) {
        mp_mutex_unlock(&state->lock);
        return MPV_ERROR_UNSUPPORTED;
    }
    state->video_url = talloc_strdup(state, source->video.url);
    state->audio_url = talloc_strdup(state, source->audio.url);
    state->user_agent = talloc_strdup(state, source->user_agent);
    state->referer = talloc_strdup(state, source->referer);
    state->allow_loopback_http =
        !!(source->flags & MPV_DASH_SOURCE_ALLOW_LOOPBACK_HTTP);
    state->baseline_present_serial = previous.serial;
    state->validated_ranges = 0;
    state->status = (mpv_dash_source_status) {
        .struct_size = sizeof(mpv_dash_source_status),
        .api_version = MPV_DASH_SOURCE_API_VERSION,
        .generation = generation,
        .phase = MPV_DASH_SOURCE_QUEUED,
    };
    mp_mutex_unlock(&state->lock);
    return MPV_ERROR_SUCCESS;
}

static void stop_failed_dash(void *ptr)
{
    struct MPContext *mpctx = ptr;
    if (!mpctx->filename || strcmp(mpctx->filename, MP_DASH_VIDEO_URL))
        return;
    if (!mpctx->stop_play) {
        mpctx->error_playing = MPV_ERROR_LOADING_FAILED;
        mpctx->stop_play = PT_ERROR;
        mp_abort_playback_async(mpctx);
        mp_wakeup_core(mpctx);
    }
}

static void signal_failure(struct mp_dash_source_state *state)
{
    // A demuxer can be blocked reading the *other* track while the curl thread
    // sees a terminal status. Wake every playback child before queuing the
    // core-thread stop; otherwise the queued callback may never get to run.
    mp_abort_playback_async(state->mpctx);
    mp_dispatch_enqueue(state->mpctx->dispatch, stop_failed_dash, state->mpctx);
    mp_wakeup_core(state->mpctx);
}

static bool set_failure(struct mp_dash_source_state *state,
                        mpv_dash_track_kind track, mpv_dash_source_failure failure)
{
    if (state->status.phase == MPV_DASH_SOURCE_FAILED ||
        state->status.phase == MPV_DASH_SOURCE_STOPPED ||
        !state->status.generation)
        return false;
    state->status.phase = MPV_DASH_SOURCE_FAILED;
    state->status.failure = failure;
    state->status.failed_track = track;
    return true;
}

void mp_dash_source_fail(struct mpv_global *global, mpv_dash_track_kind track,
                         mpv_dash_source_failure failure)
{
    struct mp_dash_source_state *state = global->dash_source;
    mp_mutex_lock(&state->lock);
    bool first = set_failure(state, track, failure);
    mp_mutex_unlock(&state->lock);
    if (first)
        signal_failure(state);
}

void mp_dash_source_response(struct mpv_global *global, mpv_dash_track_kind track,
                             int status, bool initial_full_body)
{
    struct mp_dash_source_state *state = global->dash_source;
    mp_mutex_lock(&state->lock);
    bool first = false;
    if (state->status.generation &&
        state->status.phase != MPV_DASH_SOURCE_FAILED &&
        state->status.phase != MPV_DASH_SOURCE_STOPPED)
    {
        int32_t *last = track == MPV_DASH_TRACK_VIDEO ?
            &state->status.video_http_status : &state->status.audio_http_status;
        uint32_t *count = track == MPV_DASH_TRACK_VIDEO ?
            &state->status.video_responses : &state->status.audio_responses;
        *last = status;
        if (*count < UINT32_MAX)
            (*count)++;
        if (status != 206 && !(status == 200 && initial_full_body)) {
            mpv_dash_source_failure failure =
                status == 412 ? MPV_DASH_FAILURE_HTTP_RISK :
                status == 416 ? MPV_DASH_FAILURE_HTTP_RANGE :
                status == 401 || status == 403 ? MPV_DASH_FAILURE_HTTP_AUTH :
                MPV_DASH_FAILURE_HTTP_STATUS;
            first = set_failure(state, track, failure);
        }
    }
    mp_mutex_unlock(&state->lock);
    if (first)
        signal_failure(state);
}

void mp_dash_source_range_validated(struct mpv_global *global,
                                    mpv_dash_track_kind track)
{
    struct mp_dash_source_state *state = global->dash_source;
    mp_mutex_lock(&state->lock);
    if (state->status.generation &&
        state->status.phase != MPV_DASH_SOURCE_FAILED &&
        state->status.phase != MPV_DASH_SOURCE_STOPPED &&
        (track == MPV_DASH_TRACK_VIDEO || track == MPV_DASH_TRACK_AUDIO))
        state->validated_ranges |= (uint32_t)track;
    mp_mutex_unlock(&state->lock);
}

void mp_dash_source_bound(struct mpv_global *global)
{
    struct mp_dash_source_state *state = global->dash_source;
    mp_mutex_lock(&state->lock);
    if (state->status.phase == MPV_DASH_SOURCE_QUEUED)
        state->status.phase = MPV_DASH_SOURCE_TRACKS_BOUND;
    mp_mutex_unlock(&state->lock);
}

void mp_dash_source_stopped(struct mpv_global *global, bool error)
{
    struct mp_dash_source_state *state = global->dash_source;
    mp_mutex_lock(&state->lock);
    if (state->status.generation &&
        state->status.phase != MPV_DASH_SOURCE_FAILED &&
        state->status.phase != MPV_DASH_SOURCE_STOPPED)
    {
        if (error) {
            state->status.failure = MPV_DASH_FAILURE_PLAYBACK;
            state->status.phase = MPV_DASH_SOURCE_FAILED;
        } else {
            state->status.phase = MPV_DASH_SOURCE_STOPPED;
        }
    }
    mp_mutex_unlock(&state->lock);
}

int mp_dash_source_snapshot(struct mpv_global *global, mpv_dash_source_status *out)
{
    if (!out || out->struct_size != sizeof(*out) ||
        out->api_version != MPV_DASH_SOURCE_API_VERSION)
        return MPV_ERROR_INVALID_PARAMETER;

    struct mp_dash_source_state *state = global->dash_source;
    mp_mutex_lock(&state->lock);
    mpv_dash_source_status status = state->status;
    mp_mutex_unlock(&state->lock);
    status.struct_size = sizeof(status);
    status.api_version = MPV_DASH_SOURCE_API_VERSION;
    *out = status;
    return MPV_ERROR_SUCCESS;
}

int mp_dash_source_range_snapshot(struct mpv_global *global,
                                  mpv_dash_range_capability *out)
{
    if (!out || out->struct_size != sizeof(*out) ||
        out->api_version != MPV_DASH_RANGE_CAPABILITY_API_VERSION)
        return MPV_ERROR_INVALID_PARAMETER;

    struct mp_dash_source_state *state = global->dash_source;
    mp_mutex_lock(&state->lock);
    bool active = state->status.phase != MPV_DASH_SOURCE_FAILED &&
                  state->status.phase != MPV_DASH_SOURCE_STOPPED;
    mpv_dash_range_capability result = {
        .struct_size = sizeof(*out),
        .api_version = MPV_DASH_RANGE_CAPABILITY_API_VERSION,
        .source_generation = state->status.generation,
        .video_validated_206 = active &&
            !!(state->validated_ranges & MPV_DASH_TRACK_VIDEO),
        .audio_validated_206 = active &&
            !!(state->validated_ranges & MPV_DASH_TRACK_AUDIO),
    };
    mp_mutex_unlock(&state->lock);
    *out = result;
    return MPV_ERROR_SUCCESS;
}

bool mp_dash_source_seek_blocked(struct mpv_global *global)
{
    struct mp_dash_source_state *state = global->dash_source;
    mp_mutex_lock(&state->lock);
    bool blocked = state->status.generation && state->mpctx->filename &&
                   !strcmp(state->mpctx->filename, MP_DASH_VIDEO_URL) &&
                   (state->status.phase != MPV_DASH_SOURCE_TRACKS_BOUND ||
                    state->validated_ranges !=
                        (MPV_DASH_TRACK_VIDEO | MPV_DASH_TRACK_AUDIO));
    mp_mutex_unlock(&state->lock);
    return blocked;
}

int mp_dash_source_frame_snapshot(struct mpv_global *global,
                                  mpv_dash_frame_status *out)
{
    if (!out || out->struct_size != sizeof(*out) ||
        out->api_version != MPV_DASH_FRAME_STATUS_API_VERSION)
        return MPV_ERROR_INVALID_PARAMETER;

    struct mp_dash_source_state *state = global->dash_source;
    struct vo_display_surface_frame_snapshot frame =
        vo_display_surface_last_frame(state->mpctx->display_surface);
    mp_mutex_lock(&state->lock);
    mpv_dash_frame_status result = {
        .struct_size = sizeof(result),
        .api_version = MPV_DASH_FRAME_STATUS_API_VERSION,
        .source_generation = state->status.generation,
    };
    if (state->status.phase == MPV_DASH_SOURCE_TRACKS_BOUND &&
        frame.epoch && frame.serial > state->baseline_present_serial)
    {
        result.presented_generation = state->status.generation;
        result.presented_frame_serial = frame.serial;
        result.presented_surface_epoch = frame.epoch;
    }
    mp_mutex_unlock(&state->lock);
    *out = result;
    return MPV_ERROR_SUCCESS;
}

bool mp_dash_source_active(struct mpv_global *global)
{
    struct mp_dash_source_state *state = global->dash_source;
    mp_mutex_lock(&state->lock);
    bool active = state->status.generation != 0;
    mp_mutex_unlock(&state->lock);
    return active;
}

bool mp_dash_source_failed(struct mpv_global *global)
{
    struct mp_dash_source_state *state = global->dash_source;
    mp_mutex_lock(&state->lock);
    bool failed = state->status.phase == MPV_DASH_SOURCE_FAILED;
    mp_mutex_unlock(&state->lock);
    return failed;
}

bool mp_dash_source_terminal(struct mpv_global *global)
{
    struct mp_dash_source_state *state = global->dash_source;
    mp_mutex_lock(&state->lock);
    bool terminal = state->status.phase == MPV_DASH_SOURCE_FAILED ||
                    state->status.phase == MPV_DASH_SOURCE_STOPPED;
    mp_mutex_unlock(&state->lock);
    return terminal;
}

bool mp_dash_source_get_track(struct mpv_global *global, const char *alias,
                              void *parent, mpv_dash_track_kind *kind,
                              struct mp_dash_track_config *config)
{
    struct mp_dash_source_state *state = global->dash_source;
    mp_mutex_lock(&state->lock);
    bool ok = state->status.generation &&
              state->status.phase != MPV_DASH_SOURCE_FAILED &&
              state->status.phase != MPV_DASH_SOURCE_STOPPED;
    if (ok && !strcmp(alias, MP_DASH_VIDEO_URL)) {
        *kind = MPV_DASH_TRACK_VIDEO;
        config->url = talloc_strdup(parent, state->video_url);
    } else if (ok && !strcmp(alias, MP_DASH_AUDIO_URL)) {
        *kind = MPV_DASH_TRACK_AUDIO;
        config->url = talloc_strdup(parent, state->audio_url);
    } else {
        ok = false;
    }
    if (ok) {
        config->user_agent = talloc_strdup(parent, state->user_agent);
        config->referer = talloc_strdup(parent, state->referer);
        config->allow_loopback_http = state->allow_loopback_http;
    }
    mp_mutex_unlock(&state->lock);
    return ok;
}

#else

void mp_dash_source_init(struct mpv_global *global, struct MPContext *mpctx)
{ (void)global; (void)mpctx; }
int mp_dash_source_validate(const mpv_dash_source *source)
{ (void)source; return MPV_ERROR_UNSUPPORTED; }
int mp_dash_source_begin(struct mpv_global *global, const mpv_dash_source *source)
{ (void)global; (void)source; return MPV_ERROR_UNSUPPORTED; }
void mp_dash_source_fail(struct mpv_global *global, mpv_dash_track_kind track,
                         mpv_dash_source_failure failure)
{ (void)global; (void)track; (void)failure; }
void mp_dash_source_bound(struct mpv_global *global)
{ (void)global; }
void mp_dash_source_stopped(struct mpv_global *global, bool error)
{ (void)global; (void)error; }
int mp_dash_source_snapshot(struct mpv_global *global, mpv_dash_source_status *out)
{ (void)global; (void)out; return MPV_ERROR_UNSUPPORTED; }
int mp_dash_source_range_snapshot(struct mpv_global *global,
                                  mpv_dash_range_capability *out)
{ (void)global; (void)out; return MPV_ERROR_UNSUPPORTED; }
bool mp_dash_source_seek_blocked(struct mpv_global *global)
{ (void)global; return false; }
int mp_dash_source_frame_snapshot(struct mpv_global *global,
                                  mpv_dash_frame_status *out)
{ (void)global; (void)out; return MPV_ERROR_UNSUPPORTED; }
bool mp_dash_source_active(struct mpv_global *global)
{ (void)global; return false; }
bool mp_dash_source_failed(struct mpv_global *global)
{ (void)global; return false; }
bool mp_dash_source_terminal(struct mpv_global *global)
{ (void)global; return false; }
bool mp_dash_source_get_track(struct mpv_global *global, const char *alias,
                              void *parent, mpv_dash_track_kind *kind,
                              struct mp_dash_track_config *config)
{ (void)global; (void)alias; (void)parent; (void)kind; (void)config; return false; }
void mp_dash_source_response(struct mpv_global *global, mpv_dash_track_kind track,
                             int status, bool initial_full_body)
{ (void)global; (void)track; (void)status; (void)initial_full_body; }
void mp_dash_source_range_validated(struct mpv_global *global,
                                    mpv_dash_track_kind track)
{ (void)global; (void)track; }

#endif
