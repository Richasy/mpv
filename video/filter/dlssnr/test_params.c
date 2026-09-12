/*
 * This file is part of mpv.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "params.h"

int main(void)
{
    struct dlssnr_options o = dlssnr_defaults;
    char error[256];
    int w, h;
    assert(dlssnr_options_valid(&o, error, sizeof(error)));
    assert(dlssnr_processing_extent(&o, 1920, 1080, &w, &h));
    assert(w == 1920 && h == 1080);
    o.input_resolution = 25;
    assert(dlssnr_processing_extent(&o, 1921, 1081, &w, &h));
    assert(w == 481 && h == 271);
    o.scaling = false;
    assert(dlssnr_processing_extent(&o, 1921, 1081, &w, &h));
    assert(w == 1921 && h == 1081);
    assert(dlssnr_processing_extent(&o, INT_MAX, INT_MAX, &w, &h));
    assert(w == INT_MAX && h == INT_MAX);
    assert(!dlssnr_processing_extent(&o, 0, 1080, &w, &h));

    o = dlssnr_defaults;
    o.intensity = NAN;
    assert(!dlssnr_options_valid(&o, error, sizeof(error)));
    o.intensity = INFINITY;
    assert(!dlssnr_options_valid(&o, error, sizeof(error)));
    o.intensity = -0.01f;
    assert(!dlssnr_options_valid(&o, error, sizeof(error)));
    o = dlssnr_defaults;
    o.model_path = "relative.dll";
    assert(dlssnr_options_valid(&o, error, sizeof(error)));
    assert(dlssnr_model_path_type(o.model_path) == DLSSNR_MODEL_RELATIVE);
    o.model_path = "ngx\\nvngx_dlssnr.dll";
    assert(dlssnr_options_valid(&o, error, sizeof(error)));
    o.model_path = "C:relative.dll";
    assert(!dlssnr_options_valid(&o, error, sizeof(error)));
    o.model_path = "\\relative.dll";
    assert(!dlssnr_options_valid(&o, error, sizeof(error)));
    o.model_path = "C:\\private\\nvngx_dlssnr.dll";
    assert(dlssnr_options_valid(&o, error, sizeof(error)));
    assert(dlssnr_model_path_type(o.model_path) == DLSSNR_MODEL_ABSOLUTE);
    o.model_path = "C:\\bad\npath\\nvngx_dlssnr.dll";
    assert(!dlssnr_options_valid(&o, error, sizeof(error)));
    o = dlssnr_defaults;
    assert(dlssnr_model_path_type(o.model_path) == DLSSNR_MODEL_DEFAULT);
    o.cache_path = "C:\\app data\\cache";
    assert(dlssnr_options_valid(&o, error, sizeof(error)));
    o.cache_path = "\\\\server\\share\\cache";
    assert(dlssnr_options_valid(&o, error, sizeof(error)));
    const char *invalid_cache[] = {
        "cache", "C:cache", "\\cache", "/cache", "C:\\bad\npath",
    };
    for (size_t n = 0; n < sizeof(invalid_cache) / sizeof(invalid_cache[0]); n++) {
        o.cache_path = (char *)invalid_cache[n];
        assert(!dlssnr_options_valid(&o, error, sizeof(error)));
        assert(strstr(error, "cache-path"));
    }
    o.cache_path = "";
    assert(dlssnr_options_valid(&o, error, sizeof(error)));
    o.motion_quality = 1;
    assert(!dlssnr_options_valid(&o, error, sizeof(error)));
    o.motion_quality = 0;
    o.nvof_follow_scaling = true;
    assert(!dlssnr_options_valid(&o, error, sizeof(error)));

    struct dlssnr_options other = dlssnr_defaults;
    o = dlssnr_defaults;
    o.style = 2;
    o.intensity = 0.25f;
    o.residual_multiplier = 2;
    assert(dlssnr_model_options_equal(&o, &other));
    o.preset = 1;
    assert(!dlssnr_model_options_equal(&o, &other));
    o = dlssnr_defaults;
    o.cache_path = "";
    assert(dlssnr_options_equal(&o, &other));
    o.cache_path = "C:\\app data\\cache";
    assert(!dlssnr_model_options_equal(&o, &other));
    assert(!dlssnr_options_equal(&o, &other));
    other.cache_path = o.cache_path;
    assert(dlssnr_options_equal(&o, &other));

    for (int bits = 8; bits <= 10; bits += 2) {
        float matrix[3][4];
        assert(dlssnr_yuv_matrix(bits, false, 0.2126, 0.0722, matrix));
        double factor = 1 << (bits - 8);
        double storage = bits == 10 ? 65535.0 / 64.0 : 255.0;
        for (int channel = 0; channel < 3; channel++) {
            double neutral = (matrix[channel][1] + matrix[channel][2]) *
                              (128 * factor / storage) + matrix[channel][3];
            double black = matrix[channel][0] * (16 * factor / storage) + neutral;
            double white = matrix[channel][0] * (235 * factor / storage) + neutral;
            assert(fabs(black) < 0.000001);
            assert(fabs(white - 1) < 0.000001);
        }
    }
    puts("DLSSNR parameter tests passed");
    return 0;
}
