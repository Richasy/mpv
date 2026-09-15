/*
 * This file is part of mpv.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include <limits.h>
#include <string.h>

#include "mpv_talloc.h"
#include "common/common.h"
#include "common/msg.h"
#include "osdep/threads.h"
#include "sub/dec_sub.h"
#include "sub/ocr_engine.h"
#include "sub_ocr.h"

#define OCR_QUEUE_LIMIT 32
#define OCR_BYTE_LIMIT (32u * 1024u * 1024u)

struct ocr_job {
    uint64_t id, generation;
    struct mp_ocr_image *images;
    int num_images;
    size_t bytes;
};

struct sub_ocr_worker {
    mp_mutex lock;
    mp_cond condition;
    mp_thread thread;
    bool stopping, running;
    uint64_t generation;
    size_t bytes;
    struct ocr_job *jobs[OCR_QUEUE_LIMIT];
    int num_jobs;
    struct sub_ocr_completion *completed[OCR_QUEUE_LIMIT];
    int num_completed;
    struct mp_ocr_engine *engine;
    struct mp_log *log;
    char *model, *dictionary, *runtime;
    void (*wakeup)(void *);
    void *wakeup_ctx;
};

static void clear_queues(struct sub_ocr_worker *w)
{
    for (int n = 0; n < w->num_jobs; n++) {
        w->bytes -= w->jobs[n]->bytes;
        talloc_free(w->jobs[n]);
    }
    w->num_jobs = 0;
    for (int n = 0; n < w->num_completed; n++)
        talloc_free(w->completed[n]);
    w->num_completed = 0;
}

static MP_THREAD_VOID ocr_thread(void *arg)
{
    struct sub_ocr_worker *w = arg;
    char *initial_error = NULL;
    bool attempted = false;
    mp_thread_set_name("subtitle-ocr");
    mp_mutex_lock(&w->lock);
    while (!w->stopping) {
        while (!w->stopping && !w->num_jobs)
            mp_cond_wait(&w->condition, &w->lock);
        if (w->stopping)
            break;
        struct ocr_job *job = w->jobs[0];
        MP_TARRAY_REMOVE_AT(w->jobs, w->num_jobs, 0);
        w->running = true;
        mp_mutex_unlock(&w->lock);

        if (!attempted) {
            struct mp_ocr_engine *engine = mp_ocr_engine_create(
                w->log, w->model, w->dictionary, w->runtime, &initial_error);
            mp_mutex_lock(&w->lock);
            w->engine = engine;
            attempted = true;
            mp_mutex_unlock(&w->lock);
        }

        struct mp_ocr_result *result = NULL;
        mp_mutex_lock(&w->lock);
        bool current = !w->stopping && job->generation == w->generation;
        if (current && w->engine)
            mp_ocr_engine_reset_cancel(w->engine);
        mp_mutex_unlock(&w->lock);
        if (current) {
            if (w->engine) {
                mp_ocr_engine_recognize(w->engine, job->images,
                                       job->num_images, &result);
            } else {
                result = talloc_zero(NULL, struct mp_ocr_result);
                result->error = talloc_strdup(result, initial_error
                    ? initial_error : "OCR engine initialization failed");
            }
            if (!result) {
                result = talloc_zero(NULL, struct mp_ocr_result);
                result->error = talloc_strdup(result, "OCR returned no result");
            }
        }

        mp_mutex_lock(&w->lock);
        w->running = false;
        w->bytes -= job->bytes;
        bool publish = !w->stopping && job->generation == w->generation;
        if (publish) {
            struct sub_ocr_completion *completion =
                talloc_zero(NULL, struct sub_ocr_completion);
            *completion = (struct sub_ocr_completion){
                .id = job->id, .generation = job->generation,
                .result = talloc_steal(completion, result),
            };
            result = NULL;
            mp_assert(w->num_completed < OCR_QUEUE_LIMIT);
            w->completed[w->num_completed++] = completion;
        }
        talloc_free(result);
        talloc_free(job);
        mp_mutex_unlock(&w->lock);
        if (publish)
            w->wakeup(w->wakeup_ctx);
        mp_mutex_lock(&w->lock);
    }
    mp_mutex_unlock(&w->lock);
    mp_ocr_engine_destroy(w->engine);
    talloc_free(initial_error);
    MP_THREAD_RETURN();
}

struct sub_ocr_worker *sub_ocr_worker_create(
    struct mp_log *log, const char *model, const char *dictionary,
    const char *runtime, void (*wakeup)(void *), void *wakeup_ctx)
{
    struct sub_ocr_worker *w = talloc_zero(NULL, struct sub_ocr_worker);
    w->log = log;
    w->model = talloc_strdup(w, model);
    w->dictionary = talloc_strdup(w, dictionary);
    w->runtime = runtime ? talloc_strdup(w, runtime) : NULL;
    w->wakeup = wakeup;
    w->wakeup_ctx = wakeup_ctx;
    w->generation = 1;
    mp_mutex_init(&w->lock);
    mp_cond_init(&w->condition);
    if (mp_thread_create(&w->thread, ocr_thread, w)) {
        mp_cond_destroy(&w->condition);
        mp_mutex_destroy(&w->lock);
        talloc_free(w);
        return NULL;
    }
    return w;
}

enum sub_ocr_submit_result sub_ocr_worker_submit(
    struct sub_ocr_worker *w, const struct sub_bitmap_cue *cue)
{
    if (!cue || cue->num_parts <= 0 || cue->num_parts > 64)
        return SUB_OCR_INVALID;
    size_t bytes = 0;
    for (int n = 0; n < cue->num_parts; n++) {
        const struct sub_bitmap_part *p = &cue->parts[n];
        if (!p->indices || !p->palette || p->w <= 0 || p->h <= 0 ||
            p->w > 8192 || p->h > 8192 || p->stride < p->w ||
            p->num_colors <= 0 || p->num_colors > 256)
            return SUB_OCR_INVALID;
        bytes += (size_t)p->w * p->h * 4;
        if (bytes > OCR_BYTE_LIMIT)
            return SUB_OCR_INVALID;
    }

    mp_mutex_lock(&w->lock);
    if (w->stopping || w->num_jobs + w->num_completed + w->running >=
            OCR_QUEUE_LIMIT || bytes > OCR_BYTE_LIMIT - w->bytes)
    {
        mp_mutex_unlock(&w->lock);
        return SUB_OCR_BACKPRESSURE;
    }
    struct ocr_job *job = talloc_zero(NULL, struct ocr_job);
    job->id = cue->id;
    job->generation = w->generation;
    job->bytes = bytes;
    job->num_images = cue->num_parts;
    job->images = talloc_array(job, struct mp_ocr_image, cue->num_parts);
    for (int n = 0; n < cue->num_parts; n++) {
        const struct sub_bitmap_part *p = &cue->parts[n];
        uint8_t *pixels = talloc_size(job, (size_t)p->w * p->h * 4);
        for (int y = 0; y < p->h; y++) {
            for (int x = 0; x < p->w; x++) {
                int index = p->indices[(size_t)y * p->stride + x];
                if (index >= p->num_colors) {
                    talloc_free(job);
                    mp_mutex_unlock(&w->lock);
                    return SUB_OCR_INVALID;
                }
                uint32_t color = p->palette[index];
                size_t offset = ((size_t)y * p->w + x) * 4;
                pixels[offset] = color;
                pixels[offset + 1] = color >> 8;
                pixels[offset + 2] = color >> 16;
                pixels[offset + 3] = color >> 24;
            }
        }
        job->images[n] = (struct mp_ocr_image){
            .bgra = pixels, .w = p->w, .h = p->h, .stride = p->w * 4,
            .x = p->x, .y = p->y,
        };
    }
    w->jobs[w->num_jobs++] = job;
    w->bytes += bytes;
    mp_cond_signal(&w->condition);
    mp_mutex_unlock(&w->lock);
    return SUB_OCR_QUEUED;
}

struct sub_ocr_completion *sub_ocr_worker_poll(struct sub_ocr_worker *w)
{
    mp_mutex_lock(&w->lock);
    struct sub_ocr_completion *completion = NULL;
    if (w->num_completed) {
        completion = w->completed[0];
        MP_TARRAY_REMOVE_AT(w->completed, w->num_completed, 0);
    }
    mp_mutex_unlock(&w->lock);
    return completion;
}

void sub_ocr_worker_invalidate(struct sub_ocr_worker *w)
{
    if (!w)
        return;
    mp_mutex_lock(&w->lock);
    w->generation++;
    clear_queues(w);
    if (w->engine)
        mp_ocr_engine_cancel(w->engine);
    mp_mutex_unlock(&w->lock);
}

int sub_ocr_worker_pending(struct sub_ocr_worker *w)
{
    if (!w)
        return 0;
    mp_mutex_lock(&w->lock);
    int pending = w->num_jobs + w->num_completed + w->running;
    mp_mutex_unlock(&w->lock);
    return pending;
}

void sub_ocr_worker_destroy(struct sub_ocr_worker *w)
{
    if (!w)
        return;
    mp_mutex_lock(&w->lock);
    w->stopping = true;
    clear_queues(w);
    if (w->engine)
        mp_ocr_engine_cancel(w->engine);
    mp_cond_signal(&w->condition);
    mp_mutex_unlock(&w->lock);
    mp_thread_join(w->thread);
    mp_cond_destroy(&w->condition);
    mp_mutex_destroy(&w->lock);
    talloc_free(w);
}
