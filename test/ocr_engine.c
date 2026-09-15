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

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The deterministic tests exercise file-local preprocessing directly. The
 * Meson test executable should compile this source instead of separately
 * compiling sub/ocr_engine.c.
 */
#include "../sub/ocr_engine.c"

static void paint(uint8_t *pixels, int stride, int x0, int y0, int x1, int y1,
                  uint8_t value, uint8_t alpha)
{
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            uint8_t *pixel = pixels + y * stride + x * 4;
            pixel[0] = value;
            pixel[1] = value;
            pixel[2] = value;
            pixel[3] = alpha;
        }
    }
}

static void check_segmentation_and_order(void)
{
    uint8_t first[32 * 24 * 4] = {0};
    uint8_t second[20 * 12 * 4] = {0};

    /* A detached dot separated by three empty rows remains with its stem. */
    paint(first, 32 * 4, 8, 1, 10, 3, 255, 255);
    paint(first, 32 * 4, 6, 6, 24, 12, 255, 255);
    paint(first, 32 * 4, 4, 18, 28, 22, 255, 255);
    paint(second, 20 * 4, 2, 3, 18, 9, 255, 255);

    struct mp_ocr_image images[] = {
        {.bgra = first, .w = 32, .h = 24, .stride = 32 * 4,
         .x = 100, .y = 100},
        {.bgra = second, .w = 20, .h = 12, .stride = 20 * 4,
         .x = 40, .y = 70},
    };
    struct line_box boxes[OCR_MAX_LINES];
    int count = 0;
    char *error = NULL;
    assert(segment_images(images, 2, boxes, &count, &error, NULL));
    assert(!error && count == 3);
    qsort(boxes, count, sizeof(boxes[0]), compare_boxes);

    assert(boxes[0].region == 1);
    assert(boxes[0].x0 == 42 && boxes[0].x1 == 58);
    assert(boxes[0].y0 == 73 && boxes[0].y1 == 79);
    assert(boxes[1].region == 0);
    assert(boxes[1].x0 == 106 && boxes[1].x1 == 124);
    assert(boxes[1].y0 == 101 && boxes[1].y1 == 112);
    assert(boxes[2].region == 0);
    assert(boxes[2].x0 == 104 && boxes[2].x1 == 128);
    assert(boxes[2].y0 == 118 && boxes[2].y1 == 122);
}

static void check_preprocessing(void)
{
    uint8_t pixels[24 * 12 * 4] = {0};
    paint(pixels, 24 * 4, 4, 2, 20, 10, 255, 255);
    struct mp_ocr_image image = {
        .bgra = pixels, .w = 24, .h = 12, .stride = 24 * 4,
    };
    struct line_box box = {.x0 = 4, .y0 = 2, .x1 = 20, .y1 = 10};
    struct prepared_line prepared = {0};
    char *error = NULL;
    void *tmp = talloc_new(NULL);
    assert(prepare_line(tmp, &image, &box, &prepared, &error));
    assert(!error);
    assert(prepared.width == OCR_MIN_INPUT_WIDTH);
    assert(prepared.elements ==
           (size_t)3 * OCR_INPUT_HEIGHT * OCR_MIN_INPUT_WIDTH);

    size_t plane = (size_t)OCR_INPUT_HEIGHT * prepared.width;
    float minimum = 1.0f;
    float maximum = -1.0f;
    for (size_t n = 0; n < plane; n++) {
        assert(isfinite(prepared.data[n]));
        assert(prepared.data[n] == prepared.data[plane + n]);
        assert(prepared.data[n] == prepared.data[plane * 2 + n]);
        if (prepared.data[n] < minimum)
            minimum = prepared.data[n];
        if (prepared.data[n] > maximum)
            maximum = prepared.data[n];
    }
    assert(minimum < -0.9f);
    assert(maximum > 0.9f);
    for (int y = 0; y < OCR_INPUT_HEIGHT; y++)
        assert(prepared.data[(size_t)y * prepared.width +
                             prepared.width - 1] == 0.0f);
    talloc_free(tmp);
}

static void check_bounds_and_empty_images(void)
{
    uint8_t pixel[4] = {255, 255, 255, 0};
    struct mp_ocr_image empty = {
        .bgra = pixel, .w = 1, .h = 1, .stride = 4,
    };
    struct line_box boxes[OCR_MAX_LINES];
    int count = 0;
    char *error = NULL;
    assert(!segment_images(&empty, 1, boxes, &count, &error, NULL));
    assert(error && strstr(error, "no visible pixels"));
    talloc_free(error);

    struct mp_ocr_image invalid = empty;
    invalid.w = OCR_MAX_IMAGE_DIMENSION + 1;
    error = NULL;
    assert(!segment_images(&invalid, 1, boxes, &count, &error, NULL));
    assert(error && strstr(error, "invalid dimensions"));
    talloc_free(error);

    invalid = empty;
    invalid.x = INT_MAX;
    error = NULL;
    assert(!segment_images(&invalid, 1, boxes, &count, &error, NULL));
    assert(error && strstr(error, "invalid dimensions"));
    talloc_free(error);

    error = NULL;
    assert(!segment_images(&empty, OCR_MAX_IMAGES + 1, boxes, &count,
                           &error, NULL));
    assert(error && strstr(error, "image count"));
    talloc_free(error);
}

#ifndef OCR_ENGINE_DETERMINISTIC_ONLY
static void check_model(const char *runtime, const char *model,
                        const char *dictionary)
{
    char *error = NULL;
    mp_ocr_engine *engine =
        mp_ocr_engine_create(NULL, model, dictionary, runtime, &error);
    if (!engine) {
        fprintf(stderr, "OCR model initialization failed: %s\n",
                error ? error : "unknown error");
        talloc_free(error);
        abort();
    }

    uint8_t pixels[96 * 32 * 4] = {0};
    paint(pixels, 96 * 4, 8, 5, 88, 27, 255, 255);
    struct mp_ocr_image image = {
        .bgra = pixels, .w = 96, .h = 32, .stride = 96 * 4,
        .x = 20, .y = 30,
    };
    struct mp_ocr_result *result = NULL;

    mp_ocr_engine_cancel(engine);
    assert(!mp_ocr_engine_recognize(engine, &image, 1, &result));
    assert(result && result->error && strstr(result->error, "cancelled"));
    talloc_free(result);

    mp_ocr_engine_reset_cancel(engine);
    assert(mp_ocr_engine_recognize(engine, &image, 1, &result));
    assert(result && !result->error && result->num_lines == 1);
    assert(result->lines[0].x == 28 && result->lines[0].y == 35);
    assert(result->lines[0].w == 80 && result->lines[0].h == 22);
    assert(result->lines[0].region == 0);
    assert(result->lines[0].text);
    assert(result->lines[0].confidence >= 0.0f &&
           result->lines[0].confidence <= 1.0f);
    talloc_free(result);
    mp_ocr_engine_destroy(engine);
}
#endif

int main(int argc, char **argv)
{
    check_segmentation_and_order();
    check_preprocessing();
    check_bounds_and_empty_images();
#ifndef OCR_ENGINE_DETERMINISTIC_ONLY
    if (argc == 4) {
        check_model(argv[1], argv[2], argv[3]);
    } else if (argc != 1) {
        fprintf(stderr, "usage: %s [runtime.dll model.onnx dictionary.json]\n",
                argv[0]);
        return 2;
    }
#else
    (void)argv;
    if (argc != 1)
        return 2;
#endif
    puts("ocr_engine: all tests passed");
    return 0;
}
