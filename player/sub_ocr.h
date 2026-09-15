/*
 * This file is part of mpv.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#ifndef MP_SUB_OCR_H
#define MP_SUB_OCR_H

#include <stdbool.h>
#include <stdint.h>

struct mp_log;
struct sub_bitmap_cue;
struct mp_ocr_result;
struct sub_ocr_worker;

struct sub_ocr_completion {
    uint64_t id;
    uint64_t generation;
    struct mp_ocr_result *result;
};

enum sub_ocr_submit_result {
    SUB_OCR_QUEUED,
    SUB_OCR_BACKPRESSURE,
    SUB_OCR_INVALID,
};

struct sub_ocr_worker *sub_ocr_worker_create(
    struct mp_log *log, const char *model, const char *dictionary,
    const char *runtime, void (*wakeup)(void *), void *wakeup_ctx);
enum sub_ocr_submit_result sub_ocr_worker_submit(
    struct sub_ocr_worker *worker, const struct sub_bitmap_cue *cue);
struct sub_ocr_completion *sub_ocr_worker_poll(struct sub_ocr_worker *worker);
void sub_ocr_worker_invalidate(struct sub_ocr_worker *worker);
int sub_ocr_worker_pending(struct sub_ocr_worker *worker);
void sub_ocr_worker_destroy(struct sub_ocr_worker *worker);

#endif
