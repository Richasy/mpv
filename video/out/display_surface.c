#include <stdint.h>

#include "common/common.h"
#include "mpv_talloc.h"
#include "osdep/threads.h"

#include "display_surface.h"

struct vo_display_surface_state {
    mp_mutex lock;
    void *surface;
    vo_display_surface_retain_fn retain;
    uint64_t epoch;
};

static void destroy_display_surface_state(void *ptr)
{
    struct vo_display_surface_state *state = ptr;
    mp_assert(!state->surface);
    mp_mutex_destroy(&state->lock);
}

struct vo_display_surface_state *vo_display_surface_state_create(void *parent)
{
    struct vo_display_surface_state *state =
        talloc_zero(parent, struct vo_display_surface_state);
    mp_mutex_init(&state->lock);
    talloc_set_destructor(state, destroy_display_surface_state);
    return state;
}

bool vo_display_surface_publish(struct vo_display_surface_state *state,
                                void *surface,
                                vo_display_surface_retain_fn retain)
{
    mp_assert(state);
    mp_assert(!surface || retain);

    mp_mutex_lock(&state->lock);
    bool changed = state->surface != surface;
    if (changed) {
        mp_assert(state->epoch < UINT64_MAX);
        state->surface = surface;
        state->retain = surface ? retain : NULL;
        state->epoch++;
    }
    mp_mutex_unlock(&state->lock);
    return changed;
}

struct vo_display_surface_snapshot
vo_display_surface_acquire(struct vo_display_surface_state *state)
{
    mp_assert(state);

    mp_mutex_lock(&state->lock);
    struct vo_display_surface_snapshot snapshot = {
        .surface = state->surface,
        .epoch = state->epoch,
    };
    if (snapshot.surface)
        state->retain(snapshot.surface);
    mp_mutex_unlock(&state->lock);
    return snapshot;
}

uint64_t vo_display_surface_epoch(struct vo_display_surface_state *state)
{
    mp_assert(state);

    mp_mutex_lock(&state->lock);
    uint64_t epoch = state->epoch;
    mp_mutex_unlock(&state->lock);
    return epoch;
}
