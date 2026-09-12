/*
 * This file is part of mpv.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "params.h"

const struct dlssnr_options dlssnr_defaults = {
    .enabled = true,
    .intensity = 1,
    .local_tone = 1,
    .local_structure = 1,
    .skin_structure = -1,
    .auto_mask = true,
    .ui_correction = true,
    .scaling = true,
    .input_resolution = 100,
    .residual_multiplier = 1,
    .residual_saturation = 1,
    .residual_lightness = 1,
    .shadow_structure = 1,
    .reflection_glow = 1,
};

enum dlssnr_model_path_type dlssnr_model_path_type(const char *path)
{
    if (!path || !path[0])
        return DLSSNR_MODEL_DEFAULT;
    size_t length = strlen(path);
    if (length >= 32768)
        return DLSSNR_MODEL_INVALID;
    for (size_t n = 0; n < length; n++) {
        if ((unsigned char)path[n] < 32)
            return DLSSNR_MODEL_INVALID;
    }
    bool drive_letter = (path[0] >= 'a' && path[0] <= 'z') ||
                        (path[0] >= 'A' && path[0] <= 'Z');
    if (length >= 3 && drive_letter && path[1] == ':' &&
        (path[2] == '\\' || path[2] == '/'))
        return DLSSNR_MODEL_ABSOLUTE;
    if (length >= 3 && path[0] == '\\' && path[1] == '\\')
        return DLSSNR_MODEL_ABSOLUTE;
    if (path[0] == '\\' || path[0] == '/' || strchr(path, ':'))
        return DLSSNR_MODEL_INVALID;
    return DLSSNR_MODEL_RELATIVE;
}

bool dlssnr_options_valid(const struct dlssnr_options *o,
                         char *error, size_t size)
{
#define RANGE(member, minimum, maximum) do {                                 \
    if (!isfinite((double)o->member) || o->member < (minimum) ||               \
        o->member > (maximum)) {                                             \
        snprintf(error, size, "Invalid finite range for " #member);           \
        return false;                                                       \
    }                                                                       \
} while (0)
    RANGE(preset, 0, 3);
    RANGE(style, 0, 2);
    RANGE(intensity, 0, 1);
    RANGE(local_tone, 0, 1);
    RANGE(local_structure, 0, 1);
    RANGE(skin_structure, -1, 2);
    RANGE(input_resolution, 25, 100);
    RANGE(residual_multiplier, 1, 2);
    RANGE(residual_saturation, 0, 2);
    RANGE(residual_lightness, 0, 2);
    RANGE(shadow_structure, 0, 2);
    RANGE(reflection_glow, 0, 2);
    RANGE(motion_quality, 0, 5);
#undef RANGE
    if (o->max_height < 0) {
        snprintf(error, size, "max-height must be nonnegative");
        return false;
    }
    if (o->motion_quality || o->nvof_follow_scaling) {
        snprintf(error, size, "NVOF guidance is not implemented; "
                              "motion-quality must be 0 and "
                              "nvof-follow-scaling must be no");
        return false;
    }
    if (dlssnr_model_path_type(o->model_path) == DLSSNR_MODEL_INVALID) {
        snprintf(error, size, "model-path must be absolute or module-relative; "
                              "drive-relative/rooted paths and control characters are invalid");
        return false;
    }
    enum dlssnr_model_path_type cache_type = dlssnr_model_path_type(o->cache_path);
    if (cache_type != DLSSNR_MODEL_DEFAULT && cache_type != DLSSNR_MODEL_ABSOLUTE) {
        snprintf(error, size, "cache-path must be an absolute directory or empty "
                              "for the per-user cache; relative paths are invalid");
        return false;
    }
    if (size)
        error[0] = 0;
    return true;
}

bool dlssnr_model_options_equal(const struct dlssnr_options *a,
                               const struct dlssnr_options *b)
{
    const char *ap = a->model_path ? a->model_path : "";
    const char *bp = b->model_path ? b->model_path : "";
    const char *ac = a->cache_path ? a->cache_path : "";
    const char *bc = b->cache_path ? b->cache_path : "";
    return a->preset == b->preset && !strcmp(ap, bp) && !strcmp(ac, bc);
}

bool dlssnr_options_equal(const struct dlssnr_options *a,
                         const struct dlssnr_options *b)
{
    if (!dlssnr_model_options_equal(a, b))
        return false;
#define SAME(member) if (a->member != b->member) return false
    SAME(enabled);
    SAME(style);
    SAME(intensity);
    SAME(local_tone);
    SAME(local_structure);
    SAME(skin_structure);
    SAME(auto_mask);
    SAME(ui_correction);
    SAME(scaling);
    SAME(input_resolution);
    SAME(residual_multiplier);
    SAME(residual_saturation);
    SAME(residual_lightness);
    SAME(shadow_structure);
    SAME(reflection_glow);
    SAME(motion_quality);
    SAME(nvof_follow_scaling);
    SAME(max_height);
#undef SAME
    return true;
}

bool dlssnr_processing_extent(const struct dlssnr_options *o, int w, int h,
                             int *proc_w, int *proc_h)
{
    if (w < 1 || h < 1 || o->input_resolution < 25 ||
        o->input_resolution > 100)
        return false;
    int percent = o->scaling ? o->input_resolution : 100;
    *proc_w = (int)(((int64_t)w * percent + 99) / 100);
    *proc_h = (int)(((int64_t)h * percent + 99) / 100);
    return true;
}

bool dlssnr_yuv_matrix(int bits, bool full_range, double kr, double kb,
                      float matrix[3][4])
{
    if ((bits != 8 && bits != 10) || !isfinite(kr) || !isfinite(kb) ||
        kr <= 0 || kb <= 0 || kr + kb >= 1)
        return false;
    double factor = 1 << (bits - 8);
    double maximum = (1 << bits) - 1;
    double storage = bits == 10 ? 65535.0 / 64.0 : 255.0;
    double y_min = full_range ? 0 : 16 * factor;
    double y_span = full_range ? maximum : 219 * factor;
    double c_mid = 128 * factor;
    double c_span = full_range ? maximum : 224 * factor;
    double kg = 1 - kr - kb;
    double u[3] = {0, -2 * kb * (1 - kb) / kg, 2 * (1 - kb)};
    double v[3] = {2 * (1 - kr), -2 * kr * (1 - kr) / kg, 0};
    for (int channel = 0; channel < 3; channel++) {
        matrix[channel][0] = (float)(storage / y_span);
        matrix[channel][1] = (float)(u[channel] * storage / c_span);
        matrix[channel][2] = (float)(v[channel] * storage / c_span);
        matrix[channel][3] = (float)(-y_min / y_span -
                                     (u[channel] + v[channel]) * c_mid / c_span);
    }
    return true;
}
