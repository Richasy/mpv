// SPDX-License-Identifier: LGPL-2.1-or-later

#include <math.h>
#include <stdio.h>

#include "misc/mp_assert.h"
#include "player/decoder_stall.h"

static struct mp_decoder_stall_result check(struct mp_decoder_stall *state,
                                            double now, double pts)
{
    return mp_decoder_stall_check(state, now, pts, true, 5, 3);
}

static void check_short_progress_does_not_replenish_budget(void)
{
    struct mp_decoder_stall state = {0};
    double now = 0;
    double pts = 0.146122;
    for (int attempt = 1; attempt <= 3; attempt++) {
        mp_require(check(&state, now, pts).action == MP_DECODER_STALL_NONE);
        mp_require(state.attempts == attempt - 1);
        mp_require(check(&state, now + 4.99, pts).action ==
                   MP_DECODER_STALL_NONE);
        struct mp_decoder_stall_result result = check(&state, now + 5, pts);
        mp_require(result.action == MP_DECODER_STALL_SEEK);
        mp_require(result.attempts == attempt);
        mp_require(result.stalled_for == 5);

        result = mp_decoder_stall_check(
            &state, now + 5.05, NAN, false, 5, 3);
        mp_require(result.action == MP_DECODER_STALL_NONE);
        mp_require(result.next_check < 0);
        mp_require(state.attempts == attempt);
        now += 5.25;
        pts += 0.125;
    }

    mp_require(check(&state, now, pts).action == MP_DECODER_STALL_NONE);
    struct mp_decoder_stall_result result = check(&state, now + 5, pts);
    mp_require(result.action == MP_DECODER_STALL_EXHAUSTED);
    mp_require(result.attempts == 3 && result.next_check < 0);
    for (int n = 6; n < 100; n++) {
        mp_require(check(&state, now + n, pts).action == MP_DECODER_STALL_NONE);
        mp_require(state.attempts == 3);
    }
}

static void check_interruptions_preserve_attempts(void)
{
    struct mp_decoder_stall state = {0};
    check(&state, 0, 1);
    mp_require(check(&state, 5, 1).action == MP_DECODER_STALL_SEEK);
    for (int n = 6; n < 20; n++) {
        struct mp_decoder_stall_result result = mp_decoder_stall_check(
            &state, n, 20, false, 5, 3);
        mp_require(result.action == MP_DECODER_STALL_NONE);
        mp_require(result.next_check < 0 && state.attempts == 1);
    }

    mp_require(check(&state, 20, 200).action == MP_DECODER_STALL_NONE);
    mp_require(state.attempts == 1);
    mp_require(check(&state, 24, 200).action == MP_DECODER_STALL_NONE);
    mp_require(check(&state, 25, 200).action == MP_DECODER_STALL_SEEK);
    mp_require(state.attempts == 2);

    mp_require(check(&state, 26, NAN).action == MP_DECODER_STALL_NONE);
    mp_require(check(&state, 27, 200.1).action == MP_DECODER_STALL_NONE);
    mp_require(state.attempts == 2);
    mp_require(check(&state, 32, 200.1).action == MP_DECODER_STALL_SEEK);
    mp_require(state.attempts == 3);
}

static void check_recovery_requires_time_and_progress(void)
{
    struct mp_decoder_stall state = {.attempts = 2, .exhausted = true};
    check(&state, 0, 10);
    for (int n = 1; n < 20; n++) {
        mp_require(check(&state, n * 0.25, 10 + n * 0.5).action ==
                   MP_DECODER_STALL_NONE);
        mp_require(state.attempts == 2);
    }
    struct mp_decoder_stall_result result = check(&state, 5, 20);
    mp_require(result.action == MP_DECODER_STALL_RECOVERED);
    mp_require(result.attempts == 2 && state.attempts == 0);
    mp_require(!state.exhausted);
    mp_require(check(&state, 10, 20).action == MP_DECODER_STALL_SEEK);
    mp_require(state.attempts == 1);

    state = (struct mp_decoder_stall){.attempts = 1};
    check(&state, 0, 0);
    for (int n = 1; n < 10; n++) {
        mp_require(check(&state, n, n * 0.5).action == MP_DECODER_STALL_NONE);
        mp_require(state.attempts == 1);
    }
    mp_require(check(&state, 10, 5).action == MP_DECODER_STALL_RECOVERED);
    mp_require(state.attempts == 0);
}

static void check_discontinuities_restart_observation_only(void)
{
    struct mp_decoder_stall state = {.attempts = 2};
    check(&state, 0, 100);
    mp_require(check(&state, 4, 50).action == MP_DECODER_STALL_NONE);
    mp_require(check(&state, 5, 50).action == MP_DECODER_STALL_NONE);
    mp_require(check(&state, 9, 50).action == MP_DECODER_STALL_SEEK);
    mp_require(state.attempts == 3);

    state = (struct mp_decoder_stall){.attempts = 1};
    check(&state, 0, 0);
    check(&state, 4, 4);
    mp_decoder_stall_check(&state, 100, 4, false, 5, 3);
    mp_require(check(&state, 101, 4).action == MP_DECODER_STALL_NONE);
    mp_require(check(&state, 102, 5).action == MP_DECODER_STALL_NONE);
    mp_require(state.attempts == 1);
}

static void check_disabled_and_new_file(void)
{
    struct mp_decoder_stall state = {.attempts = 3, .exhausted = true};
    struct mp_decoder_stall_result result = mp_decoder_stall_check(
        &state, 0, 10, true, 0, 3);
    mp_require(result.action == MP_DECODER_STALL_NONE && result.next_check < 0);
    result = mp_decoder_stall_check(&state, 1, 10, true, 5, 0);
    mp_require(result.action == MP_DECODER_STALL_NONE && result.next_check < 0);
    mp_require(state.attempts == 3);

    state = (struct mp_decoder_stall){0};
    check(&state, 0, 0);
    result = check(&state, 5, 0);
    mp_require(result.action == MP_DECODER_STALL_SEEK && result.attempts == 1);
}

static void check_progress_after_exhaustion_starts_a_fresh_window(void)
{
    struct mp_decoder_stall state = {.attempts = 3};
    check(&state, 0, 0);
    mp_require(check(&state, 5, 0).action == MP_DECODER_STALL_EXHAUSTED);
    mp_require(check(&state, 100, 5).action == MP_DECODER_STALL_NONE);
    for (int n = 1; n < 5; n++) {
        mp_require(check(&state, 100 + n, 5 + n).action ==
                   MP_DECODER_STALL_NONE);
        mp_require(state.attempts == 3);
    }
    mp_require(check(&state, 105, 10).action == MP_DECODER_STALL_RECOVERED);
    mp_require(state.attempts == 0 && !state.exhausted);
}

static void check_normal_playback_is_quiet(void)
{
    struct mp_decoder_stall state = {0};
    for (int n = 0; n < 10000; n++) {
        struct mp_decoder_stall_result result = check(
            &state, n / 60.0, n / 60.0 * 1.75);
        mp_require(result.action == MP_DECODER_STALL_NONE);
        mp_require(state.attempts == 0 && result.next_check == 5);
    }
}

int main(void)
{
    check_short_progress_does_not_replenish_budget();
    check_interruptions_preserve_attempts();
    check_recovery_requires_time_and_progress();
    check_discontinuities_restart_observation_only();
    check_disabled_and_new_file();
    check_progress_after_exhaustion_starts_a_fresh_window();
    check_normal_playback_is_quiet();
    puts("decoder stall policy tests passed");
    return 0;
}
