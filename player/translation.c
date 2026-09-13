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

#include <limits.h>
#include <math.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <mpv/client.h>

#include "mpv_talloc.h"

#include "common/common.h"
#include "common/msg.h"
#include "misc/json.h"
#include "misc/node.h"
#include "osdep/threads.h"
#include "osdep/timer.h"

#include "core.h"
#include "translation.h"

#define TRANSLATION_WORKERS_DEFAULT 3
#define TRANSLATION_WORKERS_MAX 8
#define TRANSLATION_PENDING_MAX 1024
#define TRANSLATION_RESULTS_MAX 128
#define TRANSLATION_OUTSTANDING_MAX 128
#define TRANSLATION_MIN_SLACK 1.0
#define TRANSLATION_DRAIN_EPSILON 0.05
#define TRANSLATION_DEFER_MS 200

#define TRANSLATION_DEFAULT_HORIZON_SEC 60
#define TRANSLATION_DEFAULT_SEEK_DEBOUNCE_MS 1500
#define TRANSLATION_DEFAULT_MIN_TEXT_CHARS 2
#define TRANSLATION_DEFAULT_REUSE_CACHE_CAPACITY 256
#define TRANSLATION_DEFAULT_REUSE_CACHE_WINDOW_MS 120000
#define TRANSLATION_DEFAULT_REPEAT_LOOP_THRESHOLD 5
#define TRANSLATION_DEFAULT_REPEAT_LOOP_WINDOW_MS 30000
#define TRANSLATION_REUSE_CACHE_HARD_CAP 512

struct translation_task {
    enum mp_translation_source source;
    uint64_t generation;
    uint64_t backend_epoch;
    uint64_t cue_id;
    uint64_t revision;
    int seq;
    double pts;
    double duration;
    unsigned flags;
    char *text;
};

struct translation_result {
    enum mp_translation_source source;
    uint64_t generation;
    uint64_t backend_epoch;
    uint64_t cue_id;
    uint64_t revision;
    int seq;
    double pts;
    double duration;
    char *text;
    char *translated;
    bool rate_limited;
    bool counted_inflight;
    char *error;
    enum mp_translation_result_kind kind;
};

struct translation_cache_entry {
    char *text;
    char *translated;
    uint64_t backend_epoch;
    int64_t inserted_ms;
    int64_t first_seen_ms;
    int hit_count;
};

struct parsed_common_config {
    enum wt_provider provider;
    char *source_lang;
    char *target_lang;
    struct wt_openai_config ai;
    struct mp_translation_limits limits;
};

struct mp_translation {
    struct mp_log *log;
    mp_translation_wakeup_fn wakeup;
    void *wakeup_ctx;

    mp_mutex state_lock;
    mp_mutex config_lock;
    bool source_active[MP_TRANSLATION_SOURCE_COUNT];
    uint64_t source_generation[MP_TRANSLATION_SOURCE_COUNT];
    double playback_pts;
    struct mp_translation_backend_ops backend_ops;
    void *backend;
    uint64_t backend_epoch;

    bool common_explicit;
    bool common_enabled;
    char *common_json;

    mp_mutex pending_lock;
    mp_cond pending_cv;
    struct translation_task **pending;
    int num_pending;
    int pending_capacity;
    int next_seq;
    int in_flight[MP_TRANSLATION_SOURCE_COUNT];
    int outstanding[MP_TRANSLATION_SOURCE_COUNT];

    mp_mutex result_lock;
    struct translation_result **results;
    int num_results;
    int result_capacity;

    mp_mutex limits_lock;
    struct mp_translation_limits limits;
    int64_t quiet_until_ms[MP_TRANSLATION_SOURCE_COUNT];
    double rpm_tokens;
    int64_t rpm_last_refill_ms;
    int session_requests;
    int horizon_deferred;
    int cache_reused;
    int loop_skipped;
    int short_skipped;
    int rpm_skipped;
    int budget_skipped;
    int queue_overflow;
    struct translation_cache_entry *cache;
    int cache_size;
    int cache_capacity;

    mp_thread workers[TRANSLATION_WORKERS_MAX];
    int worker_count;
    atomic_bool terminate;
};

static int64_t wall_ms(void)
{
    return mp_time_ns() / INT64_C(1000000);
}

static bool valid_source(enum mp_translation_source source)
{
    return source >= 0 && source < MP_TRANSLATION_SOURCE_COUNT;
}

static void set_error(char **error, const char *message)
{
    if (!error)
        return;
    *error = talloc_strdup(NULL, message);
}

static void limits_apply_defaults(struct mp_translation_limits *limits)
{
    if (limits->horizon_sec <= 0)
        limits->horizon_sec = TRANSLATION_DEFAULT_HORIZON_SEC;
    if (limits->seek_debounce_ms < 0)
        limits->seek_debounce_ms =
            TRANSLATION_DEFAULT_SEEK_DEBOUNCE_MS;
    if (limits->min_text_chars < 0)
        limits->min_text_chars = TRANSLATION_DEFAULT_MIN_TEXT_CHARS;
    if (limits->reuse_cache_capacity < 0)
        limits->reuse_cache_capacity =
            TRANSLATION_DEFAULT_REUSE_CACHE_CAPACITY;
    if (limits->reuse_cache_capacity > TRANSLATION_REUSE_CACHE_HARD_CAP)
        limits->reuse_cache_capacity = TRANSLATION_REUSE_CACHE_HARD_CAP;
    if (limits->reuse_cache_window_ms < 0)
        limits->reuse_cache_window_ms =
            TRANSLATION_DEFAULT_REUSE_CACHE_WINDOW_MS;
    if (limits->repeat_loop_threshold < 0)
        limits->repeat_loop_threshold =
            TRANSLATION_DEFAULT_REPEAT_LOOP_THRESHOLD;
    if (limits->repeat_loop_window_ms < 0)
        limits->repeat_loop_window_ms =
            TRANSLATION_DEFAULT_REPEAT_LOOP_WINDOW_MS;
    if (limits->rpm_limit < 0)
        limits->rpm_limit = 0;
    if (limits->session_request_limit < 0)
        limits->session_request_limit = 0;
}

static int utf8_chars(const char *text)
{
    int count = 0;
    if (!text)
        return 0;
    for (; *text; text++) {
        unsigned char c = *text;
        if ((c & 0xc0) != 0x80)
            count++;
    }
    return count;
}

static char *normalize_cache_text(void *parent, const char *text)
{
    if (!text)
        return NULL;
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')
        text++;
    if (!text[0])
        return NULL;

    char *normalized = talloc_strdup(parent, text);
    char *src = normalized;
    char *dst = normalized;
    bool previous_space = false;
    while (*src) {
        char c = *src++;
        if (c == '\t' || c == '\r' || c == '\n')
            c = ' ';
        if (c == ' ') {
            if (previous_space)
                continue;
            previous_space = true;
        } else {
            previous_space = false;
        }
        *dst++ = c;
    }
    while (dst > normalized && dst[-1] == ' ')
        dst--;
    *dst = '\0';
    if (!normalized[0]) {
        talloc_free(normalized);
        return NULL;
    }
    return normalized;
}

static struct translation_cache_entry *cache_lookup(
    struct mp_translation *translation, const char *text,
    uint64_t backend_epoch)
{
    if (!translation->cache || !text || translation->cache_size == 0)
        return NULL;
    for (int n = 0; n < translation->cache_size; n++) {
        if (translation->cache[n].backend_epoch == backend_epoch &&
            strcmp(translation->cache[n].text, text) == 0)
        {
            if (n > 0) {
                struct translation_cache_entry entry = translation->cache[n];
                memmove(&translation->cache[1], &translation->cache[0],
                        sizeof(entry) * n);
                translation->cache[0] = entry;
            }
            return &translation->cache[0];
        }
    }
    return NULL;
}

static void cache_clear_locked(struct mp_translation *translation)
{
    talloc_free(translation->cache);
    translation->cache = NULL;
    translation->cache_size = 0;
    translation->cache_capacity = 0;
}

static void cache_clamp_locked(struct mp_translation *translation)
{
    int capacity = translation->limits.reuse_cache_capacity;
    if (capacity <= 0) {
        cache_clear_locked(translation);
        return;
    }
    while (translation->cache_size > capacity) {
        int tail = translation->cache_size - 1;
        talloc_free(translation->cache[tail].text);
        talloc_free(translation->cache[tail].translated);
        translation->cache_size--;
    }
}

static void reset_session_locked(struct mp_translation *translation)
{
    translation->rpm_tokens = 0;
    translation->rpm_last_refill_ms = 0;
    translation->session_requests = 0;
    translation->horizon_deferred = 0;
    translation->cache_reused = 0;
    translation->loop_skipped = 0;
    translation->short_skipped = 0;
    translation->rpm_skipped = 0;
    translation->budget_skipped = 0;
    translation->queue_overflow = 0;
    cache_clear_locked(translation);
}

static void cache_put(struct mp_translation *translation, const char *text,
                      const char *translated, uint64_t backend_epoch)
{
    int capacity = translation->limits.reuse_cache_capacity;
    if (capacity <= 0 || !text || !translated)
        return;

    struct translation_cache_entry *entry =
        cache_lookup(translation, text, backend_epoch);
    int64_t now = wall_ms();
    if (entry) {
        talloc_free(entry->translated);
        entry->translated = talloc_strdup(translation->cache, translated);
        entry->inserted_ms = now;
        return;
    }

    if (!translation->cache || translation->cache_capacity < capacity) {
        translation->cache = talloc_realloc(
            translation, translation->cache,
            struct translation_cache_entry, capacity);
        memset(&translation->cache[translation->cache_capacity], 0,
               sizeof(*translation->cache) *
                   (capacity - translation->cache_capacity));
        translation->cache_capacity = capacity;
    }
    if (translation->cache_size < capacity) {
        memmove(&translation->cache[1], &translation->cache[0],
                sizeof(*translation->cache) * translation->cache_size);
        translation->cache_size++;
    } else {
        struct translation_cache_entry *tail =
            &translation->cache[capacity - 1];
        talloc_free(tail->text);
        talloc_free(tail->translated);
        memmove(&translation->cache[1], &translation->cache[0],
                sizeof(*translation->cache) * (capacity - 1));
    }
    translation->cache[0] = (struct translation_cache_entry){
        .text = talloc_strdup(translation->cache, text),
        .translated = talloc_strdup(translation->cache, translated),
        .backend_epoch = backend_epoch,
        .inserted_ms = now,
        .first_seen_ms = now,
        .hit_count = 1,
    };
}

static bool repeat_record_hit(struct mp_translation *translation,
                              const char *text, uint64_t backend_epoch)
{
    if (!text || translation->limits.repeat_loop_threshold <= 0 ||
        translation->limits.repeat_loop_window_ms <= 0)
    {
        return false;
    }

    int64_t now = wall_ms();
    struct translation_cache_entry *entry =
        cache_lookup(translation, text, backend_epoch);
    if (!entry) {
        int capacity = translation->limits.reuse_cache_capacity;
        if (capacity <= 0)
            return false;
        if (!translation->cache ||
            translation->cache_capacity < capacity)
        {
            translation->cache = talloc_realloc(
                translation, translation->cache,
                struct translation_cache_entry, capacity);
            memset(&translation->cache[translation->cache_capacity], 0,
                   sizeof(*translation->cache) *
                       (capacity - translation->cache_capacity));
            translation->cache_capacity = capacity;
        }
        if (translation->cache_size < capacity) {
            memmove(&translation->cache[1], &translation->cache[0],
                    sizeof(*translation->cache) * translation->cache_size);
            translation->cache_size++;
        } else {
            struct translation_cache_entry *tail =
                &translation->cache[capacity - 1];
            talloc_free(tail->text);
            talloc_free(tail->translated);
            memmove(&translation->cache[1], &translation->cache[0],
                    sizeof(*translation->cache) * (capacity - 1));
        }
        translation->cache[0] = (struct translation_cache_entry){
            .text = talloc_strdup(translation->cache, text),
            .backend_epoch = backend_epoch,
            .first_seen_ms = now,
            .hit_count = 1,
        };
        return false;
    }

    if (now - entry->first_seen_ms >
        translation->limits.repeat_loop_window_ms)
    {
        entry->first_seen_ms = now;
        entry->hit_count = 1;
        return false;
    }

    entry->hit_count++;
    return entry->hit_count >=
           translation->limits.repeat_loop_threshold;
}

static void refill_tokens(struct mp_translation *translation)
{
    int rpm = translation->limits.rpm_limit;
    if (rpm <= 0)
        return;
    int64_t now = wall_ms();
    if (translation->rpm_last_refill_ms == 0) {
        translation->rpm_last_refill_ms = now;
        translation->rpm_tokens = rpm;
        return;
    }
    double elapsed = now - translation->rpm_last_refill_ms;
    if (elapsed <= 0)
        return;
    translation->rpm_tokens += elapsed * (rpm / 60000.0);
    if (translation->rpm_tokens > rpm)
        translation->rpm_tokens = rpm;
    translation->rpm_last_refill_ms = now;
}

static bool reserve_request(
    struct mp_translation *translation,
    enum mp_translation_result_kind *reject_kind)
{
    if (translation->limits.session_request_limit > 0 &&
        translation->session_requests >=
            translation->limits.session_request_limit)
    {
        *reject_kind = MP_TRANSLATION_RESULT_FALLBACK_BUDGET;
        translation->budget_skipped++;
        return false;
    }

    refill_tokens(translation);
    if (translation->limits.rpm_limit > 0) {
        if (translation->rpm_tokens < 1.0) {
            *reject_kind = MP_TRANSLATION_RESULT_FALLBACK_RPM;
            translation->rpm_skipped++;
            return false;
        }
        translation->rpm_tokens -= 1.0;
    }
    translation->session_requests++;
    return true;
}

static void rollback_request(struct mp_translation *translation)
{
    if (translation->session_requests > 0)
        translation->session_requests--;
    if (translation->limits.rpm_limit > 0)
        translation->rpm_tokens += 1.0;
}

static bool task_is_current(struct mp_translation *translation,
                            enum mp_translation_source source,
                            uint64_t generation, uint64_t backend_epoch)
{
    bool current;
    mp_mutex_lock(&translation->state_lock);
    current = translation->source_generation[source] == generation &&
              translation->backend_epoch == backend_epoch;
    mp_mutex_unlock(&translation->state_lock);
    return current;
}

static bool backend_is_current(struct mp_translation *translation,
                               uint64_t backend_epoch)
{
    bool current;
    mp_mutex_lock(&translation->state_lock);
    current = translation->backend_epoch == backend_epoch;
    mp_mutex_unlock(&translation->state_lock);
    return current;
}

static void *acquire_backend(struct mp_translation *translation,
                             uint64_t backend_epoch,
                             struct mp_translation_backend_ops *ops)
{
    void *backend = NULL;
    mp_mutex_lock(&translation->state_lock);
    if (translation->backend_epoch == backend_epoch &&
        translation->backend && translation->backend_ops.acquire)
    {
        *ops = translation->backend_ops;
        backend = ops->acquire(translation->backend);
    }
    mp_mutex_unlock(&translation->state_lock);
    return backend;
}

static void worker_sleep(struct mp_translation *translation, int timeout_ms)
{
    if (timeout_ms <= 0)
        return;
    int64_t until = mp_time_ns() + (int64_t)timeout_ms * 1000000;
    mp_mutex_lock(&translation->pending_lock);
    if (!atomic_load(&translation->terminate)) {
        mp_cond_timedwait_until(&translation->pending_cv,
                                &translation->pending_lock, until);
    }
    mp_mutex_unlock(&translation->pending_lock);
}

static void push_result(struct mp_translation *translation,
                        struct translation_result *result)
{
    if (result->counted_inflight) {
        mp_mutex_lock(&translation->pending_lock);
        if (translation->in_flight[result->source] > 0)
            translation->in_flight[result->source]--;
        mp_mutex_unlock(&translation->pending_lock);
    }

    if (!task_is_current(translation, result->source,
                         result->generation, result->backend_epoch))
    {
        mp_mutex_lock(&translation->pending_lock);
        if (translation->outstanding[result->source] > 0)
            translation->outstanding[result->source]--;
        mp_mutex_unlock(&translation->pending_lock);
        talloc_free(result);
        if (translation->wakeup)
            translation->wakeup(translation->wakeup_ctx);
        return;
    }

    mp_mutex_lock(&translation->result_lock);
    if (translation->num_results >= translation->result_capacity) {
        int capacity = translation->result_capacity
            ? translation->result_capacity * 2 : 16;
        capacity = MPMIN(capacity, TRANSLATION_RESULTS_MAX);
        if (capacity > translation->result_capacity) {
            translation->results = talloc_realloc(
                translation, translation->results,
                struct translation_result *, capacity);
            translation->result_capacity = capacity;
        }
    }
    mp_assert(translation->num_results < TRANSLATION_RESULTS_MAX);
    translation->results[translation->num_results++] = result;
    mp_mutex_unlock(&translation->result_lock);

    if (translation->wakeup)
        translation->wakeup(translation->wakeup_ctx);
}

static MP_THREAD_VOID translation_worker(void *arg)
{
    struct mp_translation *translation = arg;
    mp_thread_set_name("sub-translate");

    while (!atomic_load(&translation->terminate)) {
        struct translation_task *task = NULL;
        bool saw_future = false;
        bool removed_stale = false;

        mp_mutex_lock(&translation->pending_lock);
        while (translation->num_pending == 0 &&
               !atomic_load(&translation->terminate))
        {
            mp_cond_wait(&translation->pending_cv,
                         &translation->pending_lock);
        }
        if (atomic_load(&translation->terminate)) {
            mp_mutex_unlock(&translation->pending_lock);
            break;
        }

        struct mp_translation_limits limits;
        int64_t quiet_until[MP_TRANSLATION_SOURCE_COUNT];
        mp_mutex_lock(&translation->limits_lock);
        limits = translation->limits;
        memcpy(quiet_until, translation->quiet_until_ms,
               sizeof(quiet_until));
        mp_mutex_unlock(&translation->limits_lock);

        double playback_pts;
        uint64_t generations[MP_TRANSLATION_SOURCE_COUNT];
        uint64_t backend_epoch;
        mp_mutex_lock(&translation->state_lock);
        playback_pts = translation->playback_pts;
        memcpy(generations, translation->source_generation,
               sizeof(generations));
        backend_epoch = translation->backend_epoch;
        mp_mutex_unlock(&translation->state_lock);

        int64_t now = wall_ms();
        for (int n = 0; n < translation->num_pending; ) {
            struct translation_task *candidate = translation->pending[n];
            bool stale = candidate->generation !=
                         generations[candidate->source] ||
                         candidate->backend_epoch != backend_epoch;
            if (stale) {
                if (translation->outstanding[candidate->source] > 0)
                    translation->outstanding[candidate->source]--;
                removed_stale = true;
                talloc_free(candidate);
                MP_TARRAY_REMOVE_AT(translation->pending,
                                    translation->num_pending, n);
                continue;
            }

            if (limits.enabled && limits.seek_debounce_ms > 0 &&
                now < quiet_until[candidate->source])
            {
                saw_future = true;
                n++;
                continue;
            }

            bool horizon = limits.enabled && limits.horizon_sec > 0 &&
                           isfinite(playback_pts);
            if (horizon &&
                candidate->pts > playback_pts + limits.horizon_sec)
            {
                saw_future = true;
                n++;
                continue;
            }

            task = candidate;
            MP_TARRAY_REMOVE_AT(translation->pending,
                                translation->num_pending, n);
            translation->in_flight[task->source]++;
            break;
        }
        mp_mutex_unlock(&translation->pending_lock);
        if (removed_stale && translation->wakeup)
            translation->wakeup(translation->wakeup_ctx);

        if (!task) {
            if (saw_future) {
                mp_mutex_lock(&translation->limits_lock);
                translation->horizon_deferred++;
                mp_mutex_unlock(&translation->limits_lock);
            }
            worker_sleep(translation, TRANSLATION_DEFER_MS);
            continue;
        }

        if (!task_is_current(translation, task->source,
                             task->generation, task->backend_epoch))
        {
            mp_mutex_lock(&translation->pending_lock);
            if (translation->in_flight[task->source] > 0)
                translation->in_flight[task->source]--;
            if (translation->outstanding[task->source] > 0)
                translation->outstanding[task->source]--;
            mp_mutex_unlock(&translation->pending_lock);
            talloc_free(task);
            if (translation->wakeup)
                translation->wakeup(translation->wakeup_ctx);
            continue;
        }

        struct translation_result *result =
            talloc_zero(NULL, struct translation_result);
        result->source = task->source;
        result->generation = task->generation;
        result->backend_epoch = task->backend_epoch;
        result->cue_id = task->cue_id;
        result->revision = task->revision;
        result->seq = task->seq;
        result->pts = task->pts;
        result->duration = task->duration;
        result->text = talloc_strdup(result, task->text);
        result->counted_inflight = true;
        result->kind = MP_TRANSLATION_RESULT_FALLBACK_FAILURE;

        if (limits.enabled &&
            (task->flags & MP_TRANSLATION_FILTER_SHORT) &&
            limits.min_text_chars > 0 &&
            utf8_chars(task->text) < limits.min_text_chars)
        {
            mp_mutex_lock(&translation->limits_lock);
            translation->short_skipped++;
            mp_mutex_unlock(&translation->limits_lock);
            result->kind = MP_TRANSLATION_RESULT_FALLBACK_SHORT;
            talloc_free(task);
            push_result(translation, result);
            continue;
        }

        char *normalized = NULL;
        if (limits.enabled)
            normalized = normalize_cache_text(NULL, task->text);
        if (normalized) {
            mp_mutex_lock(&translation->limits_lock);
            struct translation_cache_entry *entry =
                cache_lookup(translation, normalized,
                             task->backend_epoch);
            bool reused = false;
            int64_t current_ms = wall_ms();
            if (entry && entry->translated &&
                limits.reuse_cache_window_ms > 0 &&
                current_ms - entry->inserted_ms <=
                    limits.reuse_cache_window_ms)
            {
                result->translated =
                    talloc_strdup(result, entry->translated);
                result->kind = MP_TRANSLATION_RESULT_REUSED;
                translation->cache_reused++;
                reused = true;
            }
            bool repeated = false;
            if (!reused &&
                (task->flags & MP_TRANSLATION_FILTER_REPEAT))
            {
                repeated = repeat_record_hit(
                    translation, normalized, task->backend_epoch);
                if (repeated)
                    translation->loop_skipped++;
            }
            mp_mutex_unlock(&translation->limits_lock);

            if (reused) {
                talloc_free(normalized);
                talloc_free(task);
                push_result(translation, result);
                continue;
            }
            if (repeated) {
                result->kind = MP_TRANSLATION_RESULT_FALLBACK_LOOP;
                talloc_free(normalized);
                talloc_free(task);
                push_result(translation, result);
                continue;
            }
        }

        bool reserved = true;
        enum mp_translation_result_kind reject_kind =
            MP_TRANSLATION_RESULT_FALLBACK_FAILURE;
        if (limits.enabled) {
            mp_mutex_lock(&translation->limits_lock);
            reserved = reserve_request(translation, &reject_kind);
            mp_mutex_unlock(&translation->limits_lock);
        }
        if (!reserved) {
            result->kind = reject_kind;
            talloc_free(normalized);
            talloc_free(task);
            push_result(translation, result);
            continue;
        }

        struct mp_translation_backend_ops ops = {0};
        void *backend = acquire_backend(
            translation, task->backend_epoch, &ops);
        struct wt_call_result call = {0};
        if (backend) {
            void *tmp = talloc_new(NULL);
            ops.call(backend, tmp, task->text, &call);
            if (call.translated)
                result->translated =
                    talloc_strdup(result, call.translated);
            if (call.error[0])
                result->error = talloc_strdup(result, call.error);
            result->rate_limited = call.rate_limited;
            talloc_free(tmp);
            ops.release(backend);
        } else {
            result->error = talloc_strdup(result, "no translator");
        }

        bool current_backend =
            backend_is_current(translation, task->backend_epoch);
        if (limits.enabled && current_backend) {
            mp_mutex_lock(&translation->limits_lock);
            if (!call.http_issued)
                rollback_request(translation);
            if (result->translated && normalized)
                cache_put(translation, normalized, result->translated,
                          task->backend_epoch);
            mp_mutex_unlock(&translation->limits_lock);
        }

        result->kind = result->translated
            ? MP_TRANSLATION_RESULT_TRANSLATED
            : MP_TRANSLATION_RESULT_FALLBACK_FAILURE;

        talloc_free(normalized);
        talloc_free(task);
        push_result(translation, result);
    }

    MP_THREAD_RETURN();
}

struct mp_translation *mp_translation_create(
    void *talloc_parent, struct mp_log *log,
    mp_translation_wakeup_fn wakeup, void *wakeup_ctx)
{
    struct mp_translation *translation =
        talloc_zero(talloc_parent, struct mp_translation);
    translation->log =
        log ? mp_log_new(translation, log, "translate") : NULL;
    translation->wakeup = wakeup;
    translation->wakeup_ctx = wakeup_ctx;
    translation->playback_pts = NAN;
    translation->backend_epoch = 1;
    for (int n = 0; n < MP_TRANSLATION_SOURCE_COUNT; n++)
        translation->source_generation[n] = 1;

    mp_mutex_init(&translation->state_lock);
    mp_mutex_init(&translation->config_lock);
    mp_mutex_init(&translation->pending_lock);
    mp_mutex_init(&translation->result_lock);
    mp_mutex_init(&translation->limits_lock);
    mp_cond_init(&translation->pending_cv);
    atomic_init(&translation->terminate, false);

    translation->pending = talloc_zero_array(
        translation, struct translation_task *, 16);
    translation->pending_capacity = 16;
    translation->results = talloc_zero_array(
        translation, struct translation_result *, 16);
    translation->result_capacity = 16;
    translation->next_seq = 1;

    translation->limits = (struct mp_translation_limits){.enabled = true};
    limits_apply_defaults(&translation->limits);

    for (int n = 0; n < TRANSLATION_WORKERS_DEFAULT; n++) {
        if (mp_thread_create(&translation->workers[n],
                             translation_worker, translation) == 0)
        {
            translation->worker_count++;
        } else if (translation->log) {
            mp_warn(translation->log,
                    "failed to start translation worker %d\n", n);
        }
    }

    if (translation->worker_count == 0) {
        mp_cond_destroy(&translation->pending_cv);
        mp_mutex_destroy(&translation->limits_lock);
        mp_mutex_destroy(&translation->result_lock);
        mp_mutex_destroy(&translation->pending_lock);
        mp_mutex_destroy(&translation->config_lock);
        mp_mutex_destroy(&translation->state_lock);
        talloc_free(translation);
        return NULL;
    }

    if (translation->log) {
        mp_info(translation->log, "started %d translation worker(s)\n",
                translation->worker_count);
    }
    return translation;
}

static void clear_queues(struct mp_translation *translation, int source)
{
    mp_mutex_lock(&translation->pending_lock);
    for (int n = 0; n < translation->num_pending; ) {
        if (source < 0 || translation->pending[n]->source == source) {
            enum mp_translation_source task_source =
                translation->pending[n]->source;
            talloc_free(translation->pending[n]);
            MP_TARRAY_REMOVE_AT(translation->pending,
                                translation->num_pending, n);
            if (translation->outstanding[task_source] > 0)
                translation->outstanding[task_source]--;
        } else {
            n++;
        }
    }
    mp_cond_broadcast(&translation->pending_cv);
    mp_mutex_unlock(&translation->pending_lock);

    mp_mutex_lock(&translation->result_lock);
    for (int n = 0; n < translation->num_results; ) {
        if (source < 0 || translation->results[n]->source == source) {
            enum mp_translation_source result_source =
                translation->results[n]->source;
            talloc_free(translation->results[n]);
            MP_TARRAY_REMOVE_AT(translation->results,
                                translation->num_results, n);
            mp_mutex_lock(&translation->pending_lock);
            if (translation->outstanding[result_source] > 0)
                translation->outstanding[result_source]--;
            mp_mutex_unlock(&translation->pending_lock);
        } else {
            n++;
        }
    }
    mp_mutex_unlock(&translation->result_lock);
}

void mp_translation_destroy(struct mp_translation **ptr)
{
    if (!ptr || !*ptr)
        return;
    struct mp_translation *translation = *ptr;
    *ptr = NULL;

    atomic_store(&translation->terminate, true);
    mp_mutex_lock(&translation->pending_lock);
    mp_cond_broadcast(&translation->pending_cv);
    mp_mutex_unlock(&translation->pending_lock);
    for (int n = 0; n < translation->worker_count; n++)
        mp_thread_join(translation->workers[n]);

    clear_queues(translation, -1);

    struct mp_translation_backend_ops ops = {0};
    void *backend = NULL;
    mp_mutex_lock(&translation->state_lock);
    ops = translation->backend_ops;
    backend = translation->backend;
    translation->backend = NULL;
    memset(&translation->backend_ops, 0,
           sizeof(translation->backend_ops));
    mp_mutex_unlock(&translation->state_lock);
    if (backend && ops.release)
        ops.release(backend);

    mp_cond_destroy(&translation->pending_cv);
    mp_mutex_destroy(&translation->limits_lock);
    mp_mutex_destroy(&translation->result_lock);
    mp_mutex_destroy(&translation->pending_lock);
    mp_mutex_destroy(&translation->config_lock);
    mp_mutex_destroy(&translation->state_lock);
    talloc_free(translation);
}

static bool replace_backend_locked(
    struct mp_translation *translation,
    const struct mp_translation_backend_ops *ops,
    void *backend,
    bool legacy_only)
{
    struct mp_translation_backend_ops old_ops = {0};
    void *old_backend = NULL;

    mp_mutex_lock(&translation->state_lock);
    if (legacy_only && translation->common_explicit) {
        mp_mutex_unlock(&translation->state_lock);
        if (backend && ops && ops->release)
            ops->release(backend);
        return false;
    }
    mp_mutex_unlock(&translation->state_lock);

    clear_queues(translation, -1);
    mp_mutex_lock(&translation->limits_lock);
    cache_clear_locked(translation);
    mp_mutex_unlock(&translation->limits_lock);

    mp_mutex_lock(&translation->state_lock);
    old_ops = translation->backend_ops;
    old_backend = translation->backend;
    translation->backend_ops = ops
        ? *ops : (struct mp_translation_backend_ops){0};
    translation->backend = backend;
    translation->backend_epoch++;
    for (int n = 0; n < MP_TRANSLATION_SOURCE_COUNT; n++)
        translation->source_generation[n]++;
    mp_mutex_unlock(&translation->state_lock);

    if (old_backend && old_ops.release)
        old_ops.release(old_backend);
    return true;
}

static bool replace_backend(
    struct mp_translation *translation,
    const struct mp_translation_backend_ops *ops,
    void *backend,
    bool legacy_only)
{
    mp_mutex_lock(&translation->config_lock);
    bool replaced = replace_backend_locked(
        translation, ops, backend, legacy_only);
    mp_mutex_unlock(&translation->config_lock);
    return replaced;
}

void mp_translation_set_backend(
    struct mp_translation *translation,
    const struct mp_translation_backend_ops *ops,
    void *backend)
{
    if (!translation)
        return;
    if (backend && (!ops || !ops->acquire || !ops->release || !ops->call)) {
        if (ops && ops->release)
            ops->release(backend);
        return;
    }
    replace_backend(translation, ops, backend, false);
}

bool mp_translation_has_backend(struct mp_translation *translation)
{
    if (!translation)
        return false;
    bool available;
    mp_mutex_lock(&translation->state_lock);
    available = translation->backend != NULL;
    mp_mutex_unlock(&translation->state_lock);
    return available;
}

void mp_translation_register_source(
    struct mp_translation *translation, enum mp_translation_source source)
{
    if (!translation || !valid_source(source))
        return;
    mp_mutex_lock(&translation->state_lock);
    translation->source_active[source] = true;
    mp_mutex_unlock(&translation->state_lock);
}

void mp_translation_unregister_source(
    struct mp_translation *translation, enum mp_translation_source source)
{
    if (!translation || !valid_source(source))
        return;
    clear_queues(translation, source);
    mp_mutex_lock(&translation->state_lock);
    translation->source_active[source] = false;
    translation->source_generation[source]++;
    mp_mutex_unlock(&translation->state_lock);
}

uint64_t mp_translation_invalidate_source(
    struct mp_translation *translation, enum mp_translation_source source)
{
    if (!translation || !valid_source(source))
        return 0;
    clear_queues(translation, source);
    mp_mutex_lock(&translation->limits_lock);
    if (translation->limits.enabled &&
        translation->limits.seek_debounce_ms > 0)
    {
        int64_t until = wall_ms() +
            translation->limits.seek_debounce_ms;
        if (until > translation->quiet_until_ms[source])
            translation->quiet_until_ms[source] = until;
    }
    mp_mutex_unlock(&translation->limits_lock);

    uint64_t generation;
    mp_mutex_lock(&translation->state_lock);
    generation = ++translation->source_generation[source];
    mp_mutex_unlock(&translation->state_lock);
    return generation;
}

void mp_translation_invalidate_all(struct mp_translation *translation)
{
    if (!translation)
        return;
    clear_queues(translation, -1);
    mp_mutex_lock(&translation->state_lock);
    for (int n = 0; n < MP_TRANSLATION_SOURCE_COUNT; n++)
        translation->source_generation[n]++;
    mp_mutex_unlock(&translation->state_lock);
}

void mp_translation_set_playback_pts(
    struct mp_translation *translation, double pts)
{
    if (!translation)
        return;
    mp_mutex_lock(&translation->state_lock);
    translation->playback_pts = pts;
    mp_mutex_unlock(&translation->state_lock);
    mp_mutex_lock(&translation->pending_lock);
    mp_cond_broadcast(&translation->pending_cv);
    mp_mutex_unlock(&translation->pending_lock);
}

enum mp_translation_submit_result mp_translation_submit(
    struct mp_translation *translation,
    enum mp_translation_source source,
    uint64_t cue_id,
    uint64_t revision,
    const char *text,
    double pts,
    double duration,
    unsigned flags)
{
    if (!translation || !valid_source(source) || !text || !text[0])
        return MP_TRANSLATION_SUBMIT_INACTIVE;

    uint64_t generation;
    uint64_t backend_epoch;
    double playback_pts;
    bool active;
    bool backend;
    mp_mutex_lock(&translation->state_lock);
    generation = translation->source_generation[source];
    backend_epoch = translation->backend_epoch;
    playback_pts = translation->playback_pts;
    active = translation->source_active[source];
    backend = translation->backend != NULL;
    mp_mutex_unlock(&translation->state_lock);
    if (!active)
        return MP_TRANSLATION_SUBMIT_INACTIVE;
    if (!backend)
        return MP_TRANSLATION_SUBMIT_NO_BACKEND;
    if (isfinite(playback_pts) && isfinite(pts) &&
        isfinite(duration) && duration >= 0 &&
        pts + duration - playback_pts < TRANSLATION_MIN_SLACK)
    {
        return MP_TRANSLATION_SUBMIT_TOO_LATE;
    }

    struct translation_task *task =
        talloc_zero(NULL, struct translation_task);
    *task = (struct translation_task){
        .source = source,
        .generation = generation,
        .backend_epoch = backend_epoch,
        .cue_id = cue_id,
        .revision = revision,
        .pts = pts,
        .duration = duration,
        .flags = flags,
        .text = talloc_strdup(task, text),
    };

    mp_mutex_lock(&translation->pending_lock);
    int outstanding = 0;
    for (int n = 0; n < MP_TRANSLATION_SOURCE_COUNT; n++)
        outstanding += translation->outstanding[n];
    if (outstanding >= TRANSLATION_OUTSTANDING_MAX) {
        mp_mutex_unlock(&translation->pending_lock);
        talloc_free(task);
        mp_mutex_lock(&translation->limits_lock);
        translation->queue_overflow++;
        mp_mutex_unlock(&translation->limits_lock);
        return MP_TRANSLATION_SUBMIT_BACKPRESSURE;
    }
    translation->outstanding[source]++;
    task->seq = translation->next_seq++;
    if (translation->num_pending >= translation->pending_capacity) {
        int capacity = translation->pending_capacity
            ? translation->pending_capacity * 2 : 16;
        capacity = MPMIN(capacity, TRANSLATION_PENDING_MAX);
        if (capacity > translation->pending_capacity) {
            translation->pending = talloc_realloc(
                translation, translation->pending,
                struct translation_task *, capacity);
            translation->pending_capacity = capacity;
        }
    }
    mp_assert(translation->num_pending < TRANSLATION_PENDING_MAX);
    translation->pending[translation->num_pending++] = task;
    mp_cond_signal(&translation->pending_cv);
    mp_mutex_unlock(&translation->pending_lock);

    return MP_TRANSLATION_SUBMIT_QUEUED;
}

void mp_translation_drain(
    struct mp_translation *translation,
    enum mp_translation_source source,
    mp_translation_result_fn callback,
    void *callback_ctx)
{
    if (!translation || !valid_source(source) || !callback)
        return;

    struct translation_result **batch = NULL;
    int count = 0;
    mp_mutex_lock(&translation->result_lock);
    for (int n = 0; n < translation->num_results; ) {
        struct translation_result *result = translation->results[n];
        if (result->source == source) {
            MP_TARRAY_APPEND(NULL, batch, count, result);
            MP_TARRAY_REMOVE_AT(translation->results,
                                translation->num_results, n);
        } else {
            n++;
        }
    }
    mp_mutex_unlock(&translation->result_lock);
    if (!count)
        return;

    for (int n = 1; n < count; n++) {
        struct translation_result *current = batch[n];
        int insert = n - 1;
        while (insert >= 0 && batch[insert]->seq > current->seq) {
            batch[insert + 1] = batch[insert];
            insert--;
        }
        batch[insert + 1] = current;
    }

    uint64_t generation;
    uint64_t backend_epoch;
    double playback_pts;
    mp_mutex_lock(&translation->state_lock);
    generation = translation->source_generation[source];
    backend_epoch = translation->backend_epoch;
    playback_pts = translation->playback_pts;
    mp_mutex_unlock(&translation->state_lock);

    for (int n = 0; n < count; n++) {
        struct translation_result *result = batch[n];
        if (result->generation != generation ||
            result->backend_epoch != backend_epoch)
        {
            mp_mutex_lock(&translation->pending_lock);
            if (translation->outstanding[source] > 0)
                translation->outstanding[source]--;
            mp_mutex_unlock(&translation->pending_lock);
            talloc_free(result);
            continue;
        }
        if (isfinite(playback_pts) && isfinite(result->pts) &&
            isfinite(result->duration) && result->duration >= 0 &&
            result->pts + result->duration +
                TRANSLATION_DRAIN_EPSILON < playback_pts)
        {
            talloc_free(result->translated);
            result->translated = NULL;
            talloc_free(result->error);
            result->error =
                talloc_strdup(result, "translation arrived after cue end");
            result->kind = MP_TRANSLATION_RESULT_FALLBACK_LATE;
        }
        struct mp_translation_result public_result = {
            .source = result->source,
            .generation = result->generation,
            .cue_id = result->cue_id,
            .revision = result->revision,
            .seq = result->seq,
            .pts = result->pts,
            .duration = result->duration,
            .text = result->text,
            .translated = result->translated,
            .rate_limited = result->rate_limited,
            .error = result->error,
            .kind = result->kind,
        };
        callback(callback_ctx, &public_result);
        mp_mutex_lock(&translation->pending_lock);
        if (translation->outstanding[source] > 0)
            translation->outstanding[source]--;
        mp_mutex_unlock(&translation->pending_lock);
        talloc_free(result);
    }
    talloc_free(batch);
}

int mp_translation_pending_count(
    struct mp_translation *translation, enum mp_translation_source source)
{
    if (!translation || !valid_source(source))
        return 0;
    int count;
    mp_mutex_lock(&translation->pending_lock);
    count = translation->outstanding[source];
    mp_mutex_unlock(&translation->pending_lock);
    return count;
}

void mp_translation_get_limits(
    struct mp_translation *translation, struct mp_translation_limits *out)
{
    if (!out)
        return;
    *out = (struct mp_translation_limits){0};
    if (!translation)
        return;
    mp_mutex_lock(&translation->limits_lock);
    *out = translation->limits;
    mp_mutex_unlock(&translation->limits_lock);
}

void mp_translation_set_limits(
    struct mp_translation *translation,
    const struct mp_translation_limits *limits)
{
    if (!translation || !limits)
        return;
    struct mp_translation_limits next = *limits;
    limits_apply_defaults(&next);
    mp_mutex_lock(&translation->limits_lock);
    translation->limits = next;
    if (translation->rpm_tokens > next.rpm_limit)
        translation->rpm_tokens = next.rpm_limit;
    cache_clamp_locked(translation);
    mp_mutex_unlock(&translation->limits_lock);
    mp_mutex_lock(&translation->pending_lock);
    mp_cond_broadcast(&translation->pending_cv);
    mp_mutex_unlock(&translation->pending_lock);
}

static bool node_is_integer(struct mpv_node *node, bool strict,
                            int64_t *value)
{
    if (node->format == MPV_FORMAT_INT64) {
        *value = node->u.int64;
        return true;
    }
    if (node->format == MPV_FORMAT_DOUBLE &&
        isfinite(node->u.double_) &&
        node->u.double_ >= (double)INT64_MIN &&
        node->u.double_ <= (double)INT64_MAX &&
        (!strict || floor(node->u.double_) == node->u.double_))
    {
        *value = node->u.double_;
        return true;
    }
    return false;
}

static int apply_limits_map(struct mp_translation_limits *limits,
                            struct mpv_node *map, bool strict, char **error)
{
    if (!map || map->format != MPV_FORMAT_NODE_MAP) {
        set_error(error, "limits must be an object");
        return -1;
    }
    for (int n = 0; n < map->u.list->num; n++) {
        const char *key = map->u.list->keys[n];
        struct mpv_node *value = &map->u.list->values[n];
        int64_t integer = 0;
        bool known = true;
        if (strcmp(key, "enabled") == 0) {
            if (value->format != MPV_FORMAT_FLAG) {
                set_error(error, "limits.enabled must be a boolean");
                return -1;
            }
            limits->enabled = value->u.flag;
            continue;
        } else if (!node_is_integer(value, strict, &integer)) {
            if (strict) {
                set_error(error, "translation limit values must be integers");
                return -1;
            }
            continue;
        } else if (integer < INT_MIN || integer > INT_MAX ||
                   (strict && integer < 0))
        {
            set_error(error, "translation limit value is out of range");
            return -1;
        }

        if (strcmp(key, "horizon_sec") == 0)
            limits->horizon_sec = integer;
        else if (strcmp(key, "seek_debounce_ms") == 0)
            limits->seek_debounce_ms = integer;
        else if (strcmp(key, "min_text_chars") == 0)
            limits->min_text_chars = integer;
        else if (strcmp(key, "reuse_cache_capacity") == 0)
            limits->reuse_cache_capacity = integer;
        else if (strcmp(key, "reuse_cache_window_ms") == 0)
            limits->reuse_cache_window_ms = integer;
        else if (strcmp(key, "repeat_loop_threshold") == 0)
            limits->repeat_loop_threshold = integer;
        else if (strcmp(key, "repeat_loop_window_ms") == 0)
            limits->repeat_loop_window_ms = integer;
        else if (strcmp(key, "rpm_limit") == 0)
            limits->rpm_limit = integer;
        else if (strcmp(key, "session_request_limit") == 0)
            limits->session_request_limit = integer;
        else
            known = false;

        if (strict && !known) {
            set_error(error, "limits contains an unknown field");
            return -1;
        }
    }
    limits_apply_defaults(limits);
    return 0;
}

int mp_translation_update_limits_json(
    struct mp_translation *translation, const char *json, char **error)
{
    if (error)
        *error = NULL;
    if (!translation || !json || !json[0]) {
        set_error(error, "limits JSON is empty");
        return -1;
    }
    void *tmp = talloc_new(NULL);
    char *cursor = talloc_strdup(tmp, json);
    struct mpv_node root = {0};
    if (json_parse(tmp, &root, &cursor, MAX_JSON_DEPTH) < 0 ||
        root.format != MPV_FORMAT_NODE_MAP)
    {
        set_error(error, "limits JSON must be an object");
        talloc_free(tmp);
        return -1;
    }
    struct mp_translation_limits next;
    mp_translation_get_limits(translation, &next);
    int result = apply_limits_map(&next, &root, false, error);
    if (result == 0)
        mp_translation_set_limits(translation, &next);
    talloc_free(tmp);
    return result;
}

static bool known_key(const char *key, const char *const *keys)
{
    for (int n = 0; keys[n]; n++) {
        if (strcmp(key, keys[n]) == 0)
            return true;
    }
    return false;
}

static char *optional_string(void *parent, struct mpv_node *map,
                             const char *key, char **error)
{
    struct mpv_node *value = node_map_get(map, key);
    if (!value)
        return NULL;
    if (value->format != MPV_FORMAT_STRING) {
        set_error(error, "translation language and AI fields must be strings");
        return (char *)-1;
    }
    return talloc_strdup(parent, value->u.string);
}

static bool optional_integer(struct mpv_node *map, const char *key,
                             int fallback, int *out, char **error)
{
    struct mpv_node *value = node_map_get(map, key);
    if (!value) {
        *out = fallback;
        return true;
    }
    int64_t integer;
    if (!node_is_integer(value, true, &integer) ||
        integer < INT_MIN || integer > INT_MAX)
    {
        set_error(error, "AI integer field is invalid");
        return false;
    }
    *out = integer;
    return true;
}

static int parse_common_config(void *parent, const char *json,
                               struct parsed_common_config *out,
                               char **error)
{
    memset(out, 0, sizeof(*out));
    out->limits = (struct mp_translation_limits){.enabled = true};
    limits_apply_defaults(&out->limits);

    if (!json_validate_strict(json, MAX_JSON_DEPTH)) {
        set_error(error, "translation config must use strict JSON syntax");
        return -1;
    }

    char *cursor = talloc_strdup(parent, json);
    struct mpv_node root = {0};
    if (json_parse(parent, &root, &cursor, MAX_JSON_DEPTH) < 0 ||
        root.format != MPV_FORMAT_NODE_MAP)
    {
        set_error(error, "translation config must be a JSON object");
        return -1;
    }
    json_skip_whitespace(&cursor);
    if (cursor[0]) {
        set_error(error, "translation config contains trailing data");
        return -1;
    }

    static const char *const top_keys[] = {
        "provider", "source_lang", "target_lang", "ai", "limits", NULL,
    };
    for (int n = 0; n < root.u.list->num; n++) {
        if (!known_key(root.u.list->keys[n], top_keys)) {
            set_error(error, "translation config contains an unknown field");
            return -1;
        }
    }

    struct mpv_node *provider = node_map_get(&root, "provider");
    if (!provider || provider->format != MPV_FORMAT_STRING) {
        set_error(error, "provider must be google, azure, or ai");
        return -1;
    }
    if (strcmp(provider->u.string, "google") == 0) {
        out->provider = WT_PROVIDER_GOOGLE;
    } else if (strcmp(provider->u.string, "azure") == 0) {
        out->provider = WT_PROVIDER_AZURE;
    } else if (strcmp(provider->u.string, "ai") == 0) {
        out->provider = WT_PROVIDER_OPENAI;
    } else {
        set_error(error, "provider must be google, azure, or ai");
        return -1;
    }

    out->source_lang = optional_string(parent, &root, "source_lang", error);
    if (out->source_lang == (char *)-1)
        return -1;
    if (!out->source_lang || !out->source_lang[0])
        out->source_lang = talloc_strdup(parent, "auto");
    out->target_lang = optional_string(parent, &root, "target_lang", error);
    if (out->target_lang == (char *)-1)
        return -1;
    if (!out->target_lang || !out->target_lang[0]) {
        set_error(error, "target_lang is required");
        return -1;
    }

    struct mpv_node *limits = node_map_get(&root, "limits");
    if (limits && apply_limits_map(&out->limits, limits, true, error) < 0)
        return -1;

    struct mpv_node *ai = node_map_get(&root, "ai");
    if (out->provider != WT_PROVIDER_OPENAI) {
        if (ai && ai->format != MPV_FORMAT_NONE) {
            set_error(error, "ai must be null for google or azure");
            return -1;
        }
        return 0;
    }

    if (!ai || ai->format != MPV_FORMAT_NODE_MAP) {
        set_error(error, "ai provider requires an ai object");
        return -1;
    }
    static const char *const ai_keys[] = {
        "endpoint", "model", "api_key", "source_lang", "target_lang",
        "system_prompt", "context_size", "max_tokens", "timeout_ms", NULL,
    };
    for (int n = 0; n < ai->u.list->num; n++) {
        if (!known_key(ai->u.list->keys[n], ai_keys)) {
            set_error(error, "ai contains an unknown field");
            return -1;
        }
    }

    out->ai.endpoint = optional_string(parent, ai, "endpoint", error);
    if (out->ai.endpoint == (char *)-1)
        return -1;
    out->ai.model = optional_string(parent, ai, "model", error);
    if (out->ai.model == (char *)-1)
        return -1;
    out->ai.api_key = optional_string(parent, ai, "api_key", error);
    if (out->ai.api_key == (char *)-1)
        return -1;
    out->ai.source_lang = optional_string(parent, ai, "source_lang", error);
    if (out->ai.source_lang == (char *)-1)
        return -1;
    out->ai.target_lang = optional_string(parent, ai, "target_lang", error);
    if (out->ai.target_lang == (char *)-1)
        return -1;
    out->ai.system_prompt =
        optional_string(parent, ai, "system_prompt", error);
    if (out->ai.system_prompt == (char *)-1)
        return -1;

    if (!out->ai.endpoint || !out->ai.endpoint[0] ||
        !out->ai.model || !out->ai.model[0])
    {
        set_error(error, "ai.endpoint and ai.model are required");
        return -1;
    }
    if (!out->ai.source_lang || !out->ai.source_lang[0])
        out->ai.source_lang = out->source_lang;
    if (!out->ai.target_lang || !out->ai.target_lang[0])
        out->ai.target_lang = out->target_lang;

    if (!optional_integer(ai, "context_size", 0,
                          &out->ai.context_size, error) ||
        !optional_integer(ai, "max_tokens", -1,
                          &out->ai.max_tokens, error) ||
        !optional_integer(ai, "timeout_ms", 0,
                          &out->ai.timeout_ms, error))
    {
        return -1;
    }
    return 0;
}

bool mp_translation_validate_common_config(const char *json, char **error)
{
    if (error)
        *error = NULL;
    if (!json || !json[0])
        return true;
    void *tmp = talloc_new(NULL);
    struct parsed_common_config config;
    bool valid = parse_common_config(tmp, json, &config, error) == 0;
    talloc_free(tmp);
    return valid;
}

char *mp_translation_mask_config(void *talloc_parent, const char *json)
{
    if (!json || !json[0])
        return talloc_strdup(talloc_parent, "");
    void *tmp = talloc_new(NULL);
    char *cursor = talloc_strdup(tmp, json);
    struct mpv_node root = {0};
    if (json_parse(tmp, &root, &cursor, MAX_JSON_DEPTH) < 0 ||
        root.format != MPV_FORMAT_NODE_MAP)
    {
        talloc_free(tmp);
        return talloc_strdup(talloc_parent, "");
    }
    struct mpv_node *ai = node_map_get(&root, "ai");
    struct mpv_node *scope = ai && ai->format == MPV_FORMAT_NODE_MAP
        ? ai : &root;
    struct mpv_node *api_key = node_map_get(scope, "api_key");
    if (api_key && api_key->format == MPV_FORMAT_STRING &&
        api_key->u.string[0])
    {
        api_key->u.string = talloc_strdup(tmp, "***");
    }
    char *serialized = NULL;
    char *result = NULL;
    if (json_write(&serialized, &root) >= 0 && serialized)
        result = talloc_strdup(talloc_parent, serialized);
    talloc_free(serialized);
    talloc_free(tmp);
    return result ? result : talloc_strdup(talloc_parent, "");
}

static void *native_backend_acquire(void *ctx)
{
    return whisper_translator_acquire(ctx);
}

static void native_backend_release(void *ctx)
{
    struct whisper_translator *translator = ctx;
    whisper_translator_release(&translator);
}

static void native_backend_call(void *ctx, void *talloc_ctx,
                                const char *text,
                                struct wt_call_result *out)
{
    whisper_translate_call(ctx, talloc_ctx, text, out);
}

static void native_backend_status(void *ctx, struct wt_status *out)
{
    whisper_translator_get_status(ctx, out);
}

static const struct mp_translation_backend_ops native_backend_ops = {
    .acquire = native_backend_acquire,
    .release = native_backend_release,
    .call = native_backend_call,
    .get_status = native_backend_status,
};

static struct whisper_translator *create_native_backend(
    struct mp_translation *translation,
    const struct parsed_common_config *config)
{
    if (config->provider == WT_PROVIDER_OPENAI) {
        return whisper_translator_create_openai(
            translation, translation->log, &config->ai);
    }
    return whisper_translator_create(
        translation, translation->log, config->provider,
        config->source_lang, config->target_lang);
}

int mp_translation_set_common_config(
    struct mp_translation *translation, const char *json, char **error)
{
    if (error)
        *error = NULL;
    if (!translation)
        return -1;

    struct whisper_translator *backend = NULL;
    struct mp_translation_limits limits =
        (struct mp_translation_limits){.enabled = true};
    limits_apply_defaults(&limits);
    bool enabled = json && json[0];
    if (enabled) {
        void *tmp = talloc_new(NULL);
        struct parsed_common_config config;
        if (parse_common_config(tmp, json, &config, error) < 0) {
            talloc_free(tmp);
            return -1;
        }
        backend = create_native_backend(translation, &config);
        if (!backend) {
            set_error(error, "translation provider initialization failed");
            talloc_free(tmp);
            return -1;
        }
        limits = config.limits;
        talloc_free(tmp);
    }

    mp_mutex_lock(&translation->config_lock);
    mp_mutex_lock(&translation->state_lock);
    translation->common_explicit = true;
    translation->common_enabled = enabled;
    talloc_free(translation->common_json);
    translation->common_json =
        enabled ? talloc_strdup(translation, json) : NULL;
    mp_mutex_unlock(&translation->state_lock);

    mp_translation_set_limits(translation, &limits);
    replace_backend_locked(
        translation, backend ? &native_backend_ops : NULL,
        backend, false);
    mp_mutex_lock(&translation->limits_lock);
    reset_session_locked(translation);
    mp_mutex_unlock(&translation->limits_lock);
    mp_mutex_unlock(&translation->config_lock);
    return 0;
}

bool mp_translation_common_config_explicit(
    struct mp_translation *translation)
{
    if (!translation)
        return false;
    bool explicit;
    mp_mutex_lock(&translation->state_lock);
    explicit = translation->common_explicit;
    mp_mutex_unlock(&translation->state_lock);
    return explicit;
}

bool mp_translation_common_config_enabled(
    struct mp_translation *translation)
{
    if (!translation)
        return false;
    bool enabled;
    mp_mutex_lock(&translation->state_lock);
    enabled = translation->common_explicit &&
              translation->common_enabled &&
              translation->backend;
    mp_mutex_unlock(&translation->state_lock);
    return enabled;
}

char *mp_translation_get_common_config(
    struct mp_translation *translation, void *talloc_parent)
{
    if (!translation)
        return talloc_strdup(talloc_parent, "");
    char *json;
    mp_mutex_lock(&translation->state_lock);
    json = talloc_strdup(NULL, translation->common_json
        ? translation->common_json : "");
    mp_mutex_unlock(&translation->state_lock);
    char *masked = mp_translation_mask_config(talloc_parent, json);
    talloc_free(json);
    return masked;
}

static bool parse_legacy_ai(void *parent, const char *json,
                            struct wt_openai_config *config)
{
    memset(config, 0, sizeof(*config));
    if (!json || !json[0])
        return false;
    char *cursor = talloc_strdup(parent, json);
    struct mpv_node root = {0};
    if (json_parse(parent, &root, &cursor, MAX_JSON_DEPTH) < 0 ||
        root.format != MPV_FORMAT_NODE_MAP)
    {
        return false;
    }
    struct mpv_node *node;
#define LEGACY_STRING(key) \
    ((node = node_map_get(&root, key)) && \
     node->format == MPV_FORMAT_STRING \
        ? talloc_strdup(parent, node->u.string) : NULL)
#define LEGACY_INTEGER(key, fallback) \
    ((node = node_map_get(&root, key)) && \
     node->format == MPV_FORMAT_INT64 \
        ? (int)node->u.int64 : (fallback))
    config->endpoint = LEGACY_STRING("endpoint");
    config->model = LEGACY_STRING("model");
    config->api_key = LEGACY_STRING("api_key");
    config->source_lang = LEGACY_STRING("source_lang");
    config->target_lang = LEGACY_STRING("target_lang");
    config->system_prompt = LEGACY_STRING("system_prompt");
    config->context_size = LEGACY_INTEGER("context_size", 0);
    config->timeout_ms = LEGACY_INTEGER("timeout_ms", 0);
    config->max_tokens = LEGACY_INTEGER("max_tokens", -1);
#undef LEGACY_STRING
#undef LEGACY_INTEGER
    return config->endpoint && config->model && config->target_lang;
}

int mp_translation_configure_legacy_whisper(
    struct mp_translation *translation,
    enum wt_provider provider,
    const char *source_lang,
    const char *target_lang,
    const char *ai_json,
    const char *limits_json)
{
    if (!translation)
        return -1;
    mp_translation_register_source(
        translation, MP_TRANSLATION_SOURCE_WHISPER);
    mp_mutex_lock(&translation->config_lock);
    if (mp_translation_common_config_explicit(translation)) {
        mp_mutex_unlock(&translation->config_lock);
        return 0;
    }

    struct mp_translation_limits defaults = {.enabled = true};
    limits_apply_defaults(&defaults);
    mp_translation_set_limits(translation, &defaults);
    mp_mutex_lock(&translation->limits_lock);
    reset_session_locked(translation);
    mp_mutex_unlock(&translation->limits_lock);

    struct whisper_translator *backend = NULL;
    if (ai_json && ai_json[0]) {
        void *tmp = talloc_new(NULL);
        struct wt_openai_config config;
        if (parse_legacy_ai(tmp, ai_json, &config)) {
            backend = whisper_translator_create_openai(
                translation, translation->log, &config);
        }
        talloc_free(tmp);
    }
    if (!backend && target_lang && target_lang[0] &&
        (provider == WT_PROVIDER_GOOGLE ||
         provider == WT_PROVIDER_AZURE))
    {
        backend = whisper_translator_create(
            translation, translation->log, provider,
            source_lang && source_lang[0] ? source_lang : "auto",
            target_lang);
    }
    bool applied = replace_backend_locked(
        translation, backend ? &native_backend_ops : NULL,
        backend, true);

    if (applied && limits_json && limits_json[0]) {
        char *error = NULL;
        int result = mp_translation_update_limits_json(
            translation, limits_json, &error);
        if (result < 0 && translation->log)
            mp_warn(translation->log, "invalid legacy limits: %s\n",
                    error ? error : "unknown error");
        talloc_free(error);
    }
    mp_mutex_unlock(&translation->config_lock);
    return 0;
}

bool mp_translation_set_legacy_ai(
    struct mp_translation *translation, const char *json)
{
    if (!translation ||
        mp_translation_common_config_explicit(translation))
    {
        return false;
    }

    bool active;
    mp_mutex_lock(&translation->state_lock);
    active =
        translation->source_active[MP_TRANSLATION_SOURCE_WHISPER];
    mp_mutex_unlock(&translation->state_lock);
    if (!active)
        return false;

    struct whisper_translator *backend = NULL;
    if (json && json[0]) {
        void *tmp = talloc_new(NULL);
        struct wt_openai_config config;
        if (parse_legacy_ai(tmp, json, &config)) {
            backend = whisper_translator_create_openai(
                translation, translation->log, &config);
        } else if (translation->log) {
            mp_warn(translation->log,
                    "invalid whisper-ai-translate config; disabled\n");
        }
        talloc_free(tmp);
    }
    return replace_backend(
        translation, backend ? &native_backend_ops : NULL,
        backend, true);
}

int mp_translation_set_legacy_limits(
    struct mp_translation *translation, const char *json)
{
    if (!translation || !json || !json[0])
        return -1;
    mp_mutex_lock(&translation->config_lock);
    if (mp_translation_common_config_explicit(translation)) {
        mp_mutex_unlock(&translation->config_lock);
        return 0;
    }
    char *error = NULL;
    int result = mp_translation_update_limits_json(
        translation, json, &error);
    if (result < 0 && translation->log)
        mp_warn(translation->log, "invalid legacy limits: %s\n",
                error ? error : "unknown error");
    talloc_free(error);
    mp_mutex_unlock(&translation->config_lock);
    return result;
}

void mp_translation_stop_legacy_whisper(
    struct mp_translation *translation)
{
    if (!translation)
        return;
    mp_translation_unregister_source(
        translation, MP_TRANSLATION_SOURCE_WHISPER);
    if (!mp_translation_common_config_explicit(translation))
        replace_backend(translation, NULL, NULL, true);
}

char *mp_translation_get_legacy_status(
    struct mp_translation *translation, void *talloc_parent)
{
    if (!translation)
        return NULL;

    bool active;
    struct mp_translation_backend_ops ops = {0};
    void *backend = NULL;
    mp_mutex_lock(&translation->state_lock);
    active =
        translation->source_active[MP_TRANSLATION_SOURCE_WHISPER];
    if (active && translation->backend && translation->backend_ops.acquire) {
        ops = translation->backend_ops;
        backend = ops.acquire(translation->backend);
    }
    mp_mutex_unlock(&translation->state_lock);
    if (!backend)
        return NULL;

    struct wt_status status = {0};
    if (ops.get_status)
        ops.get_status(backend, &status);
    ops.release(backend);

    struct mp_translation_limits limits;
    int session_requests;
    int horizon_deferred;
    int cache_reused_count;
    int loop_skipped_count;
    int short_skipped_count;
    int rpm_skipped_count;
    int budget_skipped_count;
    int queue_overflow_count;
    int cache_size;
    double rpm_tokens;
    mp_mutex_lock(&translation->limits_lock);
    limits = translation->limits;
    session_requests = translation->session_requests;
    horizon_deferred = translation->horizon_deferred;
    cache_reused_count = translation->cache_reused;
    loop_skipped_count = translation->loop_skipped;
    short_skipped_count = translation->short_skipped;
    rpm_skipped_count = translation->rpm_skipped;
    budget_skipped_count = translation->budget_skipped;
    queue_overflow_count = translation->queue_overflow;
    cache_size = translation->cache_size;
    rpm_tokens = translation->rpm_tokens;
    mp_mutex_unlock(&translation->limits_lock);

    void *tmp = talloc_new(NULL);
    struct mpv_node root = {0};
    node_init(&root, MPV_FORMAT_NODE_MAP, NULL);
    talloc_steal(tmp, root.u.list);
    node_map_add_flag(&root, "enabled", status.enabled);
    node_map_add_flag(&root, "paused", status.paused);
    node_map_add_int64(&root, "fail_count", status.fail_count);
    node_map_add_int64(&root, "retry_after_ms", status.retry_after_ms);
    node_map_add_string(&root, "last_error", status.last_error);
    node_map_add_int64(&root, "session_req_used", session_requests);
    node_map_add_int64(&root, "session_request_limit",
                       limits.session_request_limit);
    node_map_add_int64(&root, "rpm_limit", limits.rpm_limit);
    node_map_add_int64(&root, "rpm_tokens",
                       (int64_t)(rpm_tokens + 0.5));
    node_map_add_int64(&root, "horizon_skipped", horizon_deferred);
    node_map_add_int64(&root, "cache_reused", cache_reused_count);
    node_map_add_int64(&root, "loop_skipped", loop_skipped_count);
    node_map_add_int64(&root, "short_skipped", short_skipped_count);
    node_map_add_int64(&root, "rpm_skipped", rpm_skipped_count);
    node_map_add_int64(&root, "budget_skipped", budget_skipped_count);
    node_map_add_int64(&root, "far_future_dropped", 0);
    node_map_add_int64(&root, "queue_overflow", queue_overflow_count);
    node_map_add_int64(&root, "horizon_sec", limits.horizon_sec);
    node_map_add_int64(&root, "seek_debounce_ms",
                       limits.seek_debounce_ms);
    node_map_add_int64(&root, "reuse_cache_capacity",
                       limits.reuse_cache_capacity);
    node_map_add_int64(&root, "reuse_cache_size", cache_size);
    bool budget_exhausted =
        limits.session_request_limit > 0 &&
        session_requests >= limits.session_request_limit;
    node_map_add_string(&root, "pause_reason",
                        budget_exhausted ? "budget_exhausted" : "");

    char *serialized = NULL;
    char *result = NULL;
    if (json_write(&serialized, &root) >= 0 && serialized)
        result = talloc_strdup(talloc_parent, serialized);
    talloc_free(serialized);
    talloc_free(tmp);
    return result;
}

static void wake_mpctx(void *ctx)
{
    mp_wakeup_core(ctx);
}

struct mp_translation *mpctx_get_translation(struct MPContext *mpctx)
{
    if (!mpctx)
        return NULL;
    mp_mutex_lock(&mpctx->translation_lock);
    if (!mpctx->translation && !mpctx->translation_init_attempted) {
        mpctx->translation_init_attempted = true;
        mpctx->translation = mp_translation_create(
            mpctx, mpctx->log, wake_mpctx, mpctx);
    }
    struct mp_translation *translation = mpctx->translation;
    mp_mutex_unlock(&mpctx->translation_lock);
    return translation;
}

void mpctx_destroy_translation(struct MPContext *mpctx)
{
    if (!mpctx)
        return;
    mp_mutex_lock(&mpctx->translation_lock);
    mp_translation_destroy(&mpctx->translation);
    mp_mutex_unlock(&mpctx->translation_lock);
}
