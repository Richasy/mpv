/*
 * This file is part of mpv.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef MP_DLSSNR_PARAMS_H
#define MP_DLSSNR_PARAMS_H

#include <stdbool.h>
#include <stddef.h>

struct dlssnr_options {
    char *model_path;
    char *cache_path;
    bool enabled;
    int preset;
    int style;
    float intensity;
    float local_tone;
    float local_structure;
    float skin_structure;
    bool auto_mask;
    bool ui_correction;
    bool scaling;
    int input_resolution;
    float residual_multiplier;
    float residual_saturation;
    float residual_lightness;
    float shadow_structure;
    float reflection_glow;
    int motion_quality;
    bool nvof_follow_scaling;
    int max_height;
};

extern const struct dlssnr_options dlssnr_defaults;

enum dlssnr_model_path_type {
    DLSSNR_MODEL_DEFAULT,
    DLSSNR_MODEL_RELATIVE,
    DLSSNR_MODEL_ABSOLUTE,
    DLSSNR_MODEL_INVALID,
};

enum dlssnr_model_path_type dlssnr_model_path_type(const char *path);
bool dlssnr_options_valid(const struct dlssnr_options *opts,
                         char *error, size_t error_size);
bool dlssnr_model_options_equal(const struct dlssnr_options *a,
                               const struct dlssnr_options *b);
bool dlssnr_options_equal(const struct dlssnr_options *a,
                         const struct dlssnr_options *b);
bool dlssnr_processing_extent(const struct dlssnr_options *opts, int w, int h,
                             int *proc_w, int *proc_h);
bool dlssnr_yuv_matrix(int bits, bool full_range, double kr, double kb,
                      float matrix[3][4]);

#endif
