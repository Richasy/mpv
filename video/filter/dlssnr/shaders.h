/*
 * This file is part of mpv.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef MP_DLSSNR_SHADERS_H
#define MP_DLSSNR_SHADERS_H

#include <stdint.h>

struct dlssnr_shader_config {
    uint32_t width, height, proc_width, proc_height;
    uint32_t texture_width, texture_height, input_yuv, reserved0;
    float matrix[3][4];
    float chroma_x, chroma_y, multiplier, saturation;
    float lightness, shadow, glow, reserved1;
    float luma[4];
};

extern const char dlssnr_shader_source[];

#endif
