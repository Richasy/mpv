#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

#include "osdep/timer.h"

// One speculative input owns this budget through opening, probing and caching.
// The ordinary playback owner removes the limits only after adopting the input.
struct mp_prefetch_budget {
    atomic_bool active;
    atomic_bool ready;
    atomic_bool failed;
    atomic_uint_fast64_t bytes;
    atomic_uint_fast64_t reserved_bytes;
    atomic_int requests;
    uint64_t max_bytes;
    int max_requests;
    int64_t deadline;
    double seconds;
};

static inline bool mp_prefetch_active(struct mp_prefetch_budget *b)
{
    return b && atomic_load(&b->active);
}

static inline bool mp_prefetch_stopped(struct mp_prefetch_budget *b)
{
    if (!mp_prefetch_active(b))
        return false;
    if (!atomic_load(&b->ready) && mp_time_ns() >= b->deadline)
        atomic_store(&b->failed, true);
    return atomic_load(&b->failed);
}
