/*
 * This file is part of mpv.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include <assert.h>
#include <errno.h>
#include <string.h>

#include "mpv_talloc.h"
#include "osdep/threads.h"
#include "osdep/timer.h"
#include "player/sub_ocr.h"
#include "sub/dec_sub.h"
#include "sub/ocr_engine.h"

struct mp_ocr_engine { int unused; };
static struct mp_ocr_engine engine;
static mp_mutex lock = MP_STATIC_MUTEX_INITIALIZER;
static mp_cond condition = MP_STATIC_COND_INITIALIZER;
static int calls, wakeups;
static bool cancelled, released, destroyed;

struct mp_ocr_engine *mp_ocr_engine_create(
    struct mp_log *log, const char *model, const char *dictionary,
    const char *runtime, char **error)
{
    assert(!strcmp(model, "fixture"));
    return &engine;
}

bool mp_ocr_engine_recognize(struct mp_ocr_engine *e,
                            const struct mp_ocr_image *images, int count,
                            struct mp_ocr_result **out)
{
    assert(count == 1 && images[0].w == 1 && images[0].h == 1);
    assert(images[0].bgra[0] == 0x33 && images[0].bgra[1] == 0x22 &&
           images[0].bgra[2] == 0x11 && images[0].bgra[3] == 0xff);
    mp_mutex_lock(&lock);
    calls++;
    mp_cond_broadcast(&condition);
    while (!cancelled && !released)
        mp_cond_wait(&condition, &lock);
    bool success = !cancelled;
    mp_mutex_unlock(&lock);
    *out = talloc_zero(NULL, struct mp_ocr_result);
    if (success) {
        (*out)->lines = talloc_zero_array(*out, struct mp_ocr_line, 1);
        (*out)->num_lines = 1;
        (*out)->lines[0].text = talloc_strdup(*out, "fixture");
    } else {
        (*out)->error = talloc_strdup(*out, "cancelled");
    }
    return success;
}

void mp_ocr_engine_cancel(struct mp_ocr_engine *e)
{
    mp_mutex_lock(&lock);
    cancelled = true;
    mp_cond_broadcast(&condition);
    mp_mutex_unlock(&lock);
}

void mp_ocr_engine_reset_cancel(struct mp_ocr_engine *e)
{
    mp_mutex_lock(&lock);
    cancelled = false;
    mp_mutex_unlock(&lock);
}

void mp_ocr_engine_destroy(struct mp_ocr_engine *e)
{
    destroyed = true;
}

static void wakeup(void *ctx)
{
    mp_mutex_lock(&lock);
    wakeups++;
    mp_cond_broadcast(&condition);
    mp_mutex_unlock(&lock);
}

static void wait_for(int *value, int expected)
{
    int64_t until = mp_time_ns() + MP_TIME_S_TO_NS(5);
    mp_mutex_lock(&lock);
    while (*value < expected) {
        int result = mp_cond_timedwait_until(&condition, &lock, until);
        assert(result != ETIMEDOUT);
    }
    mp_mutex_unlock(&lock);
}

int main(void)
{
    mp_time_init();
    struct sub_ocr_worker *worker = sub_ocr_worker_create(
        NULL, "fixture", "fixture", NULL, wakeup, NULL);
    assert(worker);
    uint8_t index = 0;
    uint32_t color = 0xff112233;
    struct sub_bitmap_part part = {
        .indices = &index, .palette = &color,
        .w = 1, .h = 1, .stride = 1, .num_colors = 1,
    };
    struct sub_bitmap_cue cue = {.id = 1, .parts = &part, .num_parts = 1};
    assert(sub_ocr_worker_submit(worker, &cue) == SUB_OCR_QUEUED);
    wait_for(&calls, 1);
    for (int n = 0; n < 31; n++) {
        cue.id++;
        assert(sub_ocr_worker_submit(worker, &cue) == SUB_OCR_QUEUED);
    }
    assert(sub_ocr_worker_submit(worker, &cue) == SUB_OCR_BACKPRESSURE);
    sub_ocr_worker_invalidate(worker);
    cue.id = 100;
    assert(sub_ocr_worker_submit(worker, &cue) == SUB_OCR_QUEUED);
    color = 0;
    wait_for(&calls, 2);
    mp_mutex_lock(&lock);
    assert(wakeups == 0);
    released = true;
    mp_cond_broadcast(&condition);
    mp_mutex_unlock(&lock);
    wait_for(&wakeups, 1);
    struct sub_ocr_completion *completion = sub_ocr_worker_poll(worker);
    assert(completion && completion->id == 100);
    assert(!completion->result->error && completion->result->num_lines == 1);
    talloc_free(completion);
    assert(!sub_ocr_worker_poll(worker));
    assert(sub_ocr_worker_pending(worker) == 0);

    color = 0xff112233;
    index = 2;
    assert(sub_ocr_worker_submit(worker, &cue) == SUB_OCR_INVALID);
    index = 0;
    mp_mutex_lock(&lock);
    released = false;
    mp_mutex_unlock(&lock);
    cue.id = 101;
    assert(sub_ocr_worker_submit(worker, &cue) == SUB_OCR_QUEUED);
    wait_for(&calls, 3);
    sub_ocr_worker_destroy(worker);
    assert(destroyed && wakeups == 1);
    return 0;
}
