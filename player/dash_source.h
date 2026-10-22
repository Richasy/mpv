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

struct mp_dash_transfer_diagnostic {
    uint64_t generation;
    uint32_t response_count;
    int track;
    int http_status;
    int curl_code;
    bool length_known;
    uint64_t expected;
    uint64_t received;
    uint64_t request_start;
    uint64_t request_end;
    bool headers_ok;
    bool aborted;
};

void mp_dash_source_init(struct mpv_global *global, struct MPContext *mpctx);
int mp_dash_source_validate(const mpv_dash_source *source);
int mp_dash_source_begin(struct mpv_global *global, const mpv_dash_source *source);
void mp_dash_source_fail(struct mpv_global *global, mpv_dash_track_kind track,
                         mpv_dash_source_failure failure);
void mp_dash_source_fail_with_origin(struct mpv_global *global,
                                     mpv_dash_track_kind track,
                                     mpv_dash_source_failure failure,
                                     mpv_dash_failure_origin origin);
void mp_dash_source_bound(struct mpv_global *global);
void mp_dash_source_stopped(struct mpv_global *global, bool error);
int mp_dash_source_snapshot(struct mpv_global *global, mpv_dash_source_status *out);
int mp_dash_source_failure_snapshot(struct mpv_global *global,
                                    mpv_dash_failure_detail *out);
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
                             int status, mpv_dash_source_failure failure,
                             mpv_dash_failure_origin origin);
void mp_dash_source_range_validated(struct mpv_global *global,
                                    mpv_dash_track_kind track);
bool mp_dash_source_has_validated_range(struct mpv_global *global,
                                        mpv_dash_track_kind track);
void mp_dash_source_fail_transfer(struct mpv_global *global,
                                  mpv_dash_source_failure failure,
                                  mpv_dash_failure_origin origin,
                                  struct mp_dash_transfer_diagnostic diagnostic);
bool mp_dash_source_transfer_snapshot(struct mpv_global *global,
                                      struct mp_dash_transfer_diagnostic *out);

#endif
