// SPDX-License-Identifier: LGPL-2.1-or-later

#ifndef MPV_PLAYER_DECODER_STALL_H
#define MPV_PLAYER_DECODER_STALL_H

#include <stdbool.h>

struct mp_decoder_stall {
    bool observing;
    bool exhausted;
    double last_pts;
    double last_advance;
    double progress_pts;
    double progress_started;
    int attempts;
};

enum mp_decoder_stall_action {
    MP_DECODER_STALL_NONE,
    MP_DECODER_STALL_SEEK,
    MP_DECODER_STALL_RECOVERED,
    MP_DECODER_STALL_EXHAUSTED,
};

struct mp_decoder_stall_result {
    enum mp_decoder_stall_action action;
    int attempts;
    double stalled_for;
    double next_check;
};

struct mp_decoder_stall_result mp_decoder_stall_check(
    struct mp_decoder_stall *state, double now, double pts, bool eligible,
    double timeout, int limit);

#endif
