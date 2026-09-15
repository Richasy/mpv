/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef MP_OCR_ENGINE_H
#define MP_OCR_ENGINE_H

#include <stdbool.h>
#include <stdint.h>

struct mp_log;
typedef struct mp_ocr_engine mp_ocr_engine;

struct mp_ocr_image {
    const uint8_t *bgra;
    int w;
    int h;
    int stride;
    int x;
    int y;
};

struct mp_ocr_line {
    char *text;
    float confidence;
    int x;
    int y;
    int w;
    int h;
    int region;
};

struct mp_ocr_result {
    struct mp_ocr_line *lines;
    int num_lines;
    char *error;
};

mp_ocr_engine *mp_ocr_engine_create(struct mp_log *log,
                                    const char *model_path,
                                    const char *dictionary_path,
                                    const char *runtime_path,
                                    char **error);
bool mp_ocr_engine_recognize(mp_ocr_engine *engine,
                             const struct mp_ocr_image *images,
                             int num_images,
                             struct mp_ocr_result **result);
void mp_ocr_engine_cancel(mp_ocr_engine *engine);
void mp_ocr_engine_reset_cancel(mp_ocr_engine *engine);
void mp_ocr_engine_destroy(mp_ocr_engine *engine);

#endif
