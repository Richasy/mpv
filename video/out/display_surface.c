#include <stdint.h>
#include <stdatomic.h>

#include "common/common.h"
#include "mpv_talloc.h"
#include "osdep/threads.h"

#include "display_surface.h"

struct vo_display_surface_state {
    mp_mutex lock;
    void *surface;
    vo_display_surface_retain_fn retain;
    uint64_t epoch;
    uint64_t presented_serial;
    uint64_t presented_epoch;
    uint64_t pending_frame_id;
    uint64_t pending_epoch;
};

static _Atomic uint64_t presented_serial_seed;

static uint64_t allocate_presented_serial(void)
{
    uint64_t previous = atomic_load(&presented_serial_seed);
    do {
        if (previous == UINT64_MAX)
            return 0;
    } while (!atomic_compare_exchange_weak(&presented_serial_seed, &previous,
                                           previous + 1));
    return previous + 1;
}

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
        state->presented_epoch = 0;
        state->pending_frame_id = 0;
        state->pending_epoch = 0;
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

void vo_display_surface_prepare_frame(struct vo_display_surface_state *state,
                                      uint64_t frame_id)
{
    if (!state || !frame_id)
        return;

    mp_mutex_lock(&state->lock);
    if (state->surface) {
        state->pending_frame_id = frame_id;
        state->pending_epoch = state->epoch;
    }
    mp_mutex_unlock(&state->lock);
}

bool vo_display_surface_present_frame(struct vo_display_surface_state *state,
                                      void *surface)
{
    if (!state)
        return false;

    mp_mutex_lock(&state->lock);
    bool presented = surface && state->surface == surface &&
                     state->pending_frame_id &&
                     state->pending_epoch == state->epoch;
    if (presented) {
        uint64_t serial = allocate_presented_serial();
        if (serial) {
            state->presented_serial = serial;
            state->presented_epoch = state->epoch;
        } else {
            presented = false;
        }
    }
    state->pending_frame_id = 0;
    state->pending_epoch = 0;
    mp_mutex_unlock(&state->lock);
    return presented;
}

struct vo_display_surface_frame_snapshot
vo_display_surface_last_frame(struct vo_display_surface_state *state)
{
    mp_assert(state);

    mp_mutex_lock(&state->lock);
    struct vo_display_surface_frame_snapshot snapshot = {0};
    if (state->surface && state->presented_epoch == state->epoch) {
        snapshot.serial = state->presented_serial;
        snapshot.epoch = state->epoch;
    }
    mp_mutex_unlock(&state->lock);
    return snapshot;
}
