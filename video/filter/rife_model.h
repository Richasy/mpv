#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static inline bool mp_rife_valid_padding(int padding)
{
    return padding == 32 || padding == 64 || padding == 128;
}

static inline int mp_rife_pad_dimension(int value, int padding)
{
    return (value + padding - 1) & ~(padding - 1);
}

static inline bool mp_rife_tensor_shape_matches(const int64_t *dims,
                                                size_t rank,
                                                int channels,
                                                int height,
                                                int width)
{
    if (rank != 4)
        return false;
    return (dims[0] == 1 || dims[0] < 0) &&
           dims[1] == channels &&
           (dims[2] == height || dims[2] < 0) &&
           (dims[3] == width || dims[3] < 0);
}
