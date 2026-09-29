/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef MP_DASH_SOURCE_H
#define MP_DASH_SOURCE_H

#include <stdbool.h>

#include "mpv/client.h"

#define MP_DASH_VIDEO_URL "dash://video"
#define MP_DASH_AUDIO_URL "dash://audio"

struct mpv_global;
struct MPContext;

struct mp_dash_track_config {
    char *url;
    char *user_agent;
    char *referer;
    bool allow_loopback_http;
};

void mp_dash_source_init(struct mpv_global *global, struct MPContext *mpctx);
int mp_dash_source_validate(const mpv_dash_source *source);
int mp_dash_source_begin(struct mpv_global *global, const mpv_dash_source *source);
void mp_dash_source_fail(struct mpv_global *global, mpv_dash_track_kind track,
                         mpv_dash_source_failure failure);
void mp_dash_source_bound(struct mpv_global *global);
void mp_dash_source_stopped(struct mpv_global *global, bool error);
int mp_dash_source_snapshot(struct mpv_global *global, mpv_dash_source_status *out);
int mp_dash_source_range_snapshot(struct mpv_global *global,
                                  mpv_dash_range_capability *out);
bool mp_dash_source_seek_blocked(struct mpv_global *global);
int mp_dash_source_frame_snapshot(struct mpv_global *global,
                                  mpv_dash_frame_status *out);
bool mp_dash_source_active(struct mpv_global *global);
bool mp_dash_source_failed(struct mpv_global *global);
bool mp_dash_source_terminal(struct mpv_global *global);
bool mp_dash_source_get_track(struct mpv_global *global, const char *alias,
                              void *parent, mpv_dash_track_kind *kind,
                              struct mp_dash_track_config *config);
void mp_dash_source_response(struct mpv_global *global, mpv_dash_track_kind track,
                             int status, bool initial_full_body);
void mp_dash_source_range_validated(struct mpv_global *global,
                                    mpv_dash_track_kind track);

#endif
