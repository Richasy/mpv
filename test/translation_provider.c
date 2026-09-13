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
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "mpv_talloc.h"
#include "osdep/threads.h"
#include "player/whisper_translate_test.h"
#include "test_utils.h"

#define MAX_FAKE_CALLS 16

struct fake_clock {
    atomic_int_fast64_t monotonic_ms;
    atomic_int_fast64_t unix_ms;
};

struct response_plan {
    enum wt_http_failure failure;
    bool http_issued;
    int http_status;
    const char *retry_after;
    const unsigned char *body;
    size_t body_len;
    bool block;
    bool released;
};

struct captured_request {
    char *host;
    int port;
    bool secure;
    char *method;
    char *path;
    char *headers;
    char *body;
    size_t body_len;
    enum wt_http_proxy_mode proxy_mode;
};

struct fake_transport {
    mp_mutex lock;
    mp_cond condition;
    struct response_plan plans[MAX_FAKE_CALLS];
    struct captured_request requests[MAX_FAKE_CALLS];
    int plan_count;
    int calls;
};

struct call_thread {
    struct whisper_translator *translator;
    const char *text;
    struct wt_call_result result;
    void *tmp;
};

static int64_t fake_monotonic_ms(void *ctx)
{
    struct fake_clock *clock = ctx;
    return atomic_load_explicit(&clock->monotonic_ms, memory_order_acquire);
}

static int64_t fake_unix_ms(void *ctx)
{
    struct fake_clock *clock = ctx;
    return atomic_load_explicit(&clock->unix_ms, memory_order_acquire);
}

static void init_clock(struct fake_clock *clock,
                       int64_t monotonic_ms, int64_t unix_ms)
{
    atomic_init(&clock->monotonic_ms, monotonic_ms);
    atomic_init(&clock->unix_ms, unix_ms);
}

static void set_monotonic_ms(struct fake_clock *clock, int64_t value)
{
    atomic_store_explicit(&clock->monotonic_ms, value, memory_order_release);
}

static void init_transport(struct fake_transport *transport)
{
    *transport = (struct fake_transport){0};
    mp_mutex_init(&transport->lock);
    mp_cond_init(&transport->condition);
}

static void destroy_transport(struct fake_transport *transport)
{
    for (int n = 0; n < transport->calls; n++) {
        talloc_free(transport->requests[n].host);
        talloc_free(transport->requests[n].method);
        talloc_free(transport->requests[n].path);
        talloc_free(transport->requests[n].headers);
        talloc_free(transport->requests[n].body);
    }
    mp_cond_destroy(&transport->condition);
    mp_mutex_destroy(&transport->lock);
}

static void add_plan(struct fake_transport *transport,
                     enum wt_http_failure failure, bool issued, int status,
                     const char *retry_after, const void *body,
                     size_t body_len, bool block)
{
    mp_require(transport->plan_count < MAX_FAKE_CALLS);
    transport->plans[transport->plan_count++] = (struct response_plan){
        .failure = failure,
        .http_issued = issued,
        .http_status = status,
        .retry_after = retry_after,
        .body = body,
        .body_len = body_len,
        .block = block,
    };
}

static void add_text_plan(struct fake_transport *transport, int status,
                          const char *retry_after, const char *body)
{
    add_plan(transport, WT_HTTP_FAILURE_NONE, true, status, retry_after,
             body, body ? strlen(body) : 0, false);
}

static void fake_http(void *ctx, void *talloc_ctx,
                      const struct wt_http_request *request,
                      struct wt_http_response *response)
{
    struct fake_transport *transport = ctx;
    mp_mutex_lock(&transport->lock);
    int index = transport->calls++;
    mp_require(index < transport->plan_count);
    struct captured_request *captured = &transport->requests[index];
    captured->host = talloc_strdup(NULL, request->host);
    captured->port = request->port;
    captured->secure = request->secure;
    captured->method = talloc_strdup(NULL, request->method);
    captured->path = talloc_strdup(NULL, request->path);
    captured->headers = talloc_strdup(
        NULL, request->headers ? request->headers : "");
    captured->body = talloc_strndup(
        NULL, request->body ? request->body : "", request->body_len);
    captured->body_len = request->body_len;
    captured->proxy_mode = request->proxy_mode;
    mp_cond_broadcast(&transport->condition);

    struct response_plan *plan = &transport->plans[index];
    while (plan->block && !plan->released) {
        int wait = mp_cond_timedwait(
            &transport->condition, &transport->lock,
            MP_TIME_S_TO_NS(5));
        mp_require(!wait);
    }
    *response = (struct wt_http_response){
        .failure = plan->failure,
        .http_issued = plan->http_issued,
        .http_status = plan->http_status,
        .retry_after = plan->retry_after,
        .body = plan->body,
        .body_len = plan->body_len,
    };
    mp_mutex_unlock(&transport->lock);
}

static void wait_for_calls(struct fake_transport *transport, int count)
{
    mp_mutex_lock(&transport->lock);
    while (transport->calls < count) {
        int wait = mp_cond_timedwait(
            &transport->condition, &transport->lock,
            MP_TIME_S_TO_NS(5));
        mp_require(!wait);
    }
    mp_mutex_unlock(&transport->lock);
}

static void release_call(struct fake_transport *transport, int index)
{
    mp_mutex_lock(&transport->lock);
    transport->plans[index].released = true;
    mp_cond_broadcast(&transport->condition);
    mp_mutex_unlock(&transport->lock);
}

static struct whisper_translator *create_translator(
    enum wt_provider provider, const char *source_lang,
    const char *target_lang, struct fake_transport *transport,
    struct fake_clock *clock)
{
    struct wt_test_hooks hooks = {
        .transport = fake_http,
        .transport_ctx = transport,
        .monotonic_ms = fake_monotonic_ms,
        .unix_ms = fake_unix_ms,
        .clock_ctx = clock,
    };
    if (provider == WT_PROVIDER_OPENAI) {
        struct wt_openai_config config = {
            .endpoint = "http://127.0.0.1:11434/v1/chat/completions",
            .model = "fixture",
            .source_lang = source_lang,
            .target_lang = target_lang,
            .timeout_ms = 1000,
            .max_tokens = 0,
        };
        return whisper_translator_create_for_test(
            NULL, provider, NULL, NULL, &config, &hooks);
    }
    return whisper_translator_create_for_test(
        NULL, provider, source_lang, target_lang, NULL, &hooks);
}

static MP_THREAD_VOID call_translate(void *ctx)
{
    struct call_thread *call = ctx;
    call->tmp = talloc_new(NULL);
    whisper_translate_call(
        call->translator, call->tmp, call->text, &call->result);
    MP_THREAD_RETURN();
}

static void destroy_call(struct call_thread *call)
{
    talloc_free(call->tmp);
}

static void test_google_request_and_response(void)
{
    struct fake_clock clock;
    init_clock(&clock, 1000, 0);
    struct fake_transport transport;
    init_transport(&transport);
    add_text_plan(
        &transport, 200, NULL,
        "{\"sentences\":["
        "{\"trans\":\"  안녕\\n\",\"orig\":\"hello\"},"
        "{\"trans\":\"\\ud55c\\uad6d \\ud83d\\ude00 &amp;  \",\"orig\":\"world\"},"
        "{\"translit\":\"annyeong\"}]}");
    struct whisper_translator *translator = create_translator(
        WT_PROVIDER_GOOGLE, "AUTO", "KO", &transport, &clock);
    mp_require(translator);

    void *tmp = talloc_new(NULL);
    struct wt_call_result result;
    whisper_translate_call(
        translator, tmp, "Line 1\n한국 😀 &+/?", &result);
    assert_string_equal(result.translated, "  안녕\n한국 😀 &amp;  ");
    assert_true(result.http_issued);
    assert_int_equal(result.http_status, 200);
    assert_false(result.rate_limited);
    assert_int_equal(transport.calls, 1);

    struct captured_request *request = &transport.requests[0];
    assert_string_equal(request->host, "translate.google.com");
    assert_int_equal(request->port, 443);
    assert_true(request->secure);
    assert_string_equal(request->method, "POST");
    assert_string_equal(
        request->path,
        "/translate_a/single?client=at&dt=t&dt=rm&dj=1");
    assert_string_equal(
        request->headers,
        "Content-Type: application/x-www-form-urlencoded;charset=utf-8\r\n");
    assert_string_equal(
        request->body,
        "sl=auto&tl=ko&q=Line+1%0A%ED%95%9C%EA%B5%AD+"
        "%F0%9F%98%80+%26%2B%2F%3F");
    assert_int_equal(request->proxy_mode, WT_HTTP_PROXY_DEFAULT);

    talloc_free(tmp);
    whisper_translator_destroy(&translator);
    destroy_transport(&transport);

    init_transport(&transport);
    static const unsigned char invalid_utf8_body[] = {
        '{', '"', 'x', '"', ':', '"', 0xc0, 0xaf, '"', '}',
    };
    add_plan(&transport, WT_HTTP_FAILURE_NONE, true, 200, NULL,
             invalid_utf8_body, sizeof(invalid_utf8_body), false);
    translator = create_translator(
        WT_PROVIDER_GOOGLE, "auto", "ko", &transport, &clock);
    tmp = talloc_new(NULL);
    whisper_translate_call(translator, tmp, "cue", &result);
    assert_string_equal(result.error, "google: invalid response encoding");
    assert_true(result.http_issued);
    talloc_free(tmp);
    whisper_translator_destroy(&translator);
    destroy_transport(&transport);
}

static void expect_parse_failure(enum wt_provider provider,
                                 const char *target_lang,
                                 const char *response)
{
    struct fake_clock clock;
    init_clock(&clock, 1000, 0);
    struct fake_transport transport;
    init_transport(&transport);
    add_text_plan(&transport, 200, NULL, response);
    struct whisper_translator *translator = create_translator(
        provider, "auto", target_lang, &transport, &clock);
    mp_require(translator);

    void *tmp = talloc_new(NULL);
    struct wt_call_result result;
    whisper_translate_call(translator, tmp, "cue", &result);
    mp_require(!result.translated);
    assert_true(result.http_issued);
    assert_string_equal(
        result.error,
        provider == WT_PROVIDER_GOOGLE
            ? "google: parse failed" : "azure: parse failed");

    talloc_free(tmp);
    whisper_translator_destroy(&translator);
    destroy_transport(&transport);
}

static void test_google_strict_response_validation(void)
{
    static const char *const malformed[] = {
        "[]",
        "{\"sentences\":{}}",
        "{\"sentences\":[1]}",
        "{\"sentences\":[{\"trans\":7}]}",
        "{\"sentences\":[{\"trans\":\"ok\",\"translit\":7}]}",
        "{\"sentences\":[{\"other\":\"value\"}]}",
        "{\"sentences\":[{\"trans\":\"\"}]}",
        "{\"sentences\":[{\"trans\":\"ok\"}]} trailing",
        "{\"sentences\":[{\"trans\":\"\\udc00\"}]}",
    };
    for (int n = 0; n < MP_ARRAY_SIZE(malformed); n++)
        expect_parse_failure(WT_PROVIDER_GOOGLE, "ko", malformed[n]);
}

static void test_bing_request_and_response(void)
{
    struct fake_clock clock;
    init_clock(&clock, 2000, 0);
    struct fake_transport transport;
    init_transport(&transport);
    add_text_plan(
        &transport, 200, NULL,
        "[ { \"translations\" : ["
        "{ \"text\" : \" \\ud55c\\uad6d\\n\\ud83d\\ude00 \", "
        "\"to\" : \"ZH-cn\" },"
        "{\"text\":\"autre\",\"to\":\"fr\"}"
        "] } ]");
    struct whisper_translator *translator = create_translator(
        WT_PROVIDER_AZURE, "AUTO", "zh-CN", &transport, &clock);
    mp_require(translator);

    void *tmp = talloc_new(NULL);
    struct wt_call_result result;
    whisper_translate_call(
        translator, tmp, "cue \"x\"\n한국 😀", &result);
    assert_string_equal(result.translated, " 한국\n😀 ");
    assert_int_equal(transport.calls, 1);

    struct captured_request *request = &transport.requests[0];
    assert_string_equal(request->host, "edge.microsoft.com");
    assert_int_equal(request->port, 443);
    assert_true(request->secure);
    assert_string_equal(request->method, "POST");
    assert_string_equal(
        request->path,
        "/translate/translatetext?from=&to=zh-Hans&isEnterpriseClient=false");
    assert_string_equal(
        request->headers,
        "Content-Type: application/json; charset=utf-8\r\n");
    mp_require(strstr(request->headers, "Authorization") == NULL);
    assert_string_equal(request->body, "[\"cue \\\"x\\\"\\n한국 😀\"]");
    assert_int_equal(request->proxy_mode, WT_HTTP_PROXY_DEFAULT);

    talloc_free(tmp);
    whisper_translator_destroy(&translator);
    destroy_transport(&transport);
}

static void test_bing_strict_response_validation(void)
{
    static const char *const malformed[] = {
        "{}",
        "[]",
        "[{},{}]",
        "[{\"translations\":{}}]",
        "[{\"translations\":[1]}]",
        "[{\"translations\":[{\"text\":1,\"to\":\"ko\"}]}]",
        "[{\"translations\":[{\"text\":\"x\",\"to\":1}]}]",
        "[{\"translations\":[{\"text\":\"x\",\"to\":\"fr\"}]}]",
        "[{\"translations\":[{\"text\":\"x\",\"to\":\"ko\"},"
        "{\"text\":\"y\",\"to\":\"KO\"}]}]",
        "[{\"translations\":[{\"text\":\"\",\"to\":\"ko\"}]}]",
        "[{\"translations\":[{\"text\":\"x\",\"to\":\"ko\"}]}] trailing",
        "[{\"translations\":[{\"text\":\"\\udc00\",\"to\":\"ko\"}]}]",
    };
    for (int n = 0; n < MP_ARRAY_SIZE(malformed); n++)
        expect_parse_failure(WT_PROVIDER_AZURE, "ko", malformed[n]);
}

static void test_openai_strict_response_validation(void)
{
    struct fake_clock clock;
    init_clock(&clock, 2500, 0);
    struct fake_transport transport;
    init_transport(&transport);
    add_text_plan(
        &transport, 200, NULL,
        "{\"choices\":[{\"message\":{\"content\":\"\\ud83d\\ude00\"}}]}");
    add_text_plan(
        &transport, 200, NULL,
        "{\"choices\":[{\"message\":{\"content\":\"ignored\"}}]} trailing");
    struct whisper_translator *translator = create_translator(
        WT_PROVIDER_OPENAI, "auto", "ko", &transport, &clock);
    mp_require(translator);
    void *tmp = talloc_new(NULL);
    struct wt_call_result result;
    whisper_translate_call(translator, tmp, "cue", &result);
    assert_string_equal(result.translated, "😀");
    whisper_translate_call(translator, tmp, "cue", &result);
    mp_require(!result.translated);
    assert_string_equal(result.error, "openai: empty content");
    assert_int_equal(transport.calls, 2);
    talloc_free(tmp);
    whisper_translator_destroy(&translator);
    destroy_transport(&transport);
}

static char *make_emoji_text(void *parent, int count)
{
    char *text = talloc_array(parent, char, (size_t)count * 4 + 1);
    char *cursor = text;
    for (int n = 0; n < count; n++) {
        memcpy(cursor, "\xF0\x9F\x98\x80", 4);
        cursor += 4;
    }
    *cursor = '\0';
    return text;
}

static void test_input_bounds_and_languages(void)
{
    struct fake_clock clock;
    init_clock(&clock, 3000, 0);
    struct fake_transport transport;
    init_transport(&transport);
    add_text_plan(
        &transport, 200, NULL,
        "{\"sentences\":[{\"trans\":\"ok\"}]}");
    struct whisper_translator *translator = create_translator(
        WT_PROVIDER_GOOGLE, "zh-hans", "zh-HANT", &transport, &clock);
    mp_require(translator);

    void *tmp = talloc_new(NULL);
    char *at_limit = make_emoji_text(tmp, 5000);
    struct wt_call_result result;
    whisper_translate_call(translator, tmp, at_limit, &result);
    assert_string_equal(result.translated, "ok");
    assert_int_equal(transport.calls, 1);
    const char *prefix = "sl=zh-CN&tl=zh-TW&q=";
    mp_require(strncmp(
        transport.requests[0].body, prefix, strlen(prefix)) == 0);

    char *over_limit = make_emoji_text(tmp, 5001);
    whisper_translate_call(translator, tmp, over_limit, &result);
    mp_require(!result.translated);
    assert_false(result.http_issued);
    assert_string_equal(result.error, "google: input too long");
    assert_int_equal(transport.calls, 1);

    const char invalid_utf8[] = {(char)0xc0, (char)0xaf, '\0'};
    whisper_translate_call(translator, tmp, invalid_utf8, &result);
    mp_require(!result.translated);
    assert_false(result.http_issued);
    assert_string_equal(result.error, "google: invalid UTF-8");
    assert_int_equal(transport.calls, 1);
    talloc_free(tmp);
    whisper_translator_destroy(&translator);
    destroy_transport(&transport);

    init_transport(&transport);
    translator = create_translator(
        WT_PROVIDER_AZURE, "en", "auto", &transport, &clock);
    mp_require(translator);
    tmp = talloc_new(NULL);
    whisper_translate_call(translator, tmp, "cue", &result);
    assert_false(result.http_issued);
    assert_string_equal(result.error, "azure: invalid language");
    assert_int_equal(transport.calls, 0);
    struct wt_status status;
    whisper_translator_get_status(translator, &status);
    assert_int_equal(status.fail_count, 0);
    talloc_free(tmp);
    whisper_translator_destroy(&translator);
    destroy_transport(&transport);

    init_transport(&transport);
    translator = create_translator(
        WT_PROVIDER_AZURE, "en", "ko", &transport, &clock);
    mp_require(translator);
    tmp = talloc_new(NULL);
    over_limit = make_emoji_text(tmp, 5001);
    whisper_translate_call(translator, tmp, over_limit, &result);
    assert_false(result.http_issued);
    assert_string_equal(result.error, "azure: input too long");
    assert_int_equal(transport.calls, 0);
    talloc_free(tmp);
    whisper_translator_destroy(&translator);
    destroy_transport(&transport);

    init_transport(&transport);
    translator = create_translator(
        WT_PROVIDER_GOOGLE, "en?bad", "ko", &transport, &clock);
    mp_require(translator);
    tmp = talloc_new(NULL);
    whisper_translate_call(translator, tmp, "cue", &result);
    assert_false(result.http_issued);
    assert_string_equal(result.error, "google: invalid language");
    assert_int_equal(transport.calls, 0);
    talloc_free(tmp);
    whisper_translator_destroy(&translator);
    destroy_transport(&transport);
}

static void test_transport_failures(void)
{
    struct {
        enum wt_http_failure failure;
        bool issued;
        int status;
        const void *body;
        size_t body_len;
        const char *error;
        int expected_fail_count;
    } cases[] = {
        {WT_HTTP_FAILURE_SETUP, false, 0, NULL, 0,
         "google: request setup failed", 0},
        {WT_HTTP_FAILURE_SEND, true, 0, NULL, 0,
         "google: request send failed", 1},
        {WT_HTTP_FAILURE_RECEIVE, true, 0, NULL, 0,
         "google: response receive failed", 1},
        {WT_HTTP_FAILURE_READ, true, 200, NULL, 0,
         "google: response read failed", 1},
        {WT_HTTP_FAILURE_TOO_LARGE, true, 200, NULL, 0,
         "google: response too large", 1},
        {WT_HTTP_FAILURE_NONE, true, 0, NULL, 0,
         "google: invalid HTTP status", 1},
        {WT_HTTP_FAILURE_NONE, true, 302, NULL, 0,
         "google: HTTP 302", 1},
        {WT_HTTP_FAILURE_NONE, true, 401, NULL, 0,
         "google: HTTP 401", 1},
    };
    for (int n = 0; n < MP_ARRAY_SIZE(cases); n++) {
        struct fake_clock clock;
        init_clock(&clock, 4000, 0);
        struct fake_transport transport;
        init_transport(&transport);
        add_plan(&transport, cases[n].failure, cases[n].issued,
                 cases[n].status, NULL, cases[n].body,
                 cases[n].body_len, false);
        struct whisper_translator *translator = create_translator(
            WT_PROVIDER_GOOGLE, "auto", "ko", &transport, &clock);
        mp_require(translator);
        void *tmp = talloc_new(NULL);
        struct wt_call_result result;
        whisper_translate_call(translator, tmp, "cue", &result);
        mp_require(!result.translated);
        assert_int_equal(result.http_issued, cases[n].issued);
        assert_string_equal(result.error, cases[n].error);
        struct wt_status status;
        whisper_translator_get_status(translator, &status);
        assert_int_equal(status.fail_count, cases[n].expected_fail_count);
        talloc_free(tmp);
        whisper_translator_destroy(&translator);
        destroy_transport(&transport);
    }

    struct fake_clock clock;
    init_clock(&clock, 4000, 0);
    struct fake_transport transport;
    init_transport(&transport);
    static const unsigned char oversized[] = "x";
    add_plan(&transport, WT_HTTP_FAILURE_NONE, true, 200, NULL,
             oversized, whisper_translate_test_max_response_bytes() + 1,
             false);
    struct whisper_translator *translator = create_translator(
        WT_PROVIDER_GOOGLE, "auto", "ko", &transport, &clock);
    void *tmp = talloc_new(NULL);
    struct wt_call_result result;
    whisper_translate_call(translator, tmp, "cue", &result);
    assert_string_equal(result.error, "google: response too large");
    assert_true(result.http_issued);
    talloc_free(tmp);
    whisper_translator_destroy(&translator);
    destroy_transport(&transport);

    init_transport(&transport);
    static const unsigned char nul_body[] = {'{', '\0', '}'};
    add_plan(&transport, WT_HTTP_FAILURE_NONE, true, 200, NULL,
             nul_body, sizeof(nul_body), false);
    translator = create_translator(
        WT_PROVIDER_GOOGLE, "auto", "ko", &transport, &clock);
    tmp = talloc_new(NULL);
    whisper_translate_call(translator, tmp, "cue", &result);
    assert_string_equal(result.error, "google: invalid response body");
    assert_true(result.http_issued);
    talloc_free(tmp);
    whisper_translator_destroy(&translator);
    destroy_transport(&transport);
}

static const char *rate_name(enum wt_provider provider)
{
    return provider == WT_PROVIDER_GOOGLE ? "google"
         : provider == WT_PROVIDER_AZURE ? "azure" : "openai";
}

static void test_common_rate_limit(void)
{
    enum wt_provider providers[] = {
        WT_PROVIDER_GOOGLE,
        WT_PROVIDER_AZURE,
        WT_PROVIDER_OPENAI,
    };
    for (int n = 0; n < MP_ARRAY_SIZE(providers); n++) {
        struct fake_clock clock;
        init_clock(&clock, 5000, 0);
        struct fake_transport transport;
        init_transport(&transport);
        add_text_plan(&transport, 429, "17", NULL);
        struct whisper_translator *translator = create_translator(
            providers[n], "auto", "ko", &transport, &clock);
        mp_require(translator);

        void *tmp = talloc_new(NULL);
        struct wt_call_result first;
        whisper_translate_call(translator, tmp, "cue", &first);
        assert_true(first.http_issued);
        assert_true(first.rate_limited);
        assert_int_equal(first.http_status, 429);
        assert_int_equal(first.retry_after_ms, 17000);
        char expected[64];
        snprintf(expected, sizeof(expected), "%s: HTTP 429",
                 rate_name(providers[n]));
        assert_string_equal(first.error, expected);

        struct wt_call_result local;
        whisper_translate_call(translator, tmp, "next", &local);
        assert_false(local.http_issued);
        assert_int_equal(local.http_status, 0);
        assert_true(local.rate_limited);
        assert_int_equal(local.retry_after_ms, 17000);
        snprintf(expected, sizeof(expected), "%s: rate limited",
                 rate_name(providers[n]));
        assert_string_equal(local.error, expected);
        assert_int_equal(transport.calls, 1);

        struct wt_status status;
        whisper_translator_get_status(translator, &status);
        assert_true(status.paused);
        assert_int_equal(status.fail_count, 1);
        assert_int_equal(status.retry_after_ms, 17000);
        assert_string_equal(status.last_error, "");

        if (providers[n] == WT_PROVIDER_OPENAI)
            assert_int_equal(
                transport.requests[0].proxy_mode, WT_HTTP_PROXY_NONE);
        talloc_free(tmp);
        whisper_translator_destroy(&translator);
        destroy_transport(&transport);
    }
}

static void test_retry_after_date_default_and_saturation(void)
{
    struct fake_clock clock;
    init_clock(&clock, 6000, 0);
    struct fake_transport transport;
    init_transport(&transport);
    add_text_plan(
        &transport, 429, "Thu, 01 Jan 1970 00:20:01 GMT", NULL);
    struct whisper_translator *translator = create_translator(
        WT_PROVIDER_AZURE, "auto", "ko", &transport, &clock);
    void *tmp = talloc_new(NULL);
    struct wt_call_result result;
    whisper_translate_call(translator, tmp, "cue", &result);
    assert_int_equal(result.retry_after_ms, 1201000);
    whisper_translate_call(translator, tmp, "next", &result);
    assert_false(result.http_issued);
    assert_int_equal(result.retry_after_ms, 1201000);
    talloc_free(tmp);
    whisper_translator_destroy(&translator);
    destroy_transport(&transport);

    init_transport(&transport);
    add_text_plan(&transport, 429, NULL, NULL);
    translator = create_translator(
        WT_PROVIDER_GOOGLE, "auto", "ko", &transport, &clock);
    tmp = talloc_new(NULL);
    whisper_translate_call(translator, tmp, "cue", &result);
    assert_int_equal(result.retry_after_ms, 0);
    whisper_translate_call(translator, tmp, "next", &result);
    assert_int_equal(result.retry_after_ms, 60000);
    talloc_free(tmp);
    whisper_translator_destroy(&translator);
    destroy_transport(&transport);

    init_transport(&transport);
    add_text_plan(&transport, 429, "3000000", NULL);
    translator = create_translator(
        WT_PROVIDER_GOOGLE, "auto", "ko", &transport, &clock);
    tmp = talloc_new(NULL);
    whisper_translate_call(translator, tmp, "cue", &result);
    assert_int_equal(result.retry_after_ms, INT_MAX);
    whisper_translate_call(translator, tmp, "next", &result);
    assert_int_equal(result.retry_after_ms, INT_MAX);
    talloc_free(tmp);
    whisper_translator_destroy(&translator);
    destroy_transport(&transport);
}

static void test_generic_cooldown(void)
{
    struct fake_clock clock;
    init_clock(&clock, 7000, 0);
    struct fake_transport transport;
    init_transport(&transport);
    add_text_plan(&transport, 404, NULL, NULL);
    struct whisper_translator *translator = create_translator(
        WT_PROVIDER_AZURE, "auto", "ko", &transport, &clock);
    void *tmp = talloc_new(NULL);
    struct wt_call_result result;
    whisper_translate_call(translator, tmp, "cue", &result);
    assert_string_equal(result.error, "azure: HTTP 404");
    assert_true(result.http_issued);
    whisper_translate_call(translator, tmp, "next", &result);
    assert_string_equal(result.error, "azure: cooling down");
    assert_false(result.http_issued);
    assert_false(result.rate_limited);
    assert_int_equal(result.retry_after_ms, 30000);
    assert_int_equal(transport.calls, 1);
    talloc_free(tmp);
    whisper_translator_destroy(&translator);
    destroy_transport(&transport);

    init_transport(&transport);
    add_text_plan(&transport, 503, "9", NULL);
    translator = create_translator(
        WT_PROVIDER_GOOGLE, "auto", "ko", &transport, &clock);
    tmp = talloc_new(NULL);
    whisper_translate_call(translator, tmp, "cue", &result);
    assert_string_equal(result.error, "google: HTTP 503");
    whisper_translate_call(translator, tmp, "next", &result);
    assert_string_equal(result.error, "google: cooling down");
    assert_false(result.rate_limited);
    assert_int_equal(result.retry_after_ms, 9000);
    assert_int_equal(transport.calls, 1);
    talloc_free(tmp);
    whisper_translator_destroy(&translator);
    destroy_transport(&transport);
}

static void test_exponential_failure_cooldown(void)
{
    struct fake_clock clock;
    init_clock(&clock, 7500, 0);
    struct fake_transport transport;
    init_transport(&transport);
    for (int n = 0; n < 5; n++) {
        add_plan(&transport, WT_HTTP_FAILURE_READ, true, 200,
                 NULL, NULL, 0, false);
    }
    struct whisper_translator *translator = create_translator(
        WT_PROVIDER_GOOGLE, "auto", "ko", &transport, &clock);
    void *tmp = talloc_new(NULL);
    struct wt_call_result result;
    for (int n = 0; n < 5; n++) {
        whisper_translate_call(translator, tmp, "cue", &result);
        assert_true(result.http_issued);
    }
    whisper_translate_call(translator, tmp, "next", &result);
    assert_false(result.http_issued);
    assert_false(result.rate_limited);
    assert_string_equal(result.error, "google: cooling down");
    assert_int_equal(result.retry_after_ms, 5000);
    assert_int_equal(transport.calls, 5);
    struct wt_status status;
    whisper_translator_get_status(translator, &status);
    assert_int_equal(status.fail_count, 5);
    talloc_free(tmp);
    whisper_translator_destroy(&translator);
    destroy_transport(&transport);
}

static void test_success_does_not_erase_newer_rate_limit(void)
{
    struct fake_clock clock;
    init_clock(&clock, 8000, 0);
    struct fake_transport transport;
    init_transport(&transport);
    add_plan(
        &transport, WT_HTTP_FAILURE_NONE, true, 200, NULL,
        "{\"sentences\":[{\"trans\":\"ok\"}]}",
        strlen("{\"sentences\":[{\"trans\":\"ok\"}]}"), true);
    add_text_plan(&transport, 429, "120", NULL);
    struct whisper_translator *translator = create_translator(
        WT_PROVIDER_GOOGLE, "auto", "ko", &transport, &clock);

    struct call_thread slow_success = {
        .translator = translator,
        .text = "slow",
    };
    struct call_thread rate_limited = {
        .translator = translator,
        .text = "limited",
    };
    mp_thread first;
    mp_thread second;
    assert_int_equal(
        mp_thread_create(&first, call_translate, &slow_success), 0);
    wait_for_calls(&transport, 1);
    assert_int_equal(
        mp_thread_create(&second, call_translate, &rate_limited), 0);
    mp_thread_join(second);
    release_call(&transport, 0);
    mp_thread_join(first);

    assert_string_equal(slow_success.result.translated, "ok");
    assert_string_equal(rate_limited.result.error, "google: HTTP 429");
    struct wt_call_result local;
    void *tmp = talloc_new(NULL);
    whisper_translate_call(translator, tmp, "after", &local);
    assert_string_equal(local.error, "google: rate limited");
    assert_false(local.http_issued);
    assert_int_equal(local.retry_after_ms, 120000);
    assert_int_equal(transport.calls, 2);

    talloc_free(tmp);
    destroy_call(&slow_success);
    destroy_call(&rate_limited);
    whisper_translator_destroy(&translator);
    destroy_transport(&transport);
}

static void test_shorter_retry_after_cannot_reduce_deadline(void)
{
    struct fake_clock clock;
    init_clock(&clock, 9000, 0);
    struct fake_transport transport;
    init_transport(&transport);
    add_plan(&transport, WT_HTTP_FAILURE_NONE, true, 429, "1200",
             NULL, 0, true);
    add_plan(&transport, WT_HTTP_FAILURE_NONE, true, 429, "10",
             NULL, 0, true);
    struct whisper_translator *translator = create_translator(
        WT_PROVIDER_GOOGLE, "auto", "ko", &transport, &clock);
    struct call_thread long_delay = {
        .translator = translator,
        .text = "long",
    };
    struct call_thread short_delay = {
        .translator = translator,
        .text = "short",
    };
    mp_thread first;
    mp_thread second;
    assert_int_equal(
        mp_thread_create(&first, call_translate, &long_delay), 0);
    wait_for_calls(&transport, 1);
    assert_int_equal(
        mp_thread_create(&second, call_translate, &short_delay), 0);
    wait_for_calls(&transport, 2);
    release_call(&transport, 0);
    mp_thread_join(first);
    release_call(&transport, 1);
    mp_thread_join(second);

    void *tmp = talloc_new(NULL);
    struct wt_call_result local;
    whisper_translate_call(translator, tmp, "after", &local);
    assert_false(local.http_issued);
    assert_int_equal(local.retry_after_ms, 1200000);
    assert_int_equal(transport.calls, 2);

    talloc_free(tmp);
    destroy_call(&long_delay);
    destroy_call(&short_delay);
    whisper_translator_destroy(&translator);
    destroy_transport(&transport);
}

static void test_single_probe_after_expiry(void)
{
    struct fake_clock clock;
    init_clock(&clock, 10000, 0);
    struct fake_transport transport;
    init_transport(&transport);
    add_text_plan(&transport, 429, "1", NULL);
    add_plan(
        &transport, WT_HTTP_FAILURE_NONE, true, 200, NULL,
        "{\"sentences\":[{\"trans\":\"ok\"}]}",
        strlen("{\"sentences\":[{\"trans\":\"ok\"}]}"), true);
    add_text_plan(
        &transport, 200, NULL,
        "{\"sentences\":[{\"trans\":\"next\"}]}");
    struct whisper_translator *translator = create_translator(
        WT_PROVIDER_GOOGLE, "auto", "ko", &transport, &clock);

    void *tmp = talloc_new(NULL);
    struct wt_call_result initial;
    whisper_translate_call(translator, tmp, "initial", &initial);
    set_monotonic_ms(&clock, 11001);

    struct call_thread probe = {
        .translator = translator,
        .text = "probe",
    };
    mp_thread thread;
    assert_int_equal(mp_thread_create(&thread, call_translate, &probe), 0);
    wait_for_calls(&transport, 2);

    struct wt_call_result concurrent;
    whisper_translate_call(translator, tmp, "concurrent", &concurrent);
    assert_false(concurrent.http_issued);
    assert_true(concurrent.rate_limited);
    assert_string_equal(concurrent.error, "google: rate limited");
    assert_int_equal(transport.calls, 2);

    release_call(&transport, 1);
    mp_thread_join(thread);
    assert_string_equal(probe.result.translated, "ok");

    struct wt_call_result after;
    whisper_translate_call(translator, tmp, "after", &after);
    assert_string_equal(after.translated, "next");
    assert_int_equal(transport.calls, 3);

    talloc_free(tmp);
    destroy_call(&probe);
    whisper_translator_destroy(&translator);
    destroy_transport(&transport);
}

int main(void)
{
    test_google_request_and_response();
    test_google_strict_response_validation();
    test_bing_request_and_response();
    test_bing_strict_response_validation();
    test_openai_strict_response_validation();
    test_input_bounds_and_languages();
    test_transport_failures();
    test_common_rate_limit();
    test_retry_after_date_default_and_saturation();
    test_generic_cooldown();
    test_exponential_failure_cooldown();
    test_success_does_not_erase_newer_rate_limit();
    test_shorter_retry_after_cannot_reduce_deadline();
    test_single_probe_after_expiry();
    return 0;
}
