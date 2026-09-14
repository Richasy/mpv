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

#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdatomic.h>
#include <limits.h>

#include <windows.h>
#include <winhttp.h>

#include <mpv/client.h>

#include "mpv_talloc.h"
#include "common/msg.h"
#include "misc/bstr.h"
#include "misc/json.h"
#include "misc/node.h"
#include "osdep/threads.h"
#include "whisper_translate.h"
#include "whisper_translate_test.h"

// --- struct definition ---

#define WT_DEFAULT_TIMEOUT_MS     5000
#define WT_MAX_TIMEOUT_MS         5000
#define WT_DEFAULT_MAX_TOKENS     128
#define WT_BACKOFF_FAIL_THRESHOLD 5
#define WT_BACKOFF_BASE_MS        5000
#define WT_BACKOFF_MAX_MS         60000
#define WT_CHALLENGE_BACKOFF_MS   30000
#define WT_RATE_LIMIT_BACKOFF_MS  60000
#define WT_PROBE_WAIT_MS          1000
#define WT_MAX_INPUT_CODEPOINTS   5000
#define WT_MAX_RESPONSE_BYTES     (1024 * 1024)
#define WT_EXTERNAL_USER_AGENT    "mpv-subtitle-translation/1.0"
#define WT_OPENAI_USER_AGENT      "mpv-whisper/1.0"

enum wt_cooldown_kind {
    WT_COOLDOWN_NONE,
    WT_COOLDOWN_GENERIC,
    WT_COOLDOWN_RATE_LIMIT,
};

struct whisper_translator {
    struct mp_log *log;
    enum wt_provider provider;
    char *source_lang;
    char *target_lang;
    HINTERNET session;          // shared session for google/azure (default proxy)
    HINTERNET session_noproxy;  // dedicated NO_PROXY session for openai (loopback safe)

    // OpenAI-only (immutable after init)
    char *oa_scheme;            // "http" | "https"
    char *oa_host;              // hostname or IP (literal, no brackets)
    int   oa_port;
    char *oa_path;              // request path including query
    bool  oa_is_secure;
    char *oa_model;
    char *oa_api_key;           // may be ""
    char *oa_system_prompt;     // already-rendered final string (or "")
    int   oa_timeout_ms;        // hard-clamped <= WT_MAX_TIMEOUT_MS at init
    int   oa_max_tokens;

    wt_http_transport_fn transport;
    void *transport_ctx;
    wt_clock_fn monotonic_ms;
    wt_clock_fn unix_ms;
    void *clock_ctx;

    // Refcount: pipeline workers acquire while a translation is in flight,
    // so destroy can't race with HTTP. >=1 means alive.
    atomic_int refcount;

    // state_lock guards everything below. HTTP calls must NEVER be performed
    // while holding this lock (release before, re-acquire after).
    mp_mutex state_lock;

    // Common: failure / backoff state
    int fail_count;
    int64_t backoff_until_ms;   // GetTickCount64 epoch
    enum wt_cooldown_kind cooldown_kind;
    uint64_t cooldown_generation;
    uint64_t next_probe_token;
    uint64_t active_probe_token;
};

// --- Helpers ---

static int64_t real_monotonic_ms(void *ctx)
{
    (void)ctx;
    return (int64_t)GetTickCount64();
}

static int64_t real_unix_ms(void *ctx)
{
    (void)ctx;
    FILETIME file_time;
    GetSystemTimeAsFileTime(&file_time);
    ULARGE_INTEGER ticks = {
        .LowPart = file_time.dwLowDateTime,
        .HighPart = file_time.dwHighDateTime,
    };
    return (int64_t)(ticks.QuadPart / 10000ULL) - 11644473600000LL;
}

static int64_t translator_monotonic_ms(struct whisper_translator *tr)
{
    return tr->monotonic_ms(tr->clock_ctx);
}

static int64_t translator_unix_ms(struct whisper_translator *tr)
{
    return tr->unix_ms(tr->clock_ctx);
}

static void set_err(struct wt_call_result *out, const char *fmt, ...)
{
    if (!out)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out->error, sizeof(out->error), fmt, ap);
    va_end(ap);
}

// Convert UTF-8 to wide string (talloc allocated)
static WCHAR *utf8_to_wide(void *talloc_ctx, const char *utf8)
{
    if (!utf8 || !utf8[0])
        return NULL;
    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                  utf8, -1, NULL, 0);
    if (len <= 0)
        return NULL;
    WCHAR *wide = talloc_array(talloc_ctx, WCHAR, len);
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                             utf8, -1, wide, len))
    {
        talloc_free(wide);
        return NULL;
    }
    return wide;
}

static bool ascii_equal_ci(const char *a, const char *b)
{
    if (!a || !b)
        return a == b;
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z')
            ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z')
            cb += 'a' - 'A';
        if (ca != cb)
            return false;
    }
    return !*a && !*b;
}

static bool valid_language_code(const char *lang)
{
    if (!lang)
        return false;
    size_t len = strlen(lang);
    if (len < 2 || len > 35)
        return false;

    int subtag_len = 0;
    bool first = true;
    for (size_t n = 0; n <= len; n++) {
        unsigned char c = (unsigned char)lang[n];
        if (c == '-' || c == '\0') {
            if (subtag_len < (first ? 2 : 1) || subtag_len > 8)
                return false;
            first = false;
            subtag_len = 0;
            continue;
        }
        if (first) {
            if (!((c >= 'A' && c <= 'Z') ||
                  (c >= 'a' && c <= 'z')))
            {
                return false;
            }
        } else if (!((c >= 'A' && c <= 'Z') ||
                     (c >= 'a' && c <= 'z') ||
                     (c >= '0' && c <= '9')))
        {
            return false;
        }
        subtag_len++;
    }
    return true;
}

enum wt_text_validation {
    WT_TEXT_VALID,
    WT_TEXT_INVALID_UTF8,
    WT_TEXT_TOO_LONG,
};

static enum wt_text_validation validate_text(const char *text,
                                             size_t max_codepoints)
{
    const unsigned char *p = (const unsigned char *)text;
    size_t count = 0;
    while (*p) {
        size_t length;
        if (*p <= 0x7f) {
            length = 1;
        } else if (*p >= 0xc2 && *p <= 0xdf &&
                   p[1] >= 0x80 && p[1] <= 0xbf)
        {
            length = 2;
        } else if (*p == 0xe0 &&
                   p[1] >= 0xa0 && p[1] <= 0xbf &&
                   p[2] >= 0x80 && p[2] <= 0xbf)
        {
            length = 3;
        } else if (((*p >= 0xe1 && *p <= 0xec) ||
                    (*p >= 0xee && *p <= 0xef)) &&
                   p[1] >= 0x80 && p[1] <= 0xbf &&
                   p[2] >= 0x80 && p[2] <= 0xbf)
        {
            length = 3;
        } else if (*p == 0xed &&
                   p[1] >= 0x80 && p[1] <= 0x9f &&
                   p[2] >= 0x80 && p[2] <= 0xbf)
        {
            length = 3;
        } else if (*p == 0xf0 &&
                   p[1] >= 0x90 && p[1] <= 0xbf &&
                   p[2] >= 0x80 && p[2] <= 0xbf &&
                   p[3] >= 0x80 && p[3] <= 0xbf)
        {
            length = 4;
        } else if (*p >= 0xf1 && *p <= 0xf3 &&
                   p[1] >= 0x80 && p[1] <= 0xbf &&
                   p[2] >= 0x80 && p[2] <= 0xbf &&
                   p[3] >= 0x80 && p[3] <= 0xbf)
        {
            length = 4;
        } else if (*p == 0xf4 &&
                   p[1] >= 0x80 && p[1] <= 0x8f &&
                   p[2] >= 0x80 && p[2] <= 0xbf &&
                   p[3] >= 0x80 && p[3] <= 0xbf)
        {
            length = 4;
        } else {
            return WT_TEXT_INVALID_UTF8;
        }
        p += length;
        if (++count > max_codepoints)
            return WT_TEXT_TOO_LONG;
    }
    return WT_TEXT_VALID;
}

// application/x-www-form-urlencoded encoding of a UTF-8 string.
static char *form_encode(void *talloc_ctx, const char *src)
{
    if (!src)
        return NULL;
    size_t src_len = strlen(src);
    if (src_len > (SIZE_MAX - 1) / 3)
        return NULL;
    char *buf = talloc_array(talloc_ctx, char, src_len * 3 + 1);
    char *dst = buf;
    for (size_t i = 0; i < src_len; i++) {
        unsigned char c = (unsigned char)src[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
        {
            *dst++ = c;
        } else if (c == ' ') {
            *dst++ = '+';
        } else {
            snprintf(dst, 4, "%%%02X", c);
            dst += 3;
        }
    }
    *dst = '\0';
    return buf;
}

static int saturated_int64_to_int(int64_t value)
{
    if (value <= 0)
        return 0;
    return value > INT_MAX ? INT_MAX : (int)value;
}

static int64_t saturated_add_ms(int64_t now, int64_t delay)
{
    if (delay <= 0)
        return now;
    if (now > INT64_MAX - delay)
        return INT64_MAX;
    return now + delay;
}

static const char *provider_name(enum wt_provider provider)
{
    switch (provider) {
    case WT_PROVIDER_GOOGLE: return "google";
    case WT_PROVIDER_AZURE: return "azure";
    case WT_PROVIDER_OPENAI: return "openai";
    default: return "translation";
    }
}

// --- URL parser ---
//
// Supports:  http://host[:port]/path[?query]
//            https://host[:port]/path[?query]
//            http://[::1]:11434/v1/chat/completions
// Stores results as talloc-children of `parent`.

static bool parse_endpoint_url(void *parent, const char *url,
                               char **out_scheme, char **out_host,
                               int *out_port, char **out_path,
                               bool *out_secure)
{
    if (!url || !url[0])
        return false;
    const char *p = url;
    bool secure;
    if (strncmp(p, "https://", 8) == 0) {
        secure = true;
        p += 8;
    } else if (strncmp(p, "http://", 7) == 0) {
        secure = false;
        p += 7;
    } else {
        return false;
    }

    const char *host_start;
    const char *host_end;
    int port = secure ? 443 : 80;

    if (*p == '[') {
        // IPv6 literal: [::1]
        host_start = p + 1;
        const char *close = strchr(p, ']');
        if (!close)
            return false;
        host_end = close;
        p = close + 1;
        if (*p == ':') {
            port = atoi(p + 1);
            while (*p && *p != '/' && *p != '?')
                p++;
        }
    } else {
        host_start = p;
        const char *colon = NULL;
        while (*p && *p != '/' && *p != '?') {
            if (*p == ':' && !colon)
                colon = p;
            p++;
        }
        if (colon) {
            host_end = colon;
            port = atoi(colon + 1);
        } else {
            host_end = p;
        }
    }

    if (host_end == host_start)
        return false;
    if (port <= 0 || port > 65535)
        return false;

    const char *path_start = p;
    if (*path_start == '\0' || *path_start == '?') {
        // No explicit path; build "/" + (optional ?query)
        char *path = talloc_strdup(parent, "/");
        if (*path_start == '?')
            path = talloc_asprintf_append(path, "%s", path_start);
        *out_path = path;
    } else {
        *out_path = talloc_strdup(parent, path_start);
    }

    *out_scheme = talloc_strdup(parent, secure ? "https" : "http");
    *out_host   = talloc_strndup(parent, host_start, host_end - host_start);
    *out_port   = port;
    *out_secure = secure;
    return true;
}

// --- HTTP request helpers ---

static bool whisper_translator_release_internal(
    struct whisper_translator **tr);

static atomic_int active_async_requests;

enum wt_async_signal {
    WT_ASYNC_SIGNAL_NONE,
    WT_ASYNC_SIGNAL_SEND_COMPLETE,
    WT_ASYNC_SIGNAL_HEADERS_AVAILABLE,
    WT_ASYNC_SIGNAL_DATA_AVAILABLE,
    WT_ASYNC_SIGNAL_READ_COMPLETE,
    WT_ASYNC_SIGNAL_REQUEST_ERROR,
};

struct wt_test_finalization_receipt {
    atomic_int refs;
    HANDLE finalized_event;
    atomic_int finalized;
    atomic_int closing_notifications;
    atomic_int closing_handle_mismatch;
    atomic_uint closing_thread_id;
    atomic_uint finalizer_thread_id;
    atomic_int connection_close_count;
    atomic_int translator_destroy_count;
};

struct wt_async_request {
    struct whisper_translator *translator;
    CRITICAL_SECTION lock;
    HANDLE operation_event;
    HINTERNET connection;
    HINTERNET expected_request;
    struct wt_test_finalization_receipt *receipt;
    struct wt_async_request *next_cleanup;
    volatile LONG owner_active;
    volatile LONG callbacks_active;
    volatile LONG cleanup_submitted;
    bool logical_finished;
    bool issued;
    enum wt_http_failure failure;
    enum wt_http_failure pending_failure;
    enum wt_async_signal signal;
    DWORD request_error;
    DWORD data_available;
    DWORD read_complete;
    int http_status;
    char *retry_after;
    unsigned char *request_body;
    size_t request_body_len;
    unsigned char io_buffer[8192];
    unsigned char *body;
    size_t body_len;
    size_t body_capacity;
    bool has_content_length;
    DWORD content_length;
    int64_t deadline_ms;
};

struct wt_async_cleanup_service {
    INIT_ONCE once;
    CRITICAL_SECTION lock;
    CONDITION_VARIABLE condition;
    struct wt_async_request *head;
    struct wt_async_request *tail;
};

static struct wt_async_cleanup_service async_cleanup_service = {
    .once = INIT_ONCE_STATIC_INIT,
};
static atomic_int injected_cleanup_thread_create_failures;
static const unsigned char async_cleanup_module_anchor;

static struct wt_test_finalization_receipt *receipt_create(void)
{
    struct wt_test_finalization_receipt *receipt =
        calloc(1, sizeof(*receipt));
    if (!receipt)
        abort();
    atomic_init(&receipt->refs, 1);
    atomic_init(&receipt->finalized, 0);
    atomic_init(&receipt->closing_notifications, 0);
    atomic_init(&receipt->closing_handle_mismatch, 0);
    atomic_init(&receipt->closing_thread_id, 0);
    atomic_init(&receipt->finalizer_thread_id, 0);
    atomic_init(&receipt->connection_close_count, 0);
    atomic_init(&receipt->translator_destroy_count, 0);
    receipt->finalized_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!receipt->finalized_event) {
        free(receipt);
        return NULL;
    }
    return receipt;
}

static void receipt_addref(struct wt_test_finalization_receipt *receipt)
{
    atomic_fetch_add_explicit(&receipt->refs, 1, memory_order_acq_rel);
}

void whisper_translate_test_finalization_release(
    struct wt_test_finalization_receipt **receipt)
{
    if (!receipt || !*receipt)
        return;
    struct wt_test_finalization_receipt *value = *receipt;
    *receipt = NULL;
    if (atomic_fetch_sub_explicit(
            &value->refs, 1, memory_order_acq_rel) == 1)
    {
        CloseHandle(value->finalized_event);
        free(value);
    }
}

bool whisper_translate_test_finalization_wait(
    struct wt_test_finalization_receipt *receipt, int timeout_ms,
    struct wt_test_finalization_status *out)
{
    if (out)
        *out = (struct wt_test_finalization_status){0};
    if (!receipt || timeout_ms < 0)
        return false;
    DWORD result = WaitForSingleObject(
        receipt->finalized_event, (DWORD)timeout_ms);
    if (result != WAIT_OBJECT_0)
        return false;
    if (out) {
        out->finalized = atomic_load_explicit(
            &receipt->finalized, memory_order_acquire) != 0;
        out->closing_notifications = atomic_load_explicit(
            &receipt->closing_notifications, memory_order_acquire);
        out->closing_handle_mismatch = atomic_load_explicit(
            &receipt->closing_handle_mismatch, memory_order_acquire) != 0;
        out->closing_thread_id = atomic_load_explicit(
            &receipt->closing_thread_id, memory_order_acquire);
        out->finalizer_thread_id = atomic_load_explicit(
            &receipt->finalizer_thread_id, memory_order_acquire);
        out->connection_close_count = atomic_load_explicit(
            &receipt->connection_close_count, memory_order_acquire);
        out->translator_destroy_count = atomic_load_explicit(
            &receipt->translator_destroy_count, memory_order_acquire);
    }
    return true;
}

static void async_finalize_resources(struct wt_async_request *ctx)
{
    struct wt_test_finalization_receipt *receipt = ctx->receipt;
    atomic_store_explicit(
        &receipt->finalizer_thread_id, GetCurrentThreadId(),
        memory_order_release);

    if (ctx->connection) {
        WinHttpCloseHandle(ctx->connection);
        ctx->connection = NULL;
        atomic_fetch_add_explicit(
            &receipt->connection_close_count, 1, memory_order_acq_rel);
    }
    if (whisper_translator_release_internal(&ctx->translator)) {
        atomic_fetch_add_explicit(
            &receipt->translator_destroy_count, 1, memory_order_acq_rel);
    }
    if (ctx->operation_event)
        CloseHandle(ctx->operation_event);
    free(ctx->retry_after);
    free(ctx->request_body);
    free(ctx->body);
    DeleteCriticalSection(&ctx->lock);
    free(ctx);

    atomic_fetch_sub_explicit(
        &active_async_requests, 1, memory_order_acq_rel);
    atomic_store_explicit(&receipt->finalized, 1, memory_order_release);
    SetEvent(receipt->finalized_event);
    whisper_translate_test_finalization_release(&receipt);
}

static MP_THREAD_VOID async_cleanup_thread(void *opaque)
{
    struct wt_async_cleanup_service *service = opaque;
    while (true) {
        EnterCriticalSection(&service->lock);
        while (!service->head) {
            SleepConditionVariableCS(
                &service->condition, &service->lock, INFINITE);
        }
        struct wt_async_request *ctx = service->head;
        service->head = ctx->next_cleanup;
        if (!service->head)
            service->tail = NULL;
        LeaveCriticalSection(&service->lock);

        while (InterlockedCompareExchange(
                   &ctx->owner_active, 0, 0) != 0 ||
               InterlockedCompareExchange(
                   &ctx->callbacks_active, 0, 0) != 0)
        {
            SwitchToThread();
        }
        async_finalize_resources(ctx);
    }
    MP_THREAD_RETURN();
}

static bool pin_async_cleanup_module(void)
{
    HMODULE module = NULL;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (const wchar_t *)&async_cleanup_module_anchor,
            &module))
    {
        return false;
    }
    if (module == GetModuleHandleW(NULL))
        return true;
    return GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_PIN,
        (const wchar_t *)&async_cleanup_module_anchor,
        &module);
}

static BOOL CALLBACK async_cleanup_initialize(
    PINIT_ONCE once, void *parameter, void **context)
{
    (void)once;
    (void)parameter;
    (void)context;
    if (!pin_async_cleanup_module())
        return FALSE;
    InitializeCriticalSection(&async_cleanup_service.lock);
    InitializeConditionVariable(&async_cleanup_service.condition);
    if (atomic_exchange_explicit(
            &injected_cleanup_thread_create_failures, 0,
            memory_order_acq_rel) > 0)
    {
        DeleteCriticalSection(&async_cleanup_service.lock);
        return FALSE;
    }
    HANDLE thread = (HANDLE)_beginthreadex(
        NULL, 0, async_cleanup_thread,
        &async_cleanup_service, 0, NULL);
    if (!thread) {
        DeleteCriticalSection(&async_cleanup_service.lock);
        return FALSE;
    }
    CloseHandle(thread);
    return TRUE;
}

static bool async_cleanup_ensure(void)
{
    return InitOnceExecuteOnce(
        &async_cleanup_service.once,
        async_cleanup_initialize, NULL, NULL);
}

void whisper_translate_test_fail_next_cleanup_thread_create(void)
{
    atomic_store_explicit(
        &injected_cleanup_thread_create_failures, 1,
        memory_order_release);
}

bool whisper_translate_test_start_cleanup_service(void)
{
    return async_cleanup_ensure();
}

static void async_cleanup_enqueue(struct wt_async_request *ctx)
{
    EnterCriticalSection(&async_cleanup_service.lock);
    if (async_cleanup_service.tail) {
        async_cleanup_service.tail->next_cleanup = ctx;
    } else {
        async_cleanup_service.head = ctx;
    }
    async_cleanup_service.tail = ctx;
    WakeConditionVariable(&async_cleanup_service.condition);
    LeaveCriticalSection(&async_cleanup_service.lock);
}

static struct wt_async_request *async_request_create(
    struct whisper_translator *tr, HINTERNET connection,
    HINTERNET request, const struct wt_http_request *source,
    int64_t deadline_ms,
    struct wt_test_finalization_receipt **out_receipt)
{
    if (!async_cleanup_ensure())
        return NULL;
    struct wt_async_request *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        abort();
    atomic_fetch_add_explicit(
        &active_async_requests, 1, memory_order_acq_rel);
    ctx->translator = whisper_translator_acquire(tr);
    ctx->connection = connection;
    ctx->expected_request = request;
    ctx->failure = WT_HTTP_FAILURE_SETUP;
    ctx->deadline_ms = deadline_ms;
    ctx->owner_active = 1;
    InitializeCriticalSection(&ctx->lock);
    ctx->operation_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    ctx->receipt = receipt_create();
    if (source->body_len) {
        ctx->request_body = malloc(source->body_len);
        if (!ctx->request_body)
            abort();
        memcpy(ctx->request_body, source->body, source->body_len);
        ctx->request_body_len = source->body_len;
    }
    if (!ctx->operation_event || !ctx->receipt) {
        if (ctx->operation_event)
            CloseHandle(ctx->operation_event);
        if (ctx->receipt) {
            whisper_translate_test_finalization_release(
                &ctx->receipt);
        }
        free(ctx->request_body);
        DeleteCriticalSection(&ctx->lock);
        whisper_translator_release(&ctx->translator);
        atomic_fetch_sub_explicit(
            &active_async_requests, 1, memory_order_acq_rel);
        free(ctx);
        return NULL;
    }
    if (out_receipt) {
        receipt_addref(ctx->receipt);
        *out_receipt = ctx->receipt;
    }
    return ctx;
}

static void async_finish_locked(struct wt_async_request *ctx,
                                enum wt_http_failure failure)
{
    ctx->logical_finished = true;
    ctx->failure = failure;
}

static void async_prepare_operation(
    struct wt_async_request *ctx,
    enum wt_http_failure pending_failure)
{
    ResetEvent(ctx->operation_event);
    EnterCriticalSection(&ctx->lock);
    ctx->signal = WT_ASYNC_SIGNAL_NONE;
    ctx->request_error = ERROR_SUCCESS;
    ctx->data_available = 0;
    ctx->read_complete = 0;
    ctx->pending_failure = pending_failure;
    LeaveCriticalSection(&ctx->lock);
}

static bool async_start_operation(
    struct wt_async_request *ctx,
    enum wt_http_failure failure, BOOL started)
{
    if (started)
        return true;
    DWORD error = GetLastError();
    if (error == ERROR_IO_PENDING)
        return true;
    EnterCriticalSection(&ctx->lock);
    async_finish_locked(ctx, failure);
    ctx->request_error = error;
    LeaveCriticalSection(&ctx->lock);
    return false;
}

static DWORD remaining_timeout_ms(int64_t deadline_ms, int64_t now_ms)
{
    if (deadline_ms <= now_ms)
        return 0;
    int64_t remaining = deadline_ms - now_ms;
    return remaining > MAXDWORD ? MAXDWORD : (DWORD)remaining;
}

static bool async_wait_operation(
    struct wt_async_request *ctx, enum wt_async_signal expected)
{
    DWORD wait = remaining_timeout_ms(
        ctx->deadline_ms,
        translator_monotonic_ms(ctx->translator));
    DWORD wait_result = WaitForSingleObject(ctx->operation_event, wait);
    EnterCriticalSection(&ctx->lock);
    if (wait_result != WAIT_OBJECT_0) {
        async_finish_locked(
            ctx, wait_result == WAIT_TIMEOUT
                ? WT_HTTP_FAILURE_TIMEOUT
                : ctx->pending_failure);
        LeaveCriticalSection(&ctx->lock);
        return false;
    }
    if (ctx->signal == WT_ASYNC_SIGNAL_REQUEST_ERROR) {
        async_finish_locked(ctx, ctx->pending_failure);
        LeaveCriticalSection(&ctx->lock);
        return false;
    }
    if (ctx->signal != expected) {
        async_finish_locked(ctx, ctx->pending_failure);
        LeaveCriticalSection(&ctx->lock);
        return false;
    }
    LeaveCriticalSection(&ctx->lock);
    return true;
}

static char *query_retry_after(HINTERNET request)
{
    DWORD size = 0;
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_RETRY_AFTER,
                            WINHTTP_HEADER_NAME_BY_INDEX, NULL, &size,
                            WINHTTP_NO_HEADER_INDEX) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
        !size || size > WT_MAX_RESPONSE_BYTES)
    {
        return NULL;
    }

    WCHAR *wide = calloc(1, size);
    if (!wide)
        abort();
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_RETRY_AFTER,
                             WINHTTP_HEADER_NAME_BY_INDEX, wide, &size,
                             WINHTTP_NO_HEADER_INDEX))
    {
        free(wide);
        return NULL;
    }
    int length = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, NULL, 0, NULL, NULL);
    if (length <= 0) {
        free(wide);
        return NULL;
    }
    char *value = malloc(length);
    if (!value)
        abort();
    if (!WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1,
            value, length, NULL, NULL))
    {
        free(value);
        value = NULL;
    }
    free(wide);
    return value;
}

static void CALLBACK winhttp_status_callback(
    HINTERNET handle, DWORD_PTR context, DWORD status,
    LPVOID status_info, DWORD status_info_length)
{
    struct wt_async_request *ctx =
        (struct wt_async_request *)context;
    if (!ctx)
        return;
    InterlockedIncrement(&ctx->callbacks_active);

    if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) {
        atomic_fetch_add_explicit(
            &ctx->receipt->closing_notifications, 1,
            memory_order_acq_rel);
        if (handle != ctx->expected_request) {
            atomic_store_explicit(
                &ctx->receipt->closing_handle_mismatch, 1,
                memory_order_release);
        }
        atomic_store_explicit(
            &ctx->receipt->closing_thread_id, GetCurrentThreadId(),
            memory_order_release);
        if (InterlockedCompareExchange(
                &ctx->cleanup_submitted, 1, 0) == 0)
        {
            async_cleanup_enqueue(ctx);
        }
        InterlockedDecrement(&ctx->callbacks_active);
        return;
    }

    EnterCriticalSection(&ctx->lock);
    if (!ctx->logical_finished) {
        switch (status) {
        case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
            ctx->signal = WT_ASYNC_SIGNAL_SEND_COMPLETE;
            SetEvent(ctx->operation_event);
            break;
        case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
            ctx->signal = WT_ASYNC_SIGNAL_HEADERS_AVAILABLE;
            SetEvent(ctx->operation_event);
            break;
        case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE:
            if (status_info &&
                status_info_length == sizeof(DWORD))
            {
                ctx->data_available = *(DWORD *)status_info;
                ctx->signal = WT_ASYNC_SIGNAL_DATA_AVAILABLE;
            } else {
                ctx->signal = WT_ASYNC_SIGNAL_REQUEST_ERROR;
            }
            SetEvent(ctx->operation_event);
            break;
        case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
            if (status_info == ctx->io_buffer &&
                status_info_length <= sizeof(ctx->io_buffer))
            {
                ctx->read_complete = status_info_length;
                ctx->signal = WT_ASYNC_SIGNAL_READ_COMPLETE;
            } else {
                ctx->signal = WT_ASYNC_SIGNAL_REQUEST_ERROR;
            }
            SetEvent(ctx->operation_event);
            break;
        case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
            if (status_info &&
                status_info_length == sizeof(WINHTTP_ASYNC_RESULT))
            {
                WINHTTP_ASYNC_RESULT *error = status_info;
                ctx->request_error = error->dwError;
            }
            ctx->signal = WT_ASYNC_SIGNAL_REQUEST_ERROR;
            SetEvent(ctx->operation_event);
            break;
        }
    }
    LeaveCriticalSection(&ctx->lock);
    InterlockedDecrement(&ctx->callbacks_active);
}

static bool async_read_response(struct wt_async_request *ctx)
{
    while (true) {
        if (translator_monotonic_ms(ctx->translator) >=
            ctx->deadline_ms)
        {
            EnterCriticalSection(&ctx->lock);
            async_finish_locked(ctx, WT_HTTP_FAILURE_TIMEOUT);
            LeaveCriticalSection(&ctx->lock);
            return false;
        }
        async_prepare_operation(ctx, WT_HTTP_FAILURE_READ);
        if (!async_start_operation(
                ctx, WT_HTTP_FAILURE_READ,
                WinHttpQueryDataAvailable(
                    ctx->expected_request, NULL)) ||
            !async_wait_operation(
                ctx, WT_ASYNC_SIGNAL_DATA_AVAILABLE))
        {
            return false;
        }

        EnterCriticalSection(&ctx->lock);
        DWORD available = ctx->data_available;
        LeaveCriticalSection(&ctx->lock);
        if (!available) {
            if (ctx->has_content_length &&
                ctx->body_len != ctx->content_length)
            {
                EnterCriticalSection(&ctx->lock);
                async_finish_locked(ctx, WT_HTTP_FAILURE_READ);
                LeaveCriticalSection(&ctx->lock);
                return false;
            }
            return true;
        }
        if (available > WT_MAX_RESPONSE_BYTES - ctx->body_len) {
            EnterCriticalSection(&ctx->lock);
            async_finish_locked(ctx, WT_HTTP_FAILURE_TOO_LARGE);
            LeaveCriticalSection(&ctx->lock);
            return false;
        }

        DWORD read_size = available < sizeof(ctx->io_buffer)
            ? available : sizeof(ctx->io_buffer);
        async_prepare_operation(ctx, WT_HTTP_FAILURE_READ);
        if (!async_start_operation(
                ctx, WT_HTTP_FAILURE_READ,
                WinHttpReadData(
                    ctx->expected_request, ctx->io_buffer,
                    read_size, NULL)) ||
            !async_wait_operation(
                ctx, WT_ASYNC_SIGNAL_READ_COMPLETE))
        {
            return false;
        }

        EnterCriticalSection(&ctx->lock);
        DWORD read = ctx->read_complete;
        LeaveCriticalSection(&ctx->lock);
        if (!read || read > read_size) {
            EnterCriticalSection(&ctx->lock);
            async_finish_locked(ctx, WT_HTTP_FAILURE_READ);
            LeaveCriticalSection(&ctx->lock);
            return false;
        }
        if (ctx->body_capacity < ctx->body_len + read + 1) {
            size_t capacity = ctx->body_capacity
                ? ctx->body_capacity : sizeof(ctx->io_buffer);
            while (capacity < ctx->body_len + read + 1)
                capacity *= 2;
            unsigned char *body = realloc(ctx->body, capacity);
            if (!body)
                abort();
            ctx->body = body;
            ctx->body_capacity = capacity;
        }
        memcpy(ctx->body + ctx->body_len, ctx->io_buffer, read);
        ctx->body_len += read;
        ctx->body[ctx->body_len] = '\0';
    }
}

static void async_copy_response(
    struct wt_async_request *ctx, void *talloc_ctx,
    struct wt_http_response *response)
{
    EnterCriticalSection(&ctx->lock);
    response->failure = ctx->failure;
    response->http_issued = ctx->issued;
    response->http_status = ctx->http_status;
    if (ctx->retry_after)
        response->retry_after =
            talloc_strdup(talloc_ctx, ctx->retry_after);
    if (ctx->body_len) {
        response->body = talloc_memdup(
            talloc_ctx, ctx->body, ctx->body_len);
        response->body_len = ctx->body_len;
    }
    LeaveCriticalSection(&ctx->lock);
}

static void async_finalize_unbound(struct wt_async_request *ctx)
{
    InterlockedExchange(&ctx->owner_active, 0);
    async_finalize_resources(ctx);
}

static void winhttp_transport_impl(
    void *opaque, void *talloc_ctx,
    const struct wt_http_request *request,
    struct wt_http_response *response,
    struct wt_test_finalization_receipt **out_receipt)
{
    struct whisper_translator *tr = opaque;
    *response = (struct wt_http_response){
        .failure = WT_HTTP_FAILURE_SETUP,
    };
    if (out_receipt)
        *out_receipt = NULL;

    int timeout_ms = request->timeout_ms > 0
        ? request->timeout_ms : WT_MAX_TIMEOUT_MS;
    int64_t started_ms = translator_monotonic_ms(tr);
    int64_t deadline_ms = saturated_add_ms(started_ms, timeout_ms);
    void *tmp = talloc_new(NULL);
    WCHAR *host = utf8_to_wide(tmp, request->host);
    WCHAR *method = utf8_to_wide(tmp, request->method);
    WCHAR *path = utf8_to_wide(tmp, request->path);
    WCHAR *headers = request->headers && request->headers[0]
        ? utf8_to_wide(tmp, request->headers) : NULL;
    HINTERNET session = request->proxy_mode == WT_HTTP_PROXY_NONE
        ? tr->session_noproxy : tr->session;
    if (!session || !host || !method || !path ||
        (request->headers && request->headers[0] && !headers) ||
        request->body_len > UINT32_MAX)
    {
        talloc_free(tmp);
        return;
    }

    HINTERNET connection = WinHttpConnect(
        session, host, request->port, 0);
    if (!connection) {
        mp_warn(tr->log, "translate: WinHttpConnect failed (%lu)\n",
                GetLastError());
        talloc_free(tmp);
        return;
    }
    DWORD flags = request->secure ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET handle = WinHttpOpenRequest(
        connection, method, path, NULL, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!handle) {
        mp_warn(tr->log, "translate: WinHttpOpenRequest failed (%lu)\n",
                GetLastError());
        WinHttpCloseHandle(connection);
        talloc_free(tmp);
        return;
    }

    struct wt_async_request *ctx = async_request_create(
        tr, connection, handle, request, deadline_ms, out_receipt);
    if (!ctx) {
        WinHttpCloseHandle(handle);
        WinHttpCloseHandle(connection);
        talloc_free(tmp);
        return;
    }

    DWORD disabled_features = WINHTTP_DISABLE_REDIRECTS;
    if (request->disable_cookies)
        disabled_features |= WINHTTP_DISABLE_COOKIES;
    bool setup_ok =
        WinHttpSetOption(handle, WINHTTP_OPTION_DISABLE_FEATURE,
                         &disabled_features, sizeof(disabled_features));
    if (setup_ok && headers) {
        setup_ok = WinHttpAddRequestHeaders(
            handle, headers, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
    }
    DWORD_PTR context = (DWORD_PTR)ctx;
    if (setup_ok) {
        setup_ok = WinHttpSetOption(
            handle, WINHTTP_OPTION_CONTEXT_VALUE,
            &context, sizeof(context));
    }

    DWORD callback_flags =
        WINHTTP_CALLBACK_FLAG_SENDREQUEST_COMPLETE |
        WINHTTP_CALLBACK_FLAG_HEADERS_AVAILABLE |
        WINHTTP_CALLBACK_FLAG_DATA_AVAILABLE |
        WINHTTP_CALLBACK_FLAG_READ_COMPLETE |
        WINHTTP_CALLBACK_FLAG_REQUEST_ERROR |
        WINHTTP_CALLBACK_FLAG_HANDLES;
    if (!setup_ok ||
        WinHttpSetStatusCallback(
            handle, winhttp_status_callback, callback_flags, 0) ==
        WINHTTP_INVALID_STATUS_CALLBACK)
    {
        response->failure = WT_HTTP_FAILURE_SETUP;
        WinHttpCloseHandle(handle);
        async_finalize_unbound(ctx);
        talloc_free(tmp);
        return;
    }

    async_prepare_operation(ctx, WT_HTTP_FAILURE_SEND);
    ctx->issued = true;
    bool complete = async_start_operation(
        ctx, WT_HTTP_FAILURE_SEND,
        WinHttpSendRequest(
            handle, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            ctx->request_body_len
                ? ctx->request_body : WINHTTP_NO_REQUEST_DATA,
            (DWORD)ctx->request_body_len,
            (DWORD)ctx->request_body_len,
            (DWORD_PTR)ctx)) &&
        async_wait_operation(
            ctx, WT_ASYNC_SIGNAL_SEND_COMPLETE);

    if (complete) {
        async_prepare_operation(ctx, WT_HTTP_FAILURE_RECEIVE);
        complete = async_start_operation(
            ctx, WT_HTTP_FAILURE_RECEIVE,
            WinHttpReceiveResponse(handle, NULL)) &&
            async_wait_operation(
                ctx, WT_ASYNC_SIGNAL_HEADERS_AVAILABLE);
    }

    if (complete) {
        DWORD status_code = 0;
        DWORD status_size = sizeof(status_code);
        if (!WinHttpQueryHeaders(
                handle,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status_code,
                &status_size, WINHTTP_NO_HEADER_INDEX))
        {
            EnterCriticalSection(&ctx->lock);
            async_finish_locked(ctx, WT_HTTP_FAILURE_STATUS);
            LeaveCriticalSection(&ctx->lock);
            complete = false;
        } else {
            ctx->http_status = (int)status_code;
            ctx->retry_after = query_retry_after(handle);
            DWORD length_size = sizeof(ctx->content_length);
            ctx->has_content_length = WinHttpQueryHeaders(
                handle,
                WINHTTP_QUERY_CONTENT_LENGTH |
                    WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX,
                &ctx->content_length, &length_size,
                WINHTTP_NO_HEADER_INDEX);
            if (ctx->has_content_length &&
                ctx->content_length > WT_MAX_RESPONSE_BYTES)
            {
                EnterCriticalSection(&ctx->lock);
                async_finish_locked(
                    ctx, WT_HTTP_FAILURE_TOO_LARGE);
                LeaveCriticalSection(&ctx->lock);
                complete = false;
            } else if (status_code == 200) {
                complete = async_read_response(ctx);
            }
        }
    }

    EnterCriticalSection(&ctx->lock);
    if (!ctx->logical_finished) {
        async_finish_locked(
            ctx, complete
                ? WT_HTTP_FAILURE_NONE : ctx->failure);
    }
    LeaveCriticalSection(&ctx->lock);
    async_copy_response(ctx, talloc_ctx, response);

    // After close, HANDLE_CLOSING owns the transition to the non-WinHTTP
    // finalizer. The initiating owner publishes its drain as its final access.
    if (!WinHttpCloseHandle(handle)) {
        mp_warn(tr->log, "translate: WinHttpCloseHandle failed (%lu)\n",
                GetLastError());
    }
    InterlockedExchange(&ctx->owner_active, 0);
    talloc_free(tmp);
}

static void winhttp_transport(
    void *opaque, void *talloc_ctx,
    const struct wt_http_request *request,
    struct wt_http_response *response)
{
    winhttp_transport_impl(
        opaque, talloc_ctx, request, response, NULL);
}


struct wt_retry_after {
    bool present;
    bool valid;
    int64_t delay_ms;
};

static struct wt_retry_after parse_retry_after(
    struct whisper_translator *tr, const char *value)
{
    if (!value)
        return (struct wt_retry_after){0};
    struct wt_retry_after parsed = {.present = true};
    while (*value == ' ' || *value == '\t')
        value++;
    size_t len = strlen(value);
    while (len && (value[len - 1] == ' ' || value[len - 1] == '\t'))
        len--;
    if (!len)
        return parsed;

    bool digits = true;
    int64_t seconds = 0;
    for (size_t n = 0; n < len; n++) {
        unsigned char c = (unsigned char)value[n];
        if (c < '0' || c > '9') {
            digits = false;
            break;
        }
        int digit = c - '0';
        if (seconds > (INT64_MAX - digit) / 10) {
            seconds = INT64_MAX;
        } else if (seconds != INT64_MAX) {
            seconds = seconds * 10 + digit;
        }
    }
    if (digits) {
        parsed.valid = true;
        if (seconds > INT64_MAX / 1000)
            parsed.delay_ms = INT64_MAX;
        else
            parsed.delay_ms = seconds * 1000;
        return parsed;
    }

    if (len >= 128)
        return parsed;
    WCHAR wide[128];
    for (size_t n = 0; n < len; n++) {
        unsigned char c = (unsigned char)value[n];
        if (c > 0x7f)
            return parsed;
        wide[n] = c;
    }
    wide[len] = L'\0';

    SYSTEMTIME system_time = {0};
    FILETIME file_time;
    if (!WinHttpTimeToSystemTime(wide, &system_time) ||
        !SystemTimeToFileTime(&system_time, &file_time))
    {
        return parsed;
    }
    ULARGE_INTEGER ticks = {
        .LowPart = file_time.dwLowDateTime,
        .HighPart = file_time.dwHighDateTime,
    };
    int64_t target_ms =
        (int64_t)(ticks.QuadPart / 10000ULL) - 11644473600000LL;
    int64_t now_ms = translator_unix_ms(tr);
    parsed.valid = true;
    parsed.delay_ms = target_ms > now_ms ? target_ms - now_ms : 0;
    return parsed;
}

static bool json_unicode_escapes_valid(const char *json)
{
    bool in_string = false;
    for (size_t n = 0; json[n]; n++) {
        if (!in_string) {
            if (json[n] == '"')
                in_string = true;
            continue;
        }
        if (json[n] == '"') {
            in_string = false;
            continue;
        }
        if (json[n] != '\\')
            continue;
        char escape = json[++n];
        if (escape != 'u')
            continue;
        unsigned value = 0;
        for (int digit = 0; digit < 4; digit++) {
            char c = json[n + 1 + digit];
            value <<= 4;
            value |= c >= '0' && c <= '9' ? c - '0'
                   : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                          : c - 'A' + 10;
        }
        n += 4;
        if (value == 0 || (value >= 0xdc00 && value <= 0xdfff))
            return false;
        if (value >= 0xd800 && value <= 0xdbff) {
            if (json[n + 1] != '\\' || json[n + 2] != 'u')
                return false;
            unsigned low = 0;
            for (int digit = 0; digit < 4; digit++) {
                char c = json[n + 3 + digit];
                low <<= 4;
                low |= c >= '0' && c <= '9' ? c - '0'
                     : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                            : c - 'A' + 10;
            }
            if (low < 0xdc00 || low > 0xdfff)
                return false;
            n += 6;
        }
    }
    return !in_string;
}

static bool node_keys_unique_recursive(struct mpv_node *node)
{
    if (!node)
        return false;
    if (node->format == MPV_FORMAT_NODE_MAP) {
        struct mpv_node_list *map = node->u.list;
        if (!map)
            return false;
        for (int n = 0; n < map->num; n++) {
            for (int k = n + 1; k < map->num; k++) {
                if (strcmp(map->keys[n], map->keys[k]) == 0)
                    return false;
            }
            if (!node_keys_unique_recursive(&map->values[n]))
                return false;
        }
    } else if (node->format == MPV_FORMAT_NODE_ARRAY) {
        struct mpv_node_list *array = node->u.list;
        if (!array)
            return false;
        for (int n = 0; n < array->num; n++) {
            if (!node_keys_unique_recursive(&array->values[n]))
                return false;
        }
    }
    return true;
}

static bool parse_json_document(void *talloc_ctx, const char *response,
                                struct mpv_node *root)
{
    if (!response ||
        validate_text(response, SIZE_MAX) != WT_TEXT_VALID ||
        !json_validate_strict(response, MAX_JSON_DEPTH) ||
        !json_unicode_escapes_valid(response))
    {
        return false;
    }
    char *cursor = talloc_strdup(talloc_ctx, response);
    if (json_parse(talloc_ctx, root, &cursor, MAX_JSON_DEPTH) < 0)
        return false;
    json_skip_whitespace(&cursor);
    return !cursor[0] && node_keys_unique_recursive(root);
}

struct wt_http_observation {
    struct wt_retry_after retry_after;
};

static bool perform_http(struct whisper_translator *tr, void *talloc_ctx,
                         const struct wt_http_request *request,
                         struct wt_call_result *out,
                         struct wt_http_observation *observation,
                         char **response_body)
{
    struct wt_http_response response = {0};
    tr->transport(tr->transport_ctx, talloc_ctx, request, &response);
    if (observation)
        observation->retry_after =
            parse_retry_after(tr, response.retry_after);
    if (out) {
        out->http_issued = response.http_issued;
        out->http_status =
            response.http_status >= 100 && response.http_status <= 599
                ? response.http_status : 0;
        out->rate_limited = response.http_status == 429;
        out->retry_after_ms = saturated_int64_to_int(
            observation && observation->retry_after.valid
                ? observation->retry_after.delay_ms : 0);
    }

    const char *name = provider_name(tr->provider);
    switch (response.failure) {
    case WT_HTTP_FAILURE_NONE:
        break;
    case WT_HTTP_FAILURE_SETUP:
        set_err(out, "%s: request setup failed", name);
        return false;
    case WT_HTTP_FAILURE_SEND:
        set_err(out, "%s: request send failed", name);
        return false;
    case WT_HTTP_FAILURE_RECEIVE:
        set_err(out, "%s: response receive failed", name);
        return false;
    case WT_HTTP_FAILURE_STATUS:
        set_err(out, "%s: invalid HTTP status", name);
        return false;
    case WT_HTTP_FAILURE_READ:
        set_err(out, "%s: response read failed", name);
        return false;
    case WT_HTTP_FAILURE_TOO_LARGE:
        set_err(out, "%s: response too large", name);
        return false;
    case WT_HTTP_FAILURE_TIMEOUT:
        set_err(out, "%s: request timed out", name);
        return false;
    }

    if (response.http_status < 100 || response.http_status > 599) {
        if (out)
            out->http_status = 0;
        set_err(out, "%s: invalid HTTP status", name);
        return false;
    }
    if (response.http_status != 200) {
        set_err(out, "%s: HTTP %d", name, response.http_status);
        return false;
    }
    if (response.body_len > WT_MAX_RESPONSE_BYTES) {
        set_err(out, "%s: response too large", name);
        return false;
    }
    if ((response.body_len && !response.body) ||
        (response.body &&
         memchr(response.body, '\0', response.body_len)))
    {
        set_err(out, "%s: invalid response body", name);
        return false;
    }

    char *body = talloc_array(talloc_ctx, char, response.body_len + 1);
    if (response.body_len)
        memcpy(body, response.body, response.body_len);
    body[response.body_len] = '\0';
    if (validate_text(body, SIZE_MAX) != WT_TEXT_VALID) {
        talloc_free(body);
        set_err(out, "%s: invalid response encoding", name);
        return false;
    }
    *response_body = body;
    return true;
}

// --- Google Translate ---

static const char *google_normalize_lang(const char *lang, bool source)
{
    if (!lang || !lang[0] || ascii_equal_ci(lang, "auto"))
        return source ? "auto" : NULL;
    if (!valid_language_code(lang))
        return NULL;
    if (ascii_equal_ci(lang, "zh") ||
        ascii_equal_ci(lang, "zh-CN") ||
        ascii_equal_ci(lang, "zh-Hans"))
    {
        return "zh-CN";
    }
    if (ascii_equal_ci(lang, "zh-TW") ||
        ascii_equal_ci(lang, "zh-Hant"))
    {
        return "zh-TW";
    }
    if (ascii_equal_ci(lang, "ko"))
        return "ko";
    return lang;
}

static char *google_extract_translation(void *talloc_ctx,
                                        const char *response)
{
    void *tmp = talloc_new(NULL);
    struct mpv_node root = {0};
    if (!parse_json_document(tmp, response, &root) ||
        root.format != MPV_FORMAT_NODE_MAP)
    {
        talloc_free(tmp);
        return NULL;
    }
    struct mpv_node *sentences = node_map_get(&root, "sentences");
    if (!sentences || sentences->format != MPV_FORMAT_NODE_ARRAY ||
        !sentences->u.list)
    {
        talloc_free(tmp);
        return NULL;
    }

    char *translated = talloc_strdup(talloc_ctx, "");
    bool found_nonempty = false;
    for (int n = 0; n < sentences->u.list->num; n++) {
        struct mpv_node *sentence = &sentences->u.list->values[n];
        if (sentence->format != MPV_FORMAT_NODE_MAP) {
            talloc_free(translated);
            talloc_free(tmp);
            return NULL;
        }
        struct mpv_node *trans = node_map_get(sentence, "trans");
        struct mpv_node *translit = node_map_get(sentence, "translit");
        struct mpv_node *src_translit =
            node_map_get(sentence, "src_translit");
        if ((translit && translit->format != MPV_FORMAT_STRING) ||
            (src_translit && src_translit->format != MPV_FORMAT_STRING))
        {
            talloc_free(translated);
            talloc_free(tmp);
            return NULL;
        }
        if (trans) {
            if (trans->format != MPV_FORMAT_STRING || !trans->u.string) {
                talloc_free(translated);
                talloc_free(tmp);
                return NULL;
            }
            translated = talloc_asprintf_append(
                translated, "%s", trans->u.string);
            found_nonempty |= trans->u.string[0] != '\0';
        } else if (!translit && !src_translit) {
            talloc_free(translated);
            talloc_free(tmp);
            return NULL;
        }
    }
    talloc_free(tmp);
    if (!found_nonempty) {
        talloc_free(translated);
        return NULL;
    }
    return translated;
}

static char *translate_google(struct whisper_translator *tr,
                              void *talloc_ctx, const char *text,
                              struct wt_call_result *out,
                              struct wt_http_observation *observation)
{
    const char *sl = google_normalize_lang(tr->source_lang, true);
    const char *tl = google_normalize_lang(tr->target_lang, false);
    if (!sl || !tl) {
        set_err(out, "google: invalid language");
        return NULL;
    }
    enum wt_text_validation text_valid =
        validate_text(text, WT_MAX_INPUT_CODEPOINTS);
    if (text_valid != WT_TEXT_VALID) {
        set_err(out, text_valid == WT_TEXT_TOO_LONG
                ? "google: input too long"
                : "google: invalid UTF-8");
        return NULL;
    }

    void *tmp = talloc_new(NULL);
    char *encoded_sl = form_encode(tmp, sl);
    char *encoded_tl = form_encode(tmp, tl);
    char *encoded_text = form_encode(tmp, text);
    if (!encoded_sl || !encoded_tl || !encoded_text) {
        set_err(out, "google: request build failed");
        talloc_free(tmp);
        return NULL;
    }
    char *body = talloc_asprintf(
        tmp, "sl=%s&tl=%s&q=%s", encoded_sl, encoded_tl, encoded_text);
    struct wt_http_request request = {
        .host = "translate.google.com",
        .port = INTERNET_DEFAULT_HTTPS_PORT,
        .secure = true,
        .method = "POST",
        .path = "/translate_a/single?client=at&dt=t&dt=rm&dj=1",
        .headers =
            "Content-Type: application/x-www-form-urlencoded;charset=utf-8\r\n",
        .body = body,
        .body_len = strlen(body),
        .proxy_mode = WT_HTTP_PROXY_DEFAULT,
        .disable_cookies = true,
        .timeout_ms = WT_MAX_TIMEOUT_MS,
    };
    char *response = NULL;
    if (!perform_http(tr, tmp, &request, out, observation, &response)) {
        talloc_free(tmp);
        return NULL;
    }
    char *result = google_extract_translation(talloc_ctx, response);
    if (!result) {
        set_err(out, "google: parse failed");
        mp_warn(tr->log, "translate: google response parse failed\n");
    }
    talloc_free(tmp);
    return result;
}

// --- Bing/Edge Translate (provider value remains "azure") ---

static const char *azure_normalize_lang(const char *lang, bool source)
{
    if (!lang || !lang[0] || ascii_equal_ci(lang, "auto"))
        return source ? "" : NULL;
    if (!valid_language_code(lang))
        return NULL;
    if (ascii_equal_ci(lang, "zh") ||
        ascii_equal_ci(lang, "zh-CN") ||
        ascii_equal_ci(lang, "zh-Hans"))
    {
        return "zh-Hans";
    }
    if (ascii_equal_ci(lang, "zh-TW") ||
        ascii_equal_ci(lang, "zh-Hant"))
    {
        return "zh-Hant";
    }
    if (ascii_equal_ci(lang, "ko"))
        return "ko";
    return lang;
}

static char *azure_extract_translation(void *talloc_ctx,
                                       const char *response,
                                       const char *target_lang)
{
    void *tmp = talloc_new(NULL);
    struct mpv_node root = {0};
    if (!parse_json_document(tmp, response, &root) ||
        root.format != MPV_FORMAT_NODE_ARRAY || !root.u.list ||
        root.u.list->num != 1)
    {
        talloc_free(tmp);
        return NULL;
    }
    struct mpv_node *item = &root.u.list->values[0];
    if (item->format != MPV_FORMAT_NODE_MAP) {
        talloc_free(tmp);
        return NULL;
    }
    struct mpv_node *translations = node_map_get(item, "translations");
    if (!translations || translations->format != MPV_FORMAT_NODE_ARRAY ||
        !translations->u.list)
    {
        talloc_free(tmp);
        return NULL;
    }

    const char *match = NULL;
    for (int n = 0; n < translations->u.list->num; n++) {
        struct mpv_node *translation = &translations->u.list->values[n];
        if (translation->format != MPV_FORMAT_NODE_MAP) {
            talloc_free(tmp);
            return NULL;
        }
        struct mpv_node *text = node_map_get(translation, "text");
        struct mpv_node *to = node_map_get(translation, "to");
        if (!text || text->format != MPV_FORMAT_STRING || !text->u.string ||
            !to || to->format != MPV_FORMAT_STRING || !to->u.string)
        {
            talloc_free(tmp);
            return NULL;
        }
        const char *normalized_to = azure_normalize_lang(to->u.string, false);
        if (!normalized_to) {
            talloc_free(tmp);
            return NULL;
        }
        if (ascii_equal_ci(normalized_to, target_lang)) {
            if (match) {
                talloc_free(tmp);
                return NULL;
            }
            match = text->u.string;
        }
    }
    char *result = match && match[0]
        ? talloc_strdup(talloc_ctx, match) : NULL;
    talloc_free(tmp);
    return result;
}

static char *translate_azure(struct whisper_translator *tr,
                             void *talloc_ctx, const char *text,
                             struct wt_call_result *out,
                             struct wt_http_observation *observation)
{
    const char *sl = azure_normalize_lang(tr->source_lang, true);
    const char *tl = azure_normalize_lang(tr->target_lang, false);
    if (!sl || !tl) {
        set_err(out, "azure: invalid language");
        return NULL;
    }
    enum wt_text_validation text_valid =
        validate_text(text, WT_MAX_INPUT_CODEPOINTS);
    if (text_valid != WT_TEXT_VALID) {
        set_err(out, text_valid == WT_TEXT_TOO_LONG
                ? "azure: input too long"
                : "azure: invalid UTF-8");
        return NULL;
    }

    void *tmp = talloc_new(NULL);
    char *encoded_sl = form_encode(tmp, sl);
    char *encoded_tl = form_encode(tmp, tl);
    if (!encoded_sl || !encoded_tl) {
        set_err(out, "azure: request build failed");
        talloc_free(tmp);
        return NULL;
    }

    struct mpv_node body_root;
    node_init(&body_root, MPV_FORMAT_NODE_ARRAY, NULL);
    talloc_steal(tmp, body_root.u.list);
    struct mpv_node *body_text =
        node_array_add(&body_root, MPV_FORMAT_NONE);
    body_text->format = MPV_FORMAT_STRING;
    body_text->u.string = talloc_strdup(body_root.u.list, text);
    char *serialized = NULL;
    if (json_write(&serialized, &body_root) < 0 || !serialized) {
        set_err(out, "azure: request build failed");
        talloc_free(serialized);
        talloc_free(tmp);
        return NULL;
    }
    char *body = talloc_strdup(tmp, serialized);
    talloc_free(serialized);

    char *path = talloc_asprintf(
        tmp, "/translate/translatetext?from=%s&to=%s&isEnterpriseClient=false",
        encoded_sl, encoded_tl);
    struct wt_http_request request = {
        .host = "edge.microsoft.com",
        .port = INTERNET_DEFAULT_HTTPS_PORT,
        .secure = true,
        .method = "POST",
        .path = path,
        .headers = "Content-Type: application/json; charset=utf-8\r\n",
        .body = body,
        .body_len = strlen(body),
        .proxy_mode = WT_HTTP_PROXY_DEFAULT,
        .disable_cookies = true,
        .timeout_ms = WT_MAX_TIMEOUT_MS,
    };
    char *response = NULL;
    if (!perform_http(tr, tmp, &request, out, observation, &response)) {
        talloc_free(tmp);
        return NULL;
    }

    char *result = azure_extract_translation(talloc_ctx, response, tl);
    if (!result) {
        set_err(out, "azure: parse failed");
        mp_warn(tr->log, "translate: azure response parse failed\n");
    }
    talloc_free(tmp);
    return result;
}


// --- OpenAI Translate ---

// Built-in fallback system prompt (used only when JSON system_prompt is empty).
// Single placeholder: {target_lang}.
static const char *OA_FALLBACK_PROMPT =
    "You are a professional subtitle translator. Translate the user's "
    "sentence to {target_lang}. Output ONLY the translated sentence, "
    "no quotes, no explanation, no language tag. Previous user/assistant "
    "messages (if any) are prior subtitle translations for context only; "
    "translate only the latest user message and do not continue the "
    "previous sentence.";

static char *oa_render_fallback_prompt(void *parent, const char *target_lang)
{
    const char *tl = target_lang && target_lang[0] ? target_lang : "the target language";
    char *out = talloc_strdup(parent, "");
    const char *p = OA_FALLBACK_PROMPT;
    while (*p) {
        const char *m = strstr(p, "{target_lang}");
        if (!m) {
            out = talloc_asprintf_append(out, "%s", p);
            break;
        }
        out = talloc_strndup_append(out, p, m - p);
        out = talloc_asprintf_append(out, "%s", tl);
        p = m + strlen("{target_lang}");
    }
    return out;
}

// (OpenAI history feature removed: translator is stateless to enable parallel
// in-flight requests from multiple worker threads. The `context_size` config
// field is still accepted for backward compatibility but ignored.)

// Strip pairs of leading/trailing matching quotes (ASCII " ', curly “”, 「」, 『』).
static void oa_strip_outer_quotes(char *s)
{
    if (!s)
        return;
    size_t len = strlen(s);
    if (len < 2)
        return;
    static const char *pairs[][2] = {
        {"\"", "\""}, {"'", "'"},
        {"\xe2\x80\x9c", "\xe2\x80\x9d"},   // “ ”
        {"\xe2\x80\x98", "\xe2\x80\x99"},   // ‘ ’
        {"\xe3\x80\x8c", "\xe3\x80\x8d"},   // 「 」
        {"\xe3\x80\x8e", "\xe3\x80\x8f"},   // 『 』
    };
    for (int i = 0; i < (int)(sizeof(pairs) / sizeof(pairs[0])); i++) {
        size_t lo = strlen(pairs[i][0]);
        size_t lc = strlen(pairs[i][1]);
        if (len >= lo + lc &&
            memcmp(s, pairs[i][0], lo) == 0 &&
            memcmp(s + len - lc, pairs[i][1], lc) == 0)
        {
            memmove(s, s + lo, len - lo - lc);
            s[len - lo - lc] = '\0';
            return;
        }
    }
}

// Strip common "Translation: " / "译文：" / etc. prefixes.
static void oa_strip_prefix(char *s)
{
    if (!s)
        return;
    static const char *prefixes[] = {
        "Translation:", "Translated text:", "Translated:",
        "译文：", "译文:", "翻译结果：", "翻译结果:",
        "翻译：", "翻译:", "中文：", "中文:",
        "English:", "Output:", NULL,
    };
    for (int i = 0; prefixes[i]; i++) {
        size_t plen = strlen(prefixes[i]);
        if (strncmp(s, prefixes[i], plen) == 0) {
            // Skip prefix and any trailing whitespace.
            char *p = s + plen;
            while (*p == ' ' || *p == '\t')
                p++;
            memmove(s, p, strlen(p) + 1);
            return;
        }
    }
}

// Strip <think>...</think> blocks (DeepSeek-R1 etc.). If <think> appears
// without a closing tag, drop everything from <think> onward — the model is
// streaming reasoning and the visible answer never came.
static void oa_strip_think(char *s)
{
    if (!s)
        return;
    char *open = strstr(s, "<think>");
    while (open) {
        char *close = strstr(open + 7, "</think>");
        if (close) {
            char *after = close + 8;
            memmove(open, after, strlen(after) + 1);
        } else {
            *open = '\0';
            break;
        }
        open = strstr(s, "<think>");
    }
}

// Strip common chat template residue.
static void oa_strip_template(char *s)
{
    if (!s)
        return;
    static const char *tags[] = {
        "<|assistant|>", "<|user|>", "<|system|>",
        "<|im_start|>", "<|im_end|>", NULL,
    };
    for (int i = 0; tags[i]; i++) {
        char *p;
        while ((p = strstr(s, tags[i])) != NULL) {
            size_t tlen = strlen(tags[i]);
            memmove(p, p + tlen, strlen(p + tlen) + 1);
        }
    }
}

static void oa_strip_markdown_fence(char *s)
{
    if (!s)
        return;
    // ```lang\n...\n```
    if (strncmp(s, "```", 3) != 0)
        return;
    char *nl = strchr(s, '\n');
    if (!nl)
        return;
    char *end = strstr(nl + 1, "```");
    if (!end)
        return;
    // Move content between nl+1 and end into s.
    size_t inner = end - (nl + 1);
    memmove(s, nl + 1, inner);
    s[inner] = '\0';
    // Trim trailing whitespace/newlines.
    while (inner > 0 && (s[inner - 1] == '\n' || s[inner - 1] == '\r' ||
                          s[inner - 1] == ' ' || s[inner - 1] == '\t'))
    {
        s[--inner] = '\0';
    }
}

static void oa_trim(char *s)
{
    if (!s)
        return;
    char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    if (p != s)
        memmove(s, p, strlen(p) + 1);
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
                        s[len - 1] == '\n' || s[len - 1] == '\r'))
    {
        s[--len] = '\0';
    }
}

static void oa_clean_response(char *s)
{
    if (!s)
        return;
    oa_trim(s);
    oa_strip_think(s);
    oa_strip_template(s);
    oa_trim(s);
    oa_strip_markdown_fence(s);
    oa_trim(s);
    oa_strip_prefix(s);
    oa_trim(s);
    oa_strip_outer_quotes(s);
    oa_trim(s);
}

// Build the chat-completions JSON request body. Returns talloc-allocated
// UTF-8 string, parented to talloc_ctx.
static char *oa_build_request_body(void *talloc_ctx,
                                   struct whisper_translator *tr,
                                   const char *text)
{
    void *tmp = talloc_new(NULL);
    struct mpv_node root;
    node_init(&root, MPV_FORMAT_NODE_MAP, NULL);
    // Reparent root's list to tmp so we can free everything together.
    talloc_steal(tmp, root.u.list);

    node_map_add_string(&root, "model", tr->oa_model);
    node_map_add_flag(&root, "stream", false);

    struct mpv_node *messages = node_map_add(&root, "messages",
                                              MPV_FORMAT_NODE_ARRAY);

    // System message
    {
        struct mpv_node *m = node_array_add(messages, MPV_FORMAT_NODE_MAP);
        const char *prompt = tr->oa_system_prompt && tr->oa_system_prompt[0]
            ? tr->oa_system_prompt
            : oa_render_fallback_prompt(tmp, tr->target_lang);
        node_map_add_string(m, "role", "system");
        node_map_add_string(m, "content", prompt);
    }

    // (No conversational history: translator is stateless.)

    // Current user turn
    {
        struct mpv_node *m = node_array_add(messages, MPV_FORMAT_NODE_MAP);
        node_map_add_string(m, "role", "user");
        node_map_add_string(m, "content", text);
    }

    // temperature
    node_map_add_double(&root, "temperature", 0.2);

    // max_tokens (omit if 0)
    if (tr->oa_max_tokens > 0)
        node_map_add_int64(&root, "max_tokens", tr->oa_max_tokens);

    // Disable model "reasoning" / chain-of-thought for low-latency subtitle
    // translation by adding `{"thinking": {"type": "disabled"}}`. Backends
    // that don't recognize the field will ignore it; backends that reject
    // unknown fields surface as a per-call HTTP error and the failure path
    // falls back to the original text.
    {
        struct mpv_node *th = node_map_add(&root, "thinking",
                                           MPV_FORMAT_NODE_MAP);
        node_map_add_string(th, "type", "disabled");
    }

    char *out = NULL;
    int n = json_write(&out, &root);
    char *result = NULL;
    if (n >= 0 && out) {
        result = talloc_strdup(talloc_ctx, out);
    }
    talloc_free(out);   // json_write uses talloc internally
    talloc_free(tmp);
    return result;
}

// Parse response and extract choices[0].message.content.
// Returns talloc-allocated UTF-8 (parented to talloc_ctx) or NULL.
static char *oa_extract_content(void *talloc_ctx, struct mp_log *log,
                                const char *response)
{
    void *tmp = talloc_new(NULL);
    struct mpv_node root = {0};
    if (!parse_json_document(tmp, response, &root)) {
        mp_warn(log, "translate: openai response is not JSON\n");
        talloc_free(tmp);
        return NULL;
    }
    char *out = NULL;
    if (root.format == MPV_FORMAT_NODE_MAP) {
        struct mpv_node *choices = node_map_get(&root, "choices");
        if (choices && choices->format == MPV_FORMAT_NODE_ARRAY &&
            choices->u.list && choices->u.list->num > 0)
        {
            struct mpv_node *first = &choices->u.list->values[0];
            if (first->format == MPV_FORMAT_NODE_MAP) {
                struct mpv_node *msg = node_map_get(first, "message");
                if (msg && msg->format == MPV_FORMAT_NODE_MAP) {
                    struct mpv_node *content = node_map_get(msg, "content");
                    if (content && content->format == MPV_FORMAT_STRING &&
                        content->u.string)
                    {
                        out = talloc_strdup(talloc_ctx, content->u.string);
                    }
                }
            }
        }
    }
    talloc_free(tmp);
    return out;
}

static char *translate_openai(struct whisper_translator *tr,
                              void *talloc_ctx, const char *text,
                              struct wt_call_result *out,
                              struct wt_http_observation *observation)
{
    if (validate_text(text, SIZE_MAX) != WT_TEXT_VALID) {
        set_err(out, "openai: invalid UTF-8");
        return NULL;
    }

    void *tmp = talloc_new(NULL);
    char *body = oa_build_request_body(tmp, tr, text);
    if (!body) {
        set_err(out, "openai: build body failed");
        talloc_free(tmp);
        return NULL;
    }

    char *headers;
    if (tr->oa_api_key && tr->oa_api_key[0]) {
        headers = talloc_asprintf(
            tmp,
            "Content-Type: application/json\r\n"
            "Accept: application/json\r\n"
            "Authorization: Bearer %s\r\n",
            tr->oa_api_key);
    } else {
        headers = talloc_strdup(
            tmp,
            "Content-Type: application/json\r\n"
            "Accept: application/json\r\n");
    }

    struct wt_http_request request = {
        .host = tr->oa_host,
        .port = tr->oa_port,
        .secure = tr->oa_is_secure,
        .method = "POST",
        .path = tr->oa_path,
        .headers = headers,
        .body = body,
        .body_len = strlen(body),
        .proxy_mode = WT_HTTP_PROXY_NONE,
        .timeout_ms = tr->oa_timeout_ms,
    };
    char *response = NULL;
    if (!perform_http(tr, tmp, &request, out, observation, &response)) {
        talloc_free(tmp);
        return NULL;
    }

    char *content = oa_extract_content(talloc_ctx, tr->log, response);
    if (!content || !content[0]) {
        set_err(out, "openai: empty content");
        talloc_free(content);
        talloc_free(tmp);
        return NULL;
    }

    oa_clean_response(content);
    if (!content[0]) {
        set_err(out, "openai: empty after clean");
        talloc_free(content);
        talloc_free(tmp);
        return NULL;
    }

    talloc_free(tmp);
    return content;
}


// --- Public API ---

static struct whisper_translator *translator_alloc(
    void *talloc_parent, struct mp_log *log, enum wt_provider provider,
    const char *source_lang, const char *target_lang)
{
    (void)talloc_parent;
    if (!target_lang || !target_lang[0])
        return NULL;
    // Lifetime is governed by the explicit refcount so an asynchronous
    // request can safely finish closing after its owner releases the backend.
    struct whisper_translator *tr =
        talloc_zero(NULL, struct whisper_translator);
    tr->log = log;
    tr->provider = provider;
    tr->source_lang = talloc_strdup(
        tr, source_lang && source_lang[0] ? source_lang : "auto");
    tr->target_lang = talloc_strdup(tr, target_lang);
    tr->transport = winhttp_transport;
    tr->transport_ctx = tr;
    tr->monotonic_ms = real_monotonic_ms;
    tr->unix_ms = real_unix_ms;
    tr->clock_ctx = NULL;
    mp_mutex_init(&tr->state_lock);
    atomic_init(&tr->refcount, 1);
    return tr;
}

static bool configure_openai(struct whisper_translator *tr,
                             const struct wt_openai_config *cfg)
{
    if (!cfg || !cfg->endpoint || !cfg->endpoint[0] ||
        !cfg->model || !cfg->model[0] ||
        !cfg->target_lang || !cfg->target_lang[0])
    {
        return false;
    }
    if (!parse_endpoint_url(tr, cfg->endpoint,
                            &tr->oa_scheme, &tr->oa_host,
                            &tr->oa_port, &tr->oa_path,
                            &tr->oa_is_secure))
    {
        return false;
    }

    tr->oa_model = talloc_strdup(tr, cfg->model);
    tr->oa_api_key = talloc_strdup(tr, cfg->api_key ? cfg->api_key : "");
    tr->oa_system_prompt = talloc_strdup(
        tr, cfg->system_prompt ? cfg->system_prompt : "");
    int timeout = cfg->timeout_ms > 0
        ? cfg->timeout_ms : WT_DEFAULT_TIMEOUT_MS;
    tr->oa_timeout_ms =
        timeout < WT_MAX_TIMEOUT_MS ? timeout : WT_MAX_TIMEOUT_MS;
    if (cfg->max_tokens == 0) {
        tr->oa_max_tokens = 0;
    } else if (cfg->max_tokens < 0) {
        tr->oa_max_tokens = WT_DEFAULT_MAX_TOKENS;
    } else {
        tr->oa_max_tokens = cfg->max_tokens;
    }
    return true;
}

struct whisper_translator *whisper_translator_create(
    void *talloc_parent, struct mp_log *log,
    enum wt_provider provider,
    const char *source_lang, const char *target_lang)
{
    if (provider != WT_PROVIDER_GOOGLE && provider != WT_PROVIDER_AZURE)
        return NULL;
    struct whisper_translator *tr = translator_alloc(
        talloc_parent, log, provider, source_lang, target_lang);
    if (!tr)
        return NULL;

    WCHAR *ua = utf8_to_wide(NULL, WT_EXTERNAL_USER_AGENT);
    tr->session = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS,
                               WINHTTP_FLAG_ASYNC);
    talloc_free(ua);

    if (!tr->session) {
        mp_err(log, "translate: WinHttpOpen failed (%lu)\n", GetLastError());
        mp_mutex_destroy(&tr->state_lock);
        talloc_free(tr);
        return NULL;
    }

    if (!WinHttpSetTimeouts(
            tr->session, WT_MAX_TIMEOUT_MS, WT_MAX_TIMEOUT_MS,
            WT_MAX_TIMEOUT_MS, WT_MAX_TIMEOUT_MS))
    {
        mp_err(log, "translate: WinHttpSetTimeouts failed (%lu)\n",
               GetLastError());
        WinHttpCloseHandle(tr->session);
        tr->session = NULL;
        mp_mutex_destroy(&tr->state_lock);
        talloc_free(tr);
        return NULL;
    }

    mp_info(log, "translate: created %s translator (%s -> %s)\n",
            provider == WT_PROVIDER_GOOGLE ? "google" : "azure",
            tr->source_lang, tr->target_lang);

    return tr;
}

struct whisper_translator *whisper_translator_create_openai(
    void *talloc_parent, struct mp_log *log,
    const struct wt_openai_config *cfg)
{
    if (!cfg) {
        mp_err(log, "translate: openai config missing endpoint/model/target_lang\n");
        return NULL;
    }
    struct whisper_translator *tr = translator_alloc(
        talloc_parent, log, WT_PROVIDER_OPENAI,
        cfg->source_lang, cfg->target_lang);
    if (!tr || !configure_openai(tr, cfg)) {
        mp_err(log, "translate: openai config missing or invalid\n");
        if (tr) {
            mp_mutex_destroy(&tr->state_lock);
            talloc_free(tr);
        }
        return NULL;
    }

    // Default-proxy session is unused for openai but kept NULL-safe.
    // Use NO_PROXY for loopback / local services.
    WCHAR *ua = utf8_to_wide(NULL, WT_OPENAI_USER_AGENT);
    tr->session_noproxy = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_NO_PROXY,
                                       WINHTTP_NO_PROXY_NAME,
                                       WINHTTP_NO_PROXY_BYPASS,
                                       WINHTTP_FLAG_ASYNC);
    talloc_free(ua);

    if (!tr->session_noproxy) {
        mp_err(log, "translate: WinHttpOpen (NO_PROXY) failed (%lu)\n",
               GetLastError());
        mp_mutex_destroy(&tr->state_lock);
        talloc_free(tr);
        return NULL;
    }

    int recv_to = tr->oa_timeout_ms;
    if (recv_to < 1000) recv_to = 1000;
    if (!WinHttpSetTimeouts(
            tr->session_noproxy, 5000, 5000, recv_to, recv_to))
    {
        mp_err(log, "translate: WinHttpSetTimeouts (NO_PROXY) failed (%lu)\n",
               GetLastError());
        WinHttpCloseHandle(tr->session_noproxy);
        tr->session_noproxy = NULL;
        mp_mutex_destroy(&tr->state_lock);
        talloc_free(tr);
        return NULL;
    }

    mp_info(log, "translate: created openai translator (model=%s, %s -> %s, "
                 "timeout=%dms, %s://%s:%d%s)\n",
            tr->oa_model, tr->source_lang, tr->target_lang,
            tr->oa_timeout_ms,
            tr->oa_scheme, tr->oa_host, tr->oa_port, tr->oa_path);
    return tr;
}

struct whisper_translator *whisper_translator_create_for_test(
    void *talloc_parent, enum wt_provider provider,
    const char *source_lang, const char *target_lang,
    const struct wt_openai_config *openai,
    const struct wt_test_hooks *hooks)
{
    if (!hooks || !hooks->transport || !hooks->monotonic_ms ||
        !hooks->unix_ms)
    {
        return NULL;
    }

    struct whisper_translator *tr;
    if (provider == WT_PROVIDER_OPENAI) {
        if (!openai)
            return NULL;
        tr = translator_alloc(
            talloc_parent, NULL, provider,
            openai->source_lang, openai->target_lang);
        if (!tr || !configure_openai(tr, openai)) {
            if (tr) {
                mp_mutex_destroy(&tr->state_lock);
                talloc_free(tr);
            }
            return NULL;
        }
    } else if (provider == WT_PROVIDER_GOOGLE ||
               provider == WT_PROVIDER_AZURE)
    {
        tr = translator_alloc(
            talloc_parent, NULL, provider, source_lang, target_lang);
        if (!tr)
            return NULL;
    } else {
        return NULL;
    }

    tr->transport = hooks->transport;
    tr->transport_ctx = hooks->transport_ctx;
    tr->monotonic_ms = hooks->monotonic_ms;
    tr->unix_ms = hooks->unix_ms;
    tr->clock_ctx = hooks->clock_ctx;
    return tr;
}

size_t whisper_translate_test_max_response_bytes(void)
{
    return WT_MAX_RESPONSE_BYTES;
}

struct wt_test_winhttp_client {
    struct whisper_translator *translator;
    int port;
    int timeout_ms;
    bool disable_cookies;
};

struct wt_test_winhttp_client *whisper_translate_test_winhttp_create(
    int port, int timeout_ms, bool disable_cookies)
{
    if (port <= 0 || port > 65535 || timeout_ms <= 0)
        return NULL;
    struct wt_test_winhttp_client *client =
        talloc_zero(NULL, struct wt_test_winhttp_client);
    client->translator = translator_alloc(
        client, NULL, WT_PROVIDER_GOOGLE, "auto", "ko");
    if (!client->translator) {
        talloc_free(client);
        return NULL;
    }

    WCHAR *user_agent = utf8_to_wide(NULL, WT_EXTERNAL_USER_AGENT);
    client->translator->session_noproxy = WinHttpOpen(
        user_agent, WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS,
        WINHTTP_FLAG_ASYNC);
    talloc_free(user_agent);
    int phase_timeout = timeout_ms < 1000 ? 1000 : timeout_ms;
    if (!client->translator->session_noproxy ||
        !WinHttpSetTimeouts(
            client->translator->session_noproxy,
            phase_timeout, phase_timeout, phase_timeout, phase_timeout))
    {
        whisper_translator_destroy(&client->translator);
        talloc_free(client);
        return NULL;
    }
    client->port = port;
    client->timeout_ms = timeout_ms;
    client->disable_cookies = disable_cookies;
    return client;
}

void whisper_translate_test_winhttp_call_body(
    struct wt_test_winhttp_client *client, void *talloc_ctx,
    const void *body, size_t body_len, struct wt_call_result *out,
    struct wt_test_finalization_receipt **receipt)
{
    memset(out, 0, sizeof(*out));
    if (receipt)
        *receipt = NULL;
    if (!client) {
        set_err(out, "google: request setup failed");
        return;
    }
    struct wt_http_request request = {
        .host = "127.0.0.1",
        .port = client->port,
        .secure = false,
        .method = "POST",
        .path = "/synthetic",
        .headers = "Content-Type: text/plain; charset=utf-8\r\n",
        .body = body,
        .body_len = body_len,
        .proxy_mode = WT_HTTP_PROXY_NONE,
        .disable_cookies = client->disable_cookies,
        .timeout_ms = client->timeout_ms,
    };
    struct wt_http_response response = {0};
    winhttp_transport_impl(
        client->translator, talloc_ctx, &request,
        &response, receipt);
    out->http_issued = response.http_issued;
    out->http_status = response.http_status;
    out->rate_limited = response.http_status == 429;
    switch (response.failure) {
    case WT_HTTP_FAILURE_NONE:
        if (response.http_status == 200 && response.body) {
            out->translated = talloc_strndup(
                talloc_ctx, (char *)response.body,
                response.body_len);
        } else {
            set_err(out, "google: HTTP %d", response.http_status);
        }
        break;
    case WT_HTTP_FAILURE_SETUP:
        set_err(out, "google: request setup failed");
        break;
    case WT_HTTP_FAILURE_SEND:
        set_err(out, "google: request send failed");
        break;
    case WT_HTTP_FAILURE_RECEIVE:
        set_err(out, "google: response receive failed");
        break;
    case WT_HTTP_FAILURE_STATUS:
        set_err(out, "google: invalid HTTP status");
        break;
    case WT_HTTP_FAILURE_READ:
        set_err(out, "google: response read failed");
        break;
    case WT_HTTP_FAILURE_TOO_LARGE:
        set_err(out, "google: response too large");
        break;
    case WT_HTTP_FAILURE_TIMEOUT:
        set_err(out, "google: request timed out");
        break;
    }
}

void whisper_translate_test_winhttp_call(
    struct wt_test_winhttp_client *client, void *talloc_ctx,
    struct wt_call_result *out,
    struct wt_test_finalization_receipt **receipt)
{
    static const char body[] = "synthetic";
    whisper_translate_test_winhttp_call_body(
        client, talloc_ctx, body, sizeof(body) - 1,
        out, receipt);
}

void whisper_translate_test_winhttp_destroy(
    struct wt_test_winhttp_client **client)
{
    if (!client || !*client)
        return;
    whisper_translator_destroy(&(*client)->translator);
    talloc_free(*client);
    *client = NULL;
}

int whisper_translate_test_active_async_requests(void)
{
    return atomic_load_explicit(
        &active_async_requests, memory_order_acquire);
}

struct whisper_translator *whisper_translator_acquire(
    struct whisper_translator *tr)
{
    if (!tr)
        return NULL;
    atomic_fetch_add_explicit(&tr->refcount, 1, memory_order_acq_rel);
    return tr;
}

static void whisper_translator_real_destroy(struct whisper_translator *t)
{
    if (t->session)
        WinHttpCloseHandle(t->session);
    if (t->session_noproxy)
        WinHttpCloseHandle(t->session_noproxy);
    mp_mutex_destroy(&t->state_lock);
    talloc_free(t);
}

static bool whisper_translator_release_internal(
    struct whisper_translator **tr)
{
    if (!tr || !*tr)
        return false;
    struct whisper_translator *t = *tr;
    *tr = NULL;
    if (atomic_fetch_sub_explicit(
            &t->refcount, 1, memory_order_acq_rel) == 1)
    {
        whisper_translator_real_destroy(t);
        return true;
    }
    return false;
}

void whisper_translator_release(struct whisper_translator **tr)
{
    whisper_translator_release_internal(tr);
}

void whisper_translator_destroy(struct whisper_translator **tr)
{
    // Equivalent to a final release of the creator's initial refcount. If
    // pipeline workers still hold acquired refs, real teardown is deferred
    // until they release.
    whisper_translator_release(tr);
}

struct wt_admission {
    uint64_t cooldown_generation;
    uint64_t probe_token;
};

static uint64_t next_probe_token_locked(struct whisper_translator *tr)
{
    tr->next_probe_token++;
    if (!tr->next_probe_token)
        tr->next_probe_token++;
    return tr->next_probe_token;
}

static void release_probe_locked(struct whisper_translator *tr,
                                 const struct wt_admission *admission)
{
    if (admission->probe_token &&
        tr->active_probe_token == admission->probe_token)
    {
        tr->active_probe_token = 0;
    }
}

static bool admit_request(struct whisper_translator *tr,
                          struct wt_call_result *out,
                          struct wt_admission *admission)
{
    int64_t now = translator_monotonic_ms(tr);
    mp_mutex_lock(&tr->state_lock);
    admission->cooldown_generation = tr->cooldown_generation;

    bool has_cooldown = tr->cooldown_kind != WT_COOLDOWN_NONE;
    int64_t remaining = tr->backoff_until_ms > now
        ? tr->backoff_until_ms - now : 0;
    if (remaining > 0 || tr->active_probe_token) {
        enum wt_cooldown_kind kind = tr->cooldown_kind;
        if (!remaining)
            remaining = WT_PROBE_WAIT_MS;
        mp_mutex_unlock(&tr->state_lock);
        if (out) {
            out->http_status = 0;
            out->http_issued = false;
            out->rate_limited = kind == WT_COOLDOWN_RATE_LIMIT;
            out->retry_after_ms = saturated_int64_to_int(remaining);
        }
        set_err(out, kind == WT_COOLDOWN_RATE_LIMIT
                ? "%s: rate limited" : "%s: cooling down",
                provider_name(tr->provider));
        return false;
    }

    if (has_cooldown) {
        admission->probe_token = next_probe_token_locked(tr);
        tr->active_probe_token = admission->probe_token;
    }
    mp_mutex_unlock(&tr->state_lock);
    return true;
}

static int64_t failure_backoff_ms(int fail_count)
{
    if (fail_count < WT_BACKOFF_FAIL_THRESHOLD)
        return 0;
    int shift = fail_count - WT_BACKOFF_FAIL_THRESHOLD;
    if (shift > 10)
        shift = 10;
    int64_t delay = (int64_t)WT_BACKOFF_BASE_MS << shift;
    return delay < WT_BACKOFF_MAX_MS ? delay : WT_BACKOFF_MAX_MS;
}

static void extend_cooldown_locked(struct whisper_translator *tr,
                                   int64_t now, int64_t delay,
                                   enum wt_cooldown_kind kind)
{
    bool was_active = tr->backoff_until_ms > now;
    int64_t candidate = saturated_add_ms(now, delay);
    if (candidate > tr->backoff_until_ms)
        tr->backoff_until_ms = candidate;
    if (kind == WT_COOLDOWN_RATE_LIMIT ||
        tr->cooldown_kind == WT_COOLDOWN_NONE ||
        !was_active)
    {
        tr->cooldown_kind = kind;
    }
    tr->cooldown_generation++;
}

static bool immediate_generic_cooldown(int http_status)
{
    return (http_status >= 300 && http_status <= 399) ||
           http_status == 401 || http_status == 403 ||
           http_status == 404 || http_status == 407;
}

static void record_outcome(struct whisper_translator *tr,
                           const struct wt_admission *admission,
                           const struct wt_call_result *call,
                           const struct wt_http_observation *observation,
                           bool success)
{
    enum {
        WT_LOG_NONE,
        WT_LOG_RATE_LIMIT,
        WT_LOG_RETRY_AFTER,
        WT_LOG_FAILURE_COOLDOWN,
    } log_event = WT_LOG_NONE;
    int64_t now = translator_monotonic_ms(tr);
    mp_mutex_lock(&tr->state_lock);

    if (success) {
        if (admission->cooldown_generation == tr->cooldown_generation) {
            tr->fail_count = 0;
            tr->backoff_until_ms = 0;
            tr->cooldown_kind = WT_COOLDOWN_NONE;
        }
        release_probe_locked(tr, admission);
        mp_mutex_unlock(&tr->state_lock);
        return;
    }

    if (!call->http_issued) {
        release_probe_locked(tr, admission);
        mp_mutex_unlock(&tr->state_lock);
        return;
    }

    if (tr->fail_count < INT_MAX)
        tr->fail_count++;

    if (call->http_status == 429) {
        int64_t delay = observation->retry_after.valid
            ? observation->retry_after.delay_ms
            : WT_RATE_LIMIT_BACKOFF_MS;
        extend_cooldown_locked(
            tr, now, delay, WT_COOLDOWN_RATE_LIMIT);
        log_event = WT_LOG_RATE_LIMIT;
    } else if (observation->retry_after.valid) {
        extend_cooldown_locked(
            tr, now, observation->retry_after.delay_ms,
            WT_COOLDOWN_GENERIC);
        log_event = WT_LOG_RETRY_AFTER;
    } else {
        int64_t delay = immediate_generic_cooldown(call->http_status)
            ? WT_CHALLENGE_BACKOFF_MS
            : failure_backoff_ms(tr->fail_count);
        if (admission->probe_token && delay <= 0)
            delay = WT_BACKOFF_BASE_MS;
        if (delay > 0) {
            extend_cooldown_locked(
                tr, now, delay, WT_COOLDOWN_GENERIC);
            log_event = WT_LOG_FAILURE_COOLDOWN;
        }
    }
    release_probe_locked(tr, admission);
    mp_mutex_unlock(&tr->state_lock);

    switch (log_event) {
    case WT_LOG_RATE_LIMIT:
        mp_warn(tr->log, "translate: %s paused after HTTP 429\n",
                provider_name(tr->provider));
        break;
    case WT_LOG_RETRY_AFTER:
        mp_warn(tr->log, "translate: %s honored Retry-After\n",
                provider_name(tr->provider));
        break;
    case WT_LOG_FAILURE_COOLDOWN:
        mp_warn(tr->log, "translate: %s entered failure cooldown\n",
                provider_name(tr->provider));
        break;
    case WT_LOG_NONE:
        break;
    }
}

void whisper_translate_call(struct whisper_translator *tr,
                            void *talloc_ctx, const char *text,
                            struct wt_call_result *out)
{
    struct wt_call_result local_out = {0};
    struct wt_call_result *call = out ? out : &local_out;
    memset(call, 0, sizeof(*call));
    if (!tr || !text || !text[0]) {
        set_err(call, "invalid args");
        return;
    }

    struct wt_admission admission = {0};
    if (!admit_request(tr, call, &admission))
        return;

    struct wt_http_observation observation = {0};
    char *result = NULL;
    switch (tr->provider) {
    case WT_PROVIDER_GOOGLE:
        result = translate_google(
            tr, talloc_ctx, text, call, &observation);
        break;
    case WT_PROVIDER_AZURE:
        result = translate_azure(
            tr, talloc_ctx, text, call, &observation);
        break;
    case WT_PROVIDER_OPENAI:
        result = translate_openai(
            tr, talloc_ctx, text, call, &observation);
        break;
    default:
        set_err(call, "unknown provider");
        break;
    }

    record_outcome(tr, &admission, call, &observation, result != NULL);
    call->translated = result;
}

char *whisper_translate(struct whisper_translator *tr,
                        void *talloc_ctx, const char *text)
{
    struct wt_call_result r;
    whisper_translate_call(tr, talloc_ctx, text, &r);
    if (!r.translated && r.error[0]) {
        // Preserve the previous behaviour of logging WARN on failure for
        // sync callers that do not inspect wt_call_result themselves.
        mp_mutex_lock(&tr->state_lock);
        int fc = tr->fail_count;
        mp_mutex_unlock(&tr->state_lock);
        if (fc <= 3 || fc % 10 == 0)
            mp_warn(tr->log, "translate: failed (count=%d, %s)\n", fc, r.error);
    }
    return r.translated;
}

void whisper_translator_get_status(struct whisper_translator *tr,
                                   struct wt_status *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    if (!tr)
        return;
    out->enabled = true;
    mp_mutex_lock(&tr->state_lock);
    out->fail_count = tr->fail_count;
    int64_t now = translator_monotonic_ms(tr);
    if (tr->backoff_until_ms && now < tr->backoff_until_ms) {
        out->paused = true;
        out->retry_after_ms =
            saturated_int64_to_int(tr->backoff_until_ms - now);
    } else if (tr->active_probe_token) {
        out->paused = true;
        out->retry_after_ms = WT_PROBE_WAIT_MS;
    }
    mp_mutex_unlock(&tr->state_lock);
    // last_error is no longer tracked at the translator level (it is
    // per-call). Keep the field in wt_status for ABI/JSON compat but emit
    // an empty string.
    out->last_error[0] = '\0';
}
