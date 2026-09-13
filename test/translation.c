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

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "mpv_talloc.h"

#include "osdep/threads.h"
#include "osdep/timer.h"
#include "player/core.h"
#include "player/sub_translate.h"
#include "player/translation.h"
#include "test_utils.h"

struct fake_backend {
    mp_mutex lock;
    mp_cond condition;
    bool block;
    bool released;
    bool fail;
    int entered;
    int exited;
    int calls;
    const char *prefix;
};

struct wake_state {
    mp_mutex lock;
    mp_cond condition;
    int count;
};

struct collected_result {
    uint64_t cue_id;
    uint64_t revision;
    double pts;
    double duration;
    enum mp_translation_result_kind kind;
    char *text;
    char *translated;
    char *error;
};

struct collector {
    struct collected_result results[16];
    int count;
};

struct dense_collector {
    bool seen[256];
    int count;
};

#define INIT_THREAD_COUNT 16

struct init_race {
    struct MPContext *mpctx;
    mp_mutex lock;
    mp_cond condition;
    int ready;
    bool start;
    struct mp_translation *instances[INIT_THREAD_COUNT];
};

struct init_thread_arg {
    struct init_race *race;
    int index;
};

void mp_wakeup_core(struct MPContext *mpctx)
{
}

static MP_THREAD_VOID initialize_translation(void *ctx)
{
    struct init_thread_arg *arg = ctx;
    mp_mutex_lock(&arg->race->lock);
    arg->race->ready++;
    mp_cond_broadcast(&arg->race->condition);
    while (!arg->race->start)
        mp_cond_wait(&arg->race->condition, &arg->race->lock);
    mp_mutex_unlock(&arg->race->lock);
    arg->race->instances[arg->index] =
        mpctx_get_translation(arg->race->mpctx);
    MP_THREAD_RETURN();
}

static void test_concurrent_context_initialization(void)
{
    struct MPContext *mpctx = talloc_zero(NULL, struct MPContext);
    mp_mutex_init(&mpctx->translation_lock);
    struct init_race race = {.mpctx = mpctx};
    mp_mutex_init(&race.lock);
    mp_cond_init(&race.condition);
    struct init_thread_arg args[INIT_THREAD_COUNT];
    mp_thread threads[INIT_THREAD_COUNT];

    for (int n = 0; n < INIT_THREAD_COUNT; n++) {
        args[n] = (struct init_thread_arg){.race = &race, .index = n};
        assert_int_equal(
            mp_thread_create(&threads[n], initialize_translation, &args[n]),
            0);
    }

    mp_mutex_lock(&race.lock);
    while (race.ready != INIT_THREAD_COUNT)
        mp_cond_wait(&race.condition, &race.lock);
    race.start = true;
    mp_cond_broadcast(&race.condition);
    mp_mutex_unlock(&race.lock);

    for (int n = 0; n < INIT_THREAD_COUNT; n++)
        mp_thread_join(threads[n]);
    mp_require(race.instances[0]);
    for (int n = 1; n < INIT_THREAD_COUNT; n++)
        mp_require(race.instances[n] == race.instances[0]);
    mp_require(mpctx->translation == race.instances[0]);

    mpctx_destroy_translation(mpctx);
    mp_require(!mpctx->translation);
    mp_mutex_destroy(&mpctx->translation_lock);
    mp_cond_destroy(&race.condition);
    mp_mutex_destroy(&race.lock);
    talloc_free(mpctx);
}

static void wake_test(void *ctx)
{
    struct wake_state *wake = ctx;
    mp_mutex_lock(&wake->lock);
    wake->count++;
    mp_cond_broadcast(&wake->condition);
    mp_mutex_unlock(&wake->lock);
}

static void wait_for_wake(struct wake_state *wake, int previous)
{
    int64_t deadline = mp_time_ns() + MP_TIME_S_TO_NS(5);
    mp_mutex_lock(&wake->lock);
    while (wake->count <= previous) {
        if (mp_cond_timedwait_until(
                &wake->condition, &wake->lock, deadline))
        {
            mp_mutex_unlock(&wake->lock);
            mp_require(false);
        }
    }
    mp_mutex_unlock(&wake->lock);
}

static void wait_for_backend(struct fake_backend *backend, bool exited)
{
    int64_t deadline = mp_time_ns() + MP_TIME_S_TO_NS(5);
    mp_mutex_lock(&backend->lock);
    while ((exited ? backend->exited : backend->entered) == 0) {
        if (mp_cond_timedwait_until(
                &backend->condition, &backend->lock, deadline))
        {
            mp_mutex_unlock(&backend->lock);
            mp_require(false);
        }
    }
    mp_mutex_unlock(&backend->lock);
}

static void *fake_acquire(void *ctx)
{
    return ctx;
}

static void fake_release(void *ctx)
{
}

static void fake_call(void *ctx, void *talloc_ctx, const char *text,
                      struct wt_call_result *out)
{
    struct fake_backend *backend = ctx;
    mp_mutex_lock(&backend->lock);
    backend->calls++;
    backend->entered++;
    mp_cond_broadcast(&backend->condition);
    while (backend->block && !backend->released)
        mp_cond_wait(&backend->condition, &backend->lock);
    bool fail = backend->fail;
    const char *prefix = backend->prefix;
    backend->exited++;
    mp_cond_broadcast(&backend->condition);
    mp_mutex_unlock(&backend->lock);

    out->http_issued = true;
    if (fail) {
        snprintf(out->error, sizeof(out->error), "fake failure");
    } else {
        out->translated = talloc_asprintf(
            talloc_ctx, "%s%s", prefix ? prefix : "", text);
    }
}

static void fake_status(void *ctx, struct wt_status *out)
{
    *out = (struct wt_status){.enabled = true};
}

static const struct mp_translation_backend_ops fake_ops = {
    .acquire = fake_acquire,
    .release = fake_release,
    .call = fake_call,
    .get_status = fake_status,
};

static void collect_result(
    void *ctx, const struct mp_translation_result *result)
{
    struct collector *collector = ctx;
    mp_require(collector->count < MP_ARRAY_SIZE(collector->results));
    struct collected_result *copy =
        &collector->results[collector->count++];
    *copy = (struct collected_result){
        .cue_id = result->cue_id,
        .revision = result->revision,
        .pts = result->pts,
        .duration = result->duration,
        .kind = result->kind,
        .text = talloc_strdup(NULL, result->text),
        .translated = talloc_strdup(NULL, result->translated),
        .error = talloc_strdup(NULL, result->error),
    };
}

static void collect_dense_result(
    void *ctx, const struct mp_translation_result *result)
{
    struct dense_collector *collector = ctx;
    mp_require(result->cue_id < MP_ARRAY_SIZE(collector->seen));
    mp_require(!collector->seen[result->cue_id]);
    mp_require(result->translated);
    collector->seen[result->cue_id] = true;
    collector->count++;
}

static void clear_collector(struct collector *collector)
{
    for (int n = 0; n < collector->count; n++) {
        talloc_free(collector->results[n].text);
        talloc_free(collector->results[n].translated);
        talloc_free(collector->results[n].error);
    }
    *collector = (struct collector){0};
}

static void init_backend(struct fake_backend *backend, const char *prefix)
{
    *backend = (struct fake_backend){.prefix = prefix};
    mp_mutex_init(&backend->lock);
    mp_cond_init(&backend->condition);
}

static void uninit_backend(struct fake_backend *backend)
{
    mp_cond_destroy(&backend->condition);
    mp_mutex_destroy(&backend->lock);
}

static struct mp_translation *create_translation(
    struct wake_state *wake, struct fake_backend *backend)
{
    struct mp_translation *translation =
        mp_translation_create(NULL, NULL, wake_test, wake);
    mp_require(translation);
    mp_translation_register_source(
        translation, MP_TRANSLATION_SOURCE_WHISPER);
    mp_translation_register_source(
        translation, MP_TRANSLATION_SOURCE_SUBTITLE);
    mp_translation_set_playback_pts(translation, 0);
    mp_translation_set_backend(translation, &fake_ops, backend);
    return translation;
}

static void test_producers_and_identity(void)
{
    struct wake_state wake = {0};
    mp_mutex_init(&wake.lock);
    mp_cond_init(&wake.condition);
    struct fake_backend backend;
    init_backend(&backend, "translated:");
    struct mp_translation *translation =
        create_translation(&wake, &backend);
    struct collector collector = {0};

    int previous = wake.count;
    assert_int_equal(
        mp_translation_submit(
            translation, MP_TRANSLATION_SOURCE_SUBTITLE,
            41, 7, "hello", 12.125, 3.75, 0),
        MP_TRANSLATION_SUBMIT_QUEUED);
    wait_for_wake(&wake, previous);
    mp_translation_drain(
        translation, MP_TRANSLATION_SOURCE_SUBTITLE,
        collect_result, &collector);
    assert_int_equal(collector.count, 1);
    assert_int_equal(collector.results[0].cue_id, 41);
    assert_int_equal(collector.results[0].revision, 7);
    assert_float_equal(collector.results[0].pts, 12.125, 0.000001);
    assert_float_equal(collector.results[0].duration, 3.75, 0.000001);
    assert_string_equal(collector.results[0].text, "hello");
    assert_string_equal(collector.results[0].translated,
                        "translated:hello");
    clear_collector(&collector);

    previous = wake.count;
    mp_translation_submit(
        translation, MP_TRANSLATION_SOURCE_SUBTITLE,
        42, 1, "overlap", 20.0, 5.0, 0);
    mp_translation_submit(
        translation, MP_TRANSLATION_SOURCE_SUBTITLE,
        43, 1, "overlap", 21.0, 5.0, 0);
    while (collector.count < 2) {
        wait_for_wake(&wake, previous);
        previous = wake.count;
        mp_translation_drain(
            translation, MP_TRANSLATION_SOURCE_SUBTITLE,
            collect_result, &collector);
    }
    assert_int_equal(collector.results[0].cue_id, 42);
    assert_int_equal(collector.results[1].cue_id, 43);
    clear_collector(&collector);

    int calls_before_late = backend.calls;
    mp_translation_set_playback_pts(translation, 30);
    assert_int_equal(
        mp_translation_submit(
            translation, MP_TRANSLATION_SOURCE_SUBTITLE,
            44, 1, "late", 20, 2, 0),
        MP_TRANSLATION_SUBMIT_TOO_LATE);
    assert_int_equal(backend.calls, calls_before_late);

    mp_translation_destroy(&translation);
    uninit_backend(&backend);
    mp_cond_destroy(&wake.condition);
    mp_mutex_destroy(&wake.lock);
}

static void test_source_specific_filters(void)
{
    struct wake_state wake = {0};
    mp_mutex_init(&wake.lock);
    mp_cond_init(&wake.condition);
    struct fake_backend backend;
    init_backend(&backend, "ok:");
    struct mp_translation *translation =
        create_translation(&wake, &backend);
    struct mp_translation_limits limits = {
        .enabled = true,
        .horizon_sec = 60,
        .min_text_chars = 3,
        .reuse_cache_capacity = 16,
        .reuse_cache_window_ms = 0,
        .repeat_loop_threshold = 2,
        .repeat_loop_window_ms = 30000,
    };
    mp_translation_set_limits(translation, &limits);
    struct collector collector = {0};

    int previous = wake.count;
    mp_translation_submit(
        translation, MP_TRANSLATION_SOURCE_WHISPER,
        1, 1, "A", 5, 2,
        MP_TRANSLATION_FILTER_SHORT |
        MP_TRANSLATION_FILTER_REPEAT);
    wait_for_wake(&wake, previous);
    mp_translation_drain(
        translation, MP_TRANSLATION_SOURCE_WHISPER,
        collect_result, &collector);
    assert_int_equal(collector.results[0].kind,
                     MP_TRANSLATION_RESULT_FALLBACK_SHORT);
    int calls_after_short = backend.calls;
    clear_collector(&collector);

    previous = wake.count;
    mp_translation_submit(
        translation, MP_TRANSLATION_SOURCE_SUBTITLE,
        2, 1, "A", 8, 2, 0);
    wait_for_wake(&wake, previous);
    mp_translation_drain(
        translation, MP_TRANSLATION_SOURCE_SUBTITLE,
        collect_result, &collector);
    assert_string_equal(collector.results[0].translated, "ok:A");
    assert_int_equal(backend.calls, calls_after_short + 1);
    clear_collector(&collector);

    previous = wake.count;
    mp_translation_submit(
        translation, MP_TRANSLATION_SOURCE_SUBTITLE,
        3, 1, "repeat", 11, 2, 0);
    wait_for_wake(&wake, previous);
    mp_translation_drain(
        translation, MP_TRANSLATION_SOURCE_SUBTITLE,
        collect_result, &collector);
    clear_collector(&collector);
    previous = wake.count;
    mp_translation_submit(
        translation, MP_TRANSLATION_SOURCE_SUBTITLE,
        4, 1, "repeat", 14, 2, 0);
    wait_for_wake(&wake, previous);
    mp_translation_drain(
        translation, MP_TRANSLATION_SOURCE_SUBTITLE,
        collect_result, &collector);
    mp_require(collector.results[0].translated);
    clear_collector(&collector);

    previous = wake.count;
    backend.fail = true;
    mp_translation_submit(
        translation, MP_TRANSLATION_SOURCE_WHISPER,
        5, 1, "loop", 17, 2,
        MP_TRANSLATION_FILTER_REPEAT);
    wait_for_wake(&wake, previous);
    mp_translation_drain(
        translation, MP_TRANSLATION_SOURCE_WHISPER,
        collect_result, &collector);
    clear_collector(&collector);
    previous = wake.count;
    mp_translation_submit(
        translation, MP_TRANSLATION_SOURCE_WHISPER,
        6, 1, "loop", 20, 2,
        MP_TRANSLATION_FILTER_REPEAT);
    wait_for_wake(&wake, previous);
    mp_translation_drain(
        translation, MP_TRANSLATION_SOURCE_WHISPER,
        collect_result, &collector);
    assert_int_equal(collector.results[0].kind,
                     MP_TRANSLATION_RESULT_FALLBACK_LOOP);
    clear_collector(&collector);

    mp_translation_destroy(&translation);
    uninit_backend(&backend);
    mp_cond_destroy(&wake.condition);
    mp_mutex_destroy(&wake.lock);
}

static void test_errors_and_cancellation(void)
{
    struct wake_state wake = {0};
    mp_mutex_init(&wake.lock);
    mp_cond_init(&wake.condition);
    struct fake_backend first;
    struct fake_backend second;
    init_backend(&first, "old:");
    init_backend(&second, "new:");
    struct mp_translation *translation =
        create_translation(&wake, &first);
    struct collector collector = {0};
    struct mp_translation_limits cache_limits = {
        .enabled = true,
        .horizon_sec = 60,
        .reuse_cache_capacity = 16,
        .reuse_cache_window_ms = 60000,
    };
    mp_translation_set_limits(translation, &cache_limits);

    first.fail = true;
    int previous = wake.count;
    mp_translation_submit(
        translation, MP_TRANSLATION_SOURCE_SUBTITLE,
        10, 1, "failure", 5, 4, 0);
    wait_for_wake(&wake, previous);
    mp_translation_drain(
        translation, MP_TRANSLATION_SOURCE_SUBTITLE,
        collect_result, &collector);
    assert_int_equal(collector.results[0].kind,
                     MP_TRANSLATION_RESULT_FALLBACK_FAILURE);
    assert_string_equal(collector.results[0].error, "fake failure");
    clear_collector(&collector);

    first.fail = false;
    first.block = true;
    first.released = false;
    first.entered = 0;
    first.exited = 0;
    previous = wake.count;
    mp_translation_submit(
        translation, MP_TRANSLATION_SOURCE_SUBTITLE,
        11, 1, "same", 10, 4, 0);
    wait_for_backend(&first, false);
    mp_translation_set_backend(translation, &fake_ops, &second);
    mp_mutex_lock(&first.lock);
    first.released = true;
    mp_cond_broadcast(&first.condition);
    mp_mutex_unlock(&first.lock);
    wait_for_backend(&first, true);
    wait_for_wake(&wake, previous);
    mp_translation_drain(
        translation, MP_TRANSLATION_SOURCE_SUBTITLE,
        collect_result, &collector);
    assert_int_equal(collector.count, 0);

    previous = wake.count;
    mp_translation_submit(
        translation, MP_TRANSLATION_SOURCE_SUBTITLE,
        12, 2, "same", 15, 4, 0);
    wait_for_wake(&wake, previous);
    mp_translation_drain(
        translation, MP_TRANSLATION_SOURCE_SUBTITLE,
        collect_result, &collector);
    assert_string_equal(collector.results[0].translated, "new:same");
    assert_int_equal(second.calls, 1);
    clear_collector(&collector);

    second.block = true;
    second.released = false;
    second.entered = 0;
    second.exited = 0;
    previous = wake.count;
    mp_translation_submit(
        translation, MP_TRANSLATION_SOURCE_SUBTITLE,
        13, 1, "seek", 20, 4, 0);
    wait_for_backend(&second, false);
    mp_translation_invalidate_source(
        translation, MP_TRANSLATION_SOURCE_SUBTITLE);
    mp_mutex_lock(&second.lock);
    second.released = true;
    mp_cond_broadcast(&second.condition);
    mp_mutex_unlock(&second.lock);
    wait_for_backend(&second, true);
    wait_for_wake(&wake, previous);
    mp_translation_drain(
        translation, MP_TRANSLATION_SOURCE_SUBTITLE,
        collect_result, &collector);
    assert_int_equal(collector.count, 0);

    mp_translation_unregister_source(
        translation, MP_TRANSLATION_SOURCE_WHISPER);
    assert_int_equal(
        mp_translation_submit(
            translation, MP_TRANSLATION_SOURCE_WHISPER,
            14, 1, "off", 25, 4, 0),
        MP_TRANSLATION_SUBMIT_INACTIVE);
    assert_int_equal(
        mp_translation_submit(
            translation, MP_TRANSLATION_SOURCE_SUBTITLE,
            15, 1, "independent", 25, 4, 0),
        MP_TRANSLATION_SUBMIT_QUEUED);

    mp_translation_destroy(&translation);
    uninit_backend(&first);
    uninit_backend(&second);
    mp_cond_destroy(&wake.condition);
    mp_mutex_destroy(&wake.lock);
}

static void drain_dense_results(struct mp_translation *translation,
                                struct wake_state *wake,
                                struct dense_collector *collector,
                                int expected)
{
    while (collector->count < expected) {
        int previous = wake->count;
        mp_translation_drain(
            translation, MP_TRANSLATION_SOURCE_SUBTITLE,
            collect_dense_result, collector);
        if (collector->count >= expected)
            return;
        wait_for_wake(wake, previous);
    }
}

static void test_dense_result_backpressure(void)
{
    struct wake_state wake = {0};
    mp_mutex_init(&wake.lock);
    mp_cond_init(&wake.condition);
    struct fake_backend backend;
    init_backend(&backend, "dense:");
    backend.block = true;
    struct mp_translation *translation =
        create_translation(&wake, &backend);
    struct mp_translation_limits limits = {.enabled = false};
    mp_translation_set_limits(translation, &limits);

    bool deferred[256] = {0};
    int accepted = 0;
    int deferred_count = 0;
    for (int n = 0; n < MP_ARRAY_SIZE(deferred); n++) {
        char text[32];
        snprintf(text, sizeof(text), "cue-%d", n);
        enum mp_translation_submit_result result =
            mp_translation_submit(
                translation, MP_TRANSLATION_SOURCE_SUBTITLE,
                n, 1, text, 10 + n, 2, 0);
        if (result == MP_TRANSLATION_SUBMIT_QUEUED) {
            accepted++;
        } else {
            assert_int_equal(result,
                             MP_TRANSLATION_SUBMIT_BACKPRESSURE);
            deferred[n] = true;
            deferred_count++;
        }
    }
    assert_int_equal(accepted, 128);
    assert_int_equal(deferred_count, 128);

    wait_for_backend(&backend, false);
    mp_mutex_lock(&backend.lock);
    backend.released = true;
    mp_cond_broadcast(&backend.condition);
    mp_mutex_unlock(&backend.lock);

    struct dense_collector collector = {0};
    drain_dense_results(translation, &wake, &collector, accepted);
    for (int n = 0; n < MP_ARRAY_SIZE(deferred); n++) {
        if (!deferred[n])
            continue;
        char text[32];
        snprintf(text, sizeof(text), "cue-%d", n);
        assert_int_equal(
            mp_translation_submit(
                translation, MP_TRANSLATION_SOURCE_SUBTITLE,
                n, 1, text, 10 + n, 2, 0),
            MP_TRANSLATION_SUBMIT_QUEUED);
    }
    drain_dense_results(
        translation, &wake, &collector, MP_ARRAY_SIZE(deferred));
    assert_int_equal(collector.count, MP_ARRAY_SIZE(deferred));

    mp_translation_destroy(&translation);
    uninit_backend(&backend);
    mp_cond_destroy(&wake.condition);
    mp_mutex_destroy(&wake.lock);
}

static void test_config_and_source_policy(void)
{
    char *error = NULL;
    const char *valid =
        "{\"provider\":\"ai\",\"source_lang\":\"auto\","
        "\"target_lang\":\"zh\",\"ai\":{"
        "\"endpoint\":\"http://127.0.0.1:9999/v1/chat/completions\","
        "\"model\":\"fixture\",\"api_key\":\"secret\","
        "\"system_prompt\":\"translate\",\"context_size\":0,"
        "\"max_tokens\":64,\"timeout_ms\":1000},"
        "\"limits\":{\"enabled\":true,\"horizon_sec\":60,"
        "\"seek_debounce_ms\":0,\"min_text_chars\":2,"
        "\"reuse_cache_capacity\":16,\"reuse_cache_window_ms\":1000,"
        "\"repeat_loop_threshold\":3,\"repeat_loop_window_ms\":1000,"
        "\"rpm_limit\":0,\"session_request_limit\":0}}";
    assert_true(mp_translation_validate_common_config(valid, &error));
    mp_require(!error);
    char *masked = mp_translation_mask_config(NULL, valid);
    mp_require(strstr(masked, "secret") == NULL);
    mp_require(strstr(masked, "***") != NULL);
    talloc_free(masked);

    const char *wrong_prompt =
        "{\"provider\":\"ai\",\"target_lang\":\"zh\",\"ai\":{"
        "\"endpoint\":\"http://127.0.0.1\",\"model\":\"fixture\","
        "\"prompt\":\"wrong\"}}";
    assert_false(mp_translation_validate_common_config(
        wrong_prompt, &error));
    talloc_free(error);
    error = NULL;
    assert_false(mp_translation_validate_common_config(
        "{\"provider\":\"google\",\"source_lang\":\"auto\"}", &error));
    talloc_free(error);
    error = NULL;
    assert_false(mp_translation_validate_common_config(
        "{provider:\"google\",target_lang:\"zh\",}", &error));
    talloc_free(error);
    error = NULL;
    assert_false(mp_translation_validate_common_config(
        "{\"provider\"=\"google\",\"target_lang\":\"zh\"}", &error));
    talloc_free(error);
    error = NULL;
    assert_false(mp_translation_validate_common_config(
        "{\"provider\":\"google\",\"target_lang\":\"zh\","
        "\"limits\":{\"rpm_limit\":-1}}", &error));
    talloc_free(error);

    assert_int_equal(
        sub_translate_classify_source("whisper", true),
        SUB_TRANSLATE_SOURCE_GENERATED);
    assert_int_equal(
        sub_translate_classify_source("translated", true),
        SUB_TRANSLATE_SOURCE_GENERATED);
    assert_int_equal(
        sub_translate_classify_source(NULL, false),
        SUB_TRANSLATE_SOURCE_BITMAP);
    assert_int_equal(
        sub_translate_classify_source(NULL, true),
        SUB_TRANSLATE_SOURCE_TEXT);

    char *escaped = sub_translate_escape_ass(
        NULL, "line {tag}\\value\nnext");
    assert_string_equal(escaped,
        "line \\{tag}\\\xE2\x81\xA0value\\Nnext");
    talloc_free(escaped);

    struct wake_state wake = {0};
    mp_mutex_init(&wake.lock);
    mp_cond_init(&wake.condition);
    struct mp_translation *translation =
        mp_translation_create(NULL, NULL, wake_test, &wake);
    mp_require(translation);
    assert_int_equal(
        mp_translation_set_common_config(translation, "", &error), 0);
    assert_true(mp_translation_common_config_explicit(translation));
    assert_false(mp_translation_common_config_enabled(translation));
    mp_translation_register_source(
        translation, MP_TRANSLATION_SOURCE_WHISPER);
    assert_false(mp_translation_set_legacy_ai(
        translation,
        "{\"endpoint\":\"http://127.0.0.1\",\"model\":\"x\","
        "\"target_lang\":\"zh\"}"));
    assert_int_equal(
        mp_translation_submit(
            translation, MP_TRANSLATION_SOURCE_WHISPER,
            1, 1, "plain", 0, 2,
            MP_TRANSLATION_FILTER_SHORT |
            MP_TRANSLATION_FILTER_REPEAT),
        MP_TRANSLATION_SUBMIT_NO_BACKEND);
    mp_translation_destroy(&translation);
    mp_cond_destroy(&wake.condition);
    mp_mutex_destroy(&wake.lock);

    mp_mutex_init(&wake.lock);
    mp_cond_init(&wake.condition);
    translation = mp_translation_create(NULL, NULL, wake_test, &wake);
    mp_require(translation);
    assert_int_equal(
        mp_translation_configure_legacy_whisper(
            translation, WT_PROVIDER_GOOGLE,
            "en", "zh", NULL,
            "{\"enabled\":true,\"horizon_sec\":33,"
            "\"seek_debounce_ms\":25,\"min_text_chars\":2}"),
        0);
    assert_true(mp_translation_has_backend(translation));
    struct mp_translation_limits limits;
    mp_translation_get_limits(translation, &limits);
    assert_int_equal(limits.horizon_sec, 33);
    assert_int_equal(limits.seek_debounce_ms, 25);
    assert_int_equal(
        mp_translation_set_legacy_limits(
            translation,
            "{\"horizon_sec\":-1,\"seek_debounce_ms\":-1,"
            "\"min_text_chars\":-1,\"rpm_limit\":-1,"
            "\"session_request_limit\":-1}"),
        0);
    mp_translation_get_limits(translation, &limits);
    assert_int_equal(limits.horizon_sec, 60);
    assert_int_equal(limits.seek_debounce_ms, 1500);
    assert_int_equal(limits.min_text_chars, 2);
    assert_int_equal(limits.rpm_limit, 0);
    assert_int_equal(limits.session_request_limit, 0);
    char *status = mp_translation_get_legacy_status(translation, NULL);
    mp_require(status);
    mp_require(strstr(status, "\"horizon_sec\":60"));
    mp_require(strstr(status, "\"seek_debounce_ms\":1500"));
    mp_require(strstr(status, "\"session_req_used\":0"));
    talloc_free(status);
    mp_translation_stop_legacy_whisper(translation);
    assert_false(mp_translation_has_backend(translation));
    mp_translation_destroy(&translation);
    mp_cond_destroy(&wake.condition);
    mp_mutex_destroy(&wake.lock);
}

int main(void)
{
    mp_time_init();
    test_concurrent_context_initialization();
    test_producers_and_identity();
    test_source_specific_filters();
    test_errors_and_cancellation();
    test_dense_result_backpressure();
    test_config_and_source_policy();
    return 0;
}
