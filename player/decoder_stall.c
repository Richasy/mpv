// SPDX-License-Identifier: LGPL-2.1-or-later

#include <math.h>

#include "decoder_stall.h"

static void start_observation(struct mp_decoder_stall *state, double now,
                              double pts)
{
    state->observing = true;
    state->last_pts = state->progress_pts = pts;
    state->last_advance = state->progress_started = now;
}

struct mp_decoder_stall_result mp_decoder_stall_check(
    struct mp_decoder_stall *state, double now, double pts, bool eligible,
    double timeout, int limit)
{
    struct mp_decoder_stall_result result = {.next_check = -1};
    if (!eligible || !isfinite(pts) || timeout <= 0 || limit <= 0) {
        state->observing = false;
        return result;
    }

    result.next_check = timeout;
    if (!state->observing || pts < state->last_pts ||
        (pts > state->last_pts && now - state->last_advance >= timeout))
    {
        start_observation(state, now, pts);
        return result;
    }

    if (pts > state->last_pts) {
        state->last_pts = pts;
        state->last_advance = now;
        // A seek's first frames are not proof of sustained playback.
        if (state->attempts &&
            now - state->progress_started >= timeout &&
            pts - state->progress_pts >= timeout)
        {
            result.action = MP_DECODER_STALL_RECOVERED;
            result.attempts = state->attempts;
            state->attempts = 0;
            state->exhausted = false;
            start_observation(state, now, pts);
        }
        return result;
    }

    result.stalled_for = now - state->last_advance;
    result.next_check = timeout - result.stalled_for;
    if (result.next_check > 0)
        return result;

    result.next_check = -1;
    if (state->attempts >= limit) {
        if (!state->exhausted) {
            state->exhausted = true;
            result.action = MP_DECODER_STALL_EXHAUSTED;
            result.attempts = state->attempts;
        }
        return result;
    }

    result.action = MP_DECODER_STALL_SEEK;
    result.attempts = ++state->attempts;
    result.next_check = timeout;
    state->exhausted = false;
    state->observing = false;
    return result;
}
