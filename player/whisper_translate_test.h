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

#ifndef MP_WHISPER_TRANSLATE_TEST_H
#define MP_WHISPER_TRANSLATE_TEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "whisper_translate.h"

// Internal offline test seam. Provider hosts and paths are still constructed
// by whisper_translate.c; tests can only observe requests and supply responses.
enum wt_http_proxy_mode {
    WT_HTTP_PROXY_DEFAULT,
    WT_HTTP_PROXY_NONE,
};

enum wt_http_failure {
    WT_HTTP_FAILURE_NONE,
    WT_HTTP_FAILURE_SETUP,
    WT_HTTP_FAILURE_SEND,
    WT_HTTP_FAILURE_RECEIVE,
    WT_HTTP_FAILURE_STATUS,
    WT_HTTP_FAILURE_READ,
    WT_HTTP_FAILURE_TOO_LARGE,
    WT_HTTP_FAILURE_TIMEOUT,
    WT_HTTP_FAILURE_CLOSE,
};

struct wt_http_request {
    const char *host;
    int port;
    bool secure;
    const char *method;
    const char *path;
    const char *headers;
    const char *body;
    size_t body_len;
    enum wt_http_proxy_mode proxy_mode;
    bool disable_cookies;
    int timeout_ms;
    int test_read_limit;
};

struct wt_http_response {
    enum wt_http_failure failure;
    bool http_issued;
    int http_status;
    const char *retry_after;
    const unsigned char *body;
    size_t body_len;
};

typedef void (*wt_http_transport_fn)(void *ctx, void *talloc_ctx,
                                    const struct wt_http_request *request,
                                    struct wt_http_response *response);
typedef int64_t (*wt_clock_fn)(void *ctx);

struct wt_test_hooks {
    wt_http_transport_fn transport;
    void *transport_ctx;
    wt_clock_fn monotonic_ms;
    wt_clock_fn unix_ms;
    void *clock_ctx;
};

struct whisper_translator *whisper_translator_create_for_test(
    void *talloc_parent, enum wt_provider provider,
    const char *source_lang, const char *target_lang,
    const struct wt_openai_config *openai,
    const struct wt_test_hooks *hooks);

size_t whisper_translate_test_max_response_bytes(void);
void whisper_translate_test_fail_next_cleanup_thread_create(void);
bool whisper_translate_test_start_cleanup_service(void);
void whisper_translate_test_hold_finalizer(void);
bool whisper_translate_test_wait_finalizer_held(int timeout_ms);
void whisper_translate_test_release_finalizer(void);

struct wt_test_winhttp_client;
struct wt_test_finalization_receipt;
struct wt_test_session_receipt;

enum wt_test_close_kind {
    WT_TEST_CLOSE_REQUEST = 1,
    WT_TEST_CLOSE_CONNECTION,
    WT_TEST_CLOSE_SESSION,
};

struct wt_test_finalization_status {
    bool terminal;
    bool finalized;
    bool retained_orphan;
    enum wt_test_close_kind retained_close_kind;
    unsigned long close_error;
    int closing_notifications;
    bool closing_handle_mismatch;
    unsigned closing_thread_id;
    unsigned finalizer_thread_id;
    int callbacks_after_terminal;
    int request_close_count;
    int connection_close_count;
    int translator_destroy_count;
};

struct wt_test_session_status {
    bool terminal;
    bool closed;
    bool retained_orphan;
    unsigned long close_error;
    int close_count;
};

// Fixed 127.0.0.1:/synthetic harness for the ordinary WinHTTP transport.
// Only the ephemeral fixture port, total deadline, and cookie policy vary.
struct wt_test_winhttp_client *whisper_translate_test_winhttp_create(
    int port, int timeout_ms, bool disable_cookies);
void whisper_translate_test_winhttp_call(
    struct wt_test_winhttp_client *client, void *talloc_ctx,
    struct wt_call_result *out,
    struct wt_test_finalization_receipt **receipt);
void whisper_translate_test_winhttp_call_body(
    struct wt_test_winhttp_client *client, void *talloc_ctx,
    const void *body, size_t body_len, struct wt_call_result *out,
    struct wt_test_finalization_receipt **receipt);
void whisper_translate_test_winhttp_destroy(
    struct wt_test_winhttp_client **client);
struct wt_test_session_receipt *
whisper_translate_test_winhttp_session_receipt(
    struct wt_test_winhttp_client *client);
void whisper_translate_test_winhttp_fail_next_close(
    struct wt_test_winhttp_client *client,
    enum wt_test_close_kind kind);
void whisper_translate_test_winhttp_fail_next_setup(
    struct wt_test_winhttp_client *client);
void whisper_translate_test_winhttp_force_closing_mismatch(
    struct wt_test_winhttp_client *client);
void whisper_translate_test_winhttp_hold_after_send(
    struct wt_test_winhttp_client *client);
bool whisper_translate_test_winhttp_wait_send_submitted(
    struct wt_test_winhttp_client *client, int timeout_ms);
bool whisper_translate_test_winhttp_send_completion_observed(
    struct wt_test_winhttp_client *client);
bool whisper_translate_test_winhttp_submitted_body_is_owned(
    struct wt_test_winhttp_client *client,
    const void *original_body, size_t body_len);
void whisper_translate_test_winhttp_release_send(
    struct wt_test_winhttp_client *client);
void whisper_translate_test_winhttp_set_read_limit(
    struct wt_test_winhttp_client *client, int bytes);
bool whisper_translate_test_finalization_wait(
    struct wt_test_finalization_receipt *receipt, int timeout_ms,
    struct wt_test_finalization_status *out);
bool whisper_translate_test_finalization_wait_late_callback(
    struct wt_test_finalization_receipt *receipt, int timeout_ms);
void whisper_translate_test_finalization_release(
    struct wt_test_finalization_receipt **receipt);
bool whisper_translate_test_session_wait(
    struct wt_test_session_receipt *receipt, int timeout_ms,
    struct wt_test_session_status *out);
void whisper_translate_test_session_release(
    struct wt_test_session_receipt **receipt);
bool whisper_translate_test_check_admission(
    struct wt_test_winhttp_client *client,
    struct wt_call_result *out);
void whisper_translate_test_winhttp_status(
    struct wt_test_winhttp_client *client,
    struct wt_status *out);
int whisper_translate_test_active_async_requests(void);

#endif
