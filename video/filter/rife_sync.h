#pragma once

#include <stdbool.h>
#include <stdint.h>

static inline bool mp_rife_slot_ready(int in_flight,
                                      uint64_t consumer_fence_value,
                                      uint64_t completed_fence_value)
{
    return !in_flight &&
           (!consumer_fence_value ||
            completed_fence_value >= consumer_fence_value);
}
