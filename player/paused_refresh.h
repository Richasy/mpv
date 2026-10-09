/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 */

#ifndef MP_PAUSED_REFRESH_H
#define MP_PAUSED_REFRESH_H

#include <stdbool.h>
#include <stdint.h>

enum mp_refresh_phase {
    MP_REFRESH_UNKNOWN,
    MP_REFRESH_REQUESTED,
    MP_REFRESH_APPLIED,
    MP_REFRESH_COMPLETED,
    MP_REFRESH_CANCELED,
};

struct mp_refresh_receipt {
    int64_t id;
    double position;
};

struct mp_refresh_source {
    bool initialized, paused, eof, seekable, video, stopped;
    bool pending, active;
    double position;
};

// Player-thread-only, constant-size state. IDs are monotonic for the entire
// player lifetime, including file changes. Zero is never an owned request.
struct mp_paused_refresh {
    int64_t source_epoch, revision, owner_epoch, owner_revision;
    enum mp_refresh_phase phase;
    struct mp_refresh_receipt requested, applied, completed, canceled;
    bool frame_done, exhausted;
};

void mp_refresh_cancel(struct mp_paused_refresh *s);
void mp_refresh_new_source(struct mp_paused_refresh *s);
void mp_refresh_new_seek(struct mp_paused_refresh *s);
void mp_refresh_supersede_seek(struct mp_paused_refresh *s,
                               int64_t *id, int64_t *epoch, int64_t *revision);
void mp_refresh_validate(struct mp_paused_refresh *s,
                         struct mp_refresh_source source);
bool mp_refresh_admit(struct mp_paused_refresh *s,
                      struct mp_refresh_source source,
                      int64_t id, double saved_position);
void mp_refresh_apply(struct mp_paused_refresh *s, int64_t id,
                      int64_t epoch, int64_t revision);
void mp_refresh_frame_done(struct mp_paused_refresh *s, int64_t id,
                           int64_t epoch, int64_t revision, bool processed);
void mp_refresh_complete(struct mp_paused_refresh *s,
                         struct mp_refresh_source source, int64_t id,
                         int64_t epoch, int64_t revision);
const char *mp_refresh_phase_name(enum mp_refresh_phase phase);

#endif
