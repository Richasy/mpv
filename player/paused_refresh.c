/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 */

#include <math.h>

#include "paused_refresh.h"

void mp_refresh_cancel(struct mp_paused_refresh *s)
{
    if (s->phase == MP_REFRESH_REQUESTED || s->phase == MP_REFRESH_APPLIED ||
        s->phase == MP_REFRESH_COMPLETED)
    {
        s->canceled = s->requested;
        s->phase = MP_REFRESH_CANCELED;
    }
    s->frame_done = false;
}

static void advance(struct mp_paused_refresh *s, int64_t *counter)
{
    if (*counter == INT64_MAX) {
        mp_refresh_cancel(s);
        s->exhausted = true;
    } else {
        *counter += 1;
    }
}

void mp_refresh_new_source(struct mp_paused_refresh *s)
{
    mp_refresh_cancel(s);
    advance(s, &s->source_epoch);
    advance(s, &s->revision);
}

void mp_refresh_new_seek(struct mp_paused_refresh *s)
{
    mp_refresh_cancel(s);
    advance(s, &s->revision);
}

void mp_refresh_supersede_seek(struct mp_paused_refresh *s,
                               int64_t *id, int64_t *epoch, int64_t *revision)
{
    mp_refresh_new_seek(s);
    *id = 0;
    *epoch = 0;
    *revision = 0;
}

void mp_refresh_validate(struct mp_paused_refresh *s,
                         struct mp_refresh_source source)
{
    if (!source.initialized || !source.paused || source.eof ||
        source.stopped || !source.video || s->exhausted)
        mp_refresh_cancel(s);
}

bool mp_refresh_admit(struct mp_paused_refresh *s,
                      struct mp_refresh_source source,
                      int64_t id, double saved_position)
{
    if (s->exhausted || !s->source_epoch || s->revision == INT64_MAX ||
        id <= 0 || id <= s->requested.id ||
        !source.initialized || !source.paused || source.eof ||
        !source.seekable || !source.video || source.stopped ||
        source.pending || source.active ||
        !isfinite(saved_position) || saved_position < 0 ||
        !isfinite(source.position) || source.position < 0 ||
        fabs(saved_position - source.position) > 0.000001)
        return false;

    mp_refresh_new_seek(s);
    s->requested = (struct mp_refresh_receipt){id, saved_position};
    s->owner_epoch = s->source_epoch;
    s->owner_revision = s->revision;
    s->phase = MP_REFRESH_REQUESTED;
    return true;
}

static bool matches(struct mp_paused_refresh *s, int64_t id,
                    int64_t epoch, int64_t revision)
{
    return !s->exhausted && id > 0 && id == s->requested.id &&
           epoch == s->source_epoch && epoch == s->owner_epoch &&
           revision == s->revision && revision == s->owner_revision;
}

void mp_refresh_apply(struct mp_paused_refresh *s, int64_t id,
                      int64_t epoch, int64_t revision)
{
    if (s->phase == MP_REFRESH_REQUESTED && matches(s, id, epoch, revision)) {
        s->applied = s->requested;
        s->phase = MP_REFRESH_APPLIED;
    }
}

void mp_refresh_frame_done(struct mp_paused_refresh *s, int64_t id,
                           int64_t epoch, int64_t revision, bool processed)
{
    if (s->phase == MP_REFRESH_APPLIED && matches(s, id, epoch, revision)) {
        if (processed)
            s->frame_done = true;
        else
            mp_refresh_cancel(s);
    }
}

void mp_refresh_complete(struct mp_paused_refresh *s,
                         struct mp_refresh_source source, int64_t id,
                         int64_t epoch, int64_t revision)
{
    if (s->phase != MP_REFRESH_APPLIED || !matches(s, id, epoch, revision))
        return;

    if (s->frame_done && source.initialized && source.paused &&
        !source.eof && !source.stopped && !source.pending && source.video)
    {
        s->completed = s->requested;
        s->phase = MP_REFRESH_COMPLETED;
    } else {
        mp_refresh_cancel(s);
    }
}

const char *mp_refresh_phase_name(enum mp_refresh_phase phase)
{
    switch (phase) {
    case MP_REFRESH_REQUESTED: return "requested";
    case MP_REFRESH_APPLIED: return "applied";
    case MP_REFRESH_COMPLETED: return "completed";
    case MP_REFRESH_CANCELED: return "canceled";
    default: return "unknown";
    }
}
