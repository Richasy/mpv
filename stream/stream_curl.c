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

#include <inttypes.h>
#include <errno.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <curl/curl.h>

#include <libavformat/avio.h>
#include <libavformat/version.h>
#include <libavutil/avstring.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>

#include "stream.h"
#include "stream_curl.h"

#include "common/common.h"
#include "common/global.h"
#include "common/msg.h"
#include "cookies.h"
#include "demux/avio_crypto.h"
#include "demux/demux.h"
#include "misc/bstr.h"
#include "misc/dispatch.h"
#include "misc/path_utils.h"
#include "misc/thread_tools.h"
#include "mpv_talloc.h"
#include "network.h"
#include "options/m_config.h"
#include "options/m_option.h"
#include "options/path.h"
#include "osdep/threads.h"
#include "osdep/timer.h"
#include "player/dash_source.h"

enum curl_proto {
    MP_CURL_PROTO_HTTP,
    MP_CURL_PROTO_FTP,
};

struct curl_scheme {
    bstr scheme;
    enum curl_proto proto;
};

static const struct curl_scheme curl_schemes[] = {
    {bstr0_lit("http"), MP_CURL_PROTO_HTTP},
    {bstr0_lit("https"), MP_CURL_PROTO_HTTP},
    {bstr0_lit("ftp"), MP_CURL_PROTO_FTP},
    {bstr0_lit("ftps"), MP_CURL_PROTO_FTP},
};

// Special args for use by lavf. Matches lavf/http.c "offset"/"end_offset" opts.
// `offset` is the inclusive starting byte.
// `end_offset` is the exclusive upper bound (0 = unbounded).
struct curl_open_args {
    int64_t offset;
    int64_t end_offset;
};

struct curl_opts {
    bool enabled;
    int http_version;
    int max_redirects;
    int max_retries;
    int prefer_family;
    bool http_keepalive;
    bool retry_http_429;
    bool respect_retry_after;
    double retry_delay_max;
    double connect_timeout;
    int64_t buffer_size;
    int64_t max_request_size;
};

// Older lavf has a bug with nested IO cleanup, so don't enable curl by default.
// <https://code.ffmpeg.org/FFmpeg/FFmpeg/pulls/23082>
#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(62, 15, 101)
#define CURL_BY_DEFAULT
#elif LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(62, 12, 102) && LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(62, 13, 0)
#define CURL_BY_DEFAULT /* 8.1 backport */
#elif LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(62, 3, 103) && LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(62, 4, 0)
#define CURL_BY_DEFAULT /* 8.0 backport */
#endif

#define OPT_BASE_STRUCT struct curl_opts
const struct m_sub_options curl_conf = {
    .opts = (const struct m_option[]) {
        {"enabled", OPT_BOOL(enabled)},
        {"http-version", OPT_CHOICE(http_version,
            {"auto", CURL_HTTP_VERSION_NONE},
            {"1.0", CURL_HTTP_VERSION_1_0},
            {"1.1", CURL_HTTP_VERSION_1_1},
            {"2", CURL_HTTP_VERSION_2},
            {"2tls", CURL_HTTP_VERSION_2TLS},
            {"2-prior-knowledge", CURL_HTTP_VERSION_2_PRIOR_KNOWLEDGE},
            {"3", CURL_HTTP_VERSION_3},
            {"3only", CURL_HTTP_VERSION_3ONLY}
            )},
        {"max-redirects", OPT_INT(max_redirects), M_RANGE(0, 100)},
        {"max-retries", OPT_INT(max_retries), M_RANGE(0, 100)},
        {"prefer-family", OPT_CHOICE(prefer_family,
            {"auto", 0}, {"ipv4", 1}, {"ipv6", 2})},
        {"http-keepalive", OPT_BOOL(http_keepalive)},
        {"retry-http-429", OPT_BOOL(retry_http_429)},
        {"respect-retry-after", OPT_BOOL(respect_retry_after)},
        {"retry-delay-max", OPT_DOUBLE(retry_delay_max), M_RANGE(0, 600)},
        {"connect-timeout", OPT_DOUBLE(connect_timeout), M_RANGE(0, 600)},
        {"buffer-size", OPT_BYTE_SIZE(buffer_size),
            M_RANGE(2 * CURL_MAX_WRITE_SIZE, M_MAX_MEM_BYTES)},
        {"max-request-size", OPT_BYTE_SIZE(max_request_size),
            M_RANGE(0, M_MAX_MEM_BYTES)},
        {0}
    },
    .defaults = &(const struct curl_opts) {
#ifdef CURL_BY_DEFAULT
        .enabled = true,
#endif
        .http_version = CURL_HTTP_VERSION_NONE,
        .max_redirects = 16,
        .max_retries = 5,
        .http_keepalive = true,
        .respect_retry_after = true,
        .retry_delay_max = 30,
        .connect_timeout = 30,
        .buffer_size = 4 << 20, // 4 MiB
        .max_request_size = 0,
    },
    .size = sizeof(struct curl_opts),
};

static const struct curl_scheme *curl_scheme_lookup(bstr url)
{
    bstr scheme = mp_split_proto(url, NULL);
    for (int i = 0; i < MP_ARRAY_SIZE(curl_schemes); i++) {
        if (bstrcasecmp(scheme, curl_schemes[i].scheme) == 0)
            return &curl_schemes[i];
    }
    return NULL;
}

// libcurl rejects URLs that contain raw spaces or non-ASCII bytes with
// CURLE_URL_MALFORMAT ("URL using bad/illegal format or missing URL"), unlike
// lavf's http protocol which is lenient. WebDAV/SMB/etc. paths proxied over
// http (e.g. "http://host/dav/动漫/foo - bar.mp4") routinely contain spaces and
// unicode, so escape them the same way stream_lavf's normalize_url() does.
// Escape everything but reserved characters, and keep '%' in the allow set so
// already-escaped URLs are not double-escaped. '#' is deliberately NOT reserved:
// a literal '#' in a proxied filename (e.g. "メダリスト #13.mkv") would otherwise
// be parsed by libcurl as the start of a URL fragment and dropped from the
// request path, so the server receives a truncated path and answers 404.
static char *normalize_url(void *ta_parent, const char *url)
{
    return mp_url_escape(ta_parent, url, ":/?[]@!$&'()*+,;=%");
}

struct curl_ctx {
    mp_thread thread;
    struct mp_dispatch_queue *dispatch;
    CURLM *multi;
    bool exit;
    struct priv *retries;
    uint64_t next_request_id;

    // Set when libcurl cannot be used anymore.
    const char *error;
    atomic_bool failed;
#ifdef MPV_DASH_TEST_HOOKS
    mp_mutex test_lock;
    mp_cond test_cond;
    int test_role;
    bool test_armed;
    bool test_entered;
    bool test_release;
    int test_success_branches;
    int test_blocked_continuations;
    int test_added_after_stop;
    int test_window_role;
    int test_window_end;
    int test_complete_error_role;
    struct priv *test_waiting;
#endif
};

// Per-stream state, owned by the curl thread.
struct priv {
    struct mp_log *log;
    struct mpv_global *global;
    struct curl_ctx *ctx;
    struct stream *s;

    struct curl_opts *opts;
    struct mp_network_opts *net_opts;

    CURL *curl;
    struct curl_slist *headers;
    char *url;
    const struct curl_scheme *scheme;
    mpv_dash_track_kind dash_kind;
    struct mp_dash_track_config dash;
    bool dash_status_seen;
    bool dash_interim_headers;
    unsigned dash_interim_count;
    bool dash_ordinary_response;
    bool dash_headers_ok;
    bool dash_response_length_known;
    uint64_t dash_requested_end;
    uint64_t dash_response_length;

    // Stream parameters
    bool seekable;
    int64_t content_size; // -1 if unknown
    int64_t start_offset; // requested initial byte offset, immutable after open
    char *server_filename; // from Content-Disposition, NULL if absent

    // Producer state. Only touched by the curl thread.
    uint64_t request_start;    // absolute byte position of next request
    uint64_t request_received; // bytes received in the current request
    uint64_t request_end;      // exclusive byte cap (0 = unbounded)
    int retry_count;           // consecutive failed attempts at request_start
    bool active;               // handle is currently active in the multi
    bool finished;             // current request has reached EOF
    int64_t reply_end;         // inclusive end of the reply's byte range, -1 if none
    bool reply_rejected;       // reply is unusable, its body is dropped
    bool retry_http;
    double retry_after;
    int64_t open_deadline;
    int64_t retry_deadline;
    int64_t retry_at;
    struct priv *retry_next;

    uint64_t diagnostic_id;
    uint64_t diagnostic_offset;
    int64_t diagnostic_started;
    int64_t diagnostic_progress;
    int64_t diagnostic_next_report;
    int diagnostic_reports;
    bool diagnostic_headers;
    bool diagnostic_body;
    int64_t diagnostic_range_start;
    int64_t diagnostic_range_total;

    // Probe state. Set on the curl thread read by curl_open after.
    bool probed;
    bool initial_probe_ok;
    bool initial_probe_seekable;
    bool stream_ok;

    // Shared state, protected by mtx.
    mp_mutex mtx;
    mp_cond cond;
    uint8_t *buffer;
    size_t buffer_size;
    size_t head, tail, count;
    bool paused;         // write callback paused due to a full buffer
    bool request_pending; // continuation waits for consumer demand
    bool stream_eof;     // producer has delivered all data
    bool stream_error;   // unrecoverable error
    CURLcode error_code; // CURLcode behind stream_error (CURLE_OK if none)
    atomic_bool aborted; // canceled by user (mp_cancel)
    struct mp_icy *icy;  // ICY metadata state, dormant until Icy-MetaInt seen
};

// Curl thread

enum cmd_kind {
    CMD_ADD,
    CMD_REMOVE,
    CMD_SEEK,
    CMD_UNPAUSE,
    CMD_EXIT,
};

struct cmd {
    enum cmd_kind kind;
    struct curl_ctx *ctx;
    struct priv *p;
    int64_t pos;
    bool drop;
};

static void start_request(struct priv *p);
static void on_done(struct priv *p, CURLcode code);

static void defer_request(struct priv *p)
{
    mp_mutex_lock(&p->mtx);
    p->request_pending = true;
    mp_cond_broadcast(&p->cond);
    mp_mutex_unlock(&p->mtx);
}

static double diagnostic_time(struct priv *p, CURLINFO info)
{
    curl_off_t value;
    return curl_easy_getinfo(p->curl, info, &value) == CURLE_OK
        ? (double)value / 1000 : -1;
}

static long diagnostic_long(struct priv *p, CURLINFO info)
{
    long value;
    return curl_easy_getinfo(p->curl, info, &value) == CURLE_OK ? value : -1;
}

// Curl-thread only. Never query or format addresses, headers, or error buffers.
static void report_request(struct priv *p, const char *event, int result)
{
    if (!p->diagnostic_id)
        return;
    bool pending = !strcmp(event, "pending");
    int level = pending || result > 0 ? MSGL_WARN : MSGL_INFO;
    if (!mp_msg_test(p->log, level))
        return;

    mp_mutex_lock(&p->mtx);
    size_t buffered = p->count;
    bool paused = p->paused;
    mp_mutex_unlock(&p->mtx);
    int64_t now = mp_time_ns();
    mp_msg(p->log, level,
           "curl_request %s id=%"PRIu64" offset=%"PRIu64
           " retry=%d status=%ld proxy_status=%ld http_version=%ld"
           " redirects=%ld new_connections=%ld range_start=%"PRId64
           " range_total=%"PRId64" received=%"PRIu64" buffered=%zu"
           " paused=%d cancelled=%d rejected=%d headers=%d body=%d"
           " curl_result=%d elapsed_ms=%.3f idle_ms=%.3f"
           " dns_ms=%.3f connect_ms=%.3f tls_ms=%.3f pretransfer_ms=%.3f"
           " first_byte_ms=%.3f redirect_ms=%.3f total_ms=%.3f report=%d/3\n",
           event, p->diagnostic_id, p->diagnostic_offset, p->retry_count,
           diagnostic_long(p, CURLINFO_RESPONSE_CODE),
           diagnostic_long(p, CURLINFO_HTTP_CONNECTCODE),
           diagnostic_long(p, CURLINFO_HTTP_VERSION),
           diagnostic_long(p, CURLINFO_REDIRECT_COUNT),
           diagnostic_long(p, CURLINFO_NUM_CONNECTS),
           p->diagnostic_range_start, p->diagnostic_range_total,
           p->request_received, buffered, paused, atomic_load(&p->aborted),
           p->reply_rejected, p->diagnostic_headers, p->diagnostic_body, result,
           MP_TIME_NS_TO_MS(now - p->diagnostic_started),
           MP_TIME_NS_TO_MS(now - p->diagnostic_progress),
           diagnostic_time(p, CURLINFO_NAMELOOKUP_TIME_T),
           diagnostic_time(p, CURLINFO_CONNECT_TIME_T),
           diagnostic_time(p, CURLINFO_APPCONNECT_TIME_T),
           diagnostic_time(p, CURLINFO_PRETRANSFER_TIME_T),
           diagnostic_time(p, CURLINFO_STARTTRANSFER_TIME_T),
           diagnostic_time(p, CURLINFO_REDIRECT_TIME_T),
           diagnostic_time(p, CURLINFO_TOTAL_TIME_T), p->diagnostic_reports);
}

static void note_request_progress(struct priv *p)
{
    p->diagnostic_progress = mp_time_ns();
    p->diagnostic_next_report =
        p->diagnostic_progress + MP_TIME_S_TO_NS(15);
}

static void remove_retry(struct priv *p)
{
    struct priv **next = &p->ctx->retries;
    while (*next) {
        if (*next == p) {
            *next = p->retry_next;
            p->retry_next = NULL;
            p->retry_at = 0;
            return;
        }
        next = &(*next)->retry_next;
    }
}

static void start_pending_retries(struct curl_ctx *ctx)
{
    struct priv **next = &ctx->retries;
    int64_t now = mp_time_ns();
    while (*next) {
        struct priv *p = *next;
        if (atomic_load(&p->aborted) || p->retry_at <= now) {
            *next = p->retry_next;
            p->retry_next = NULL;
            p->retry_at = 0;
            if (!atomic_load(&p->aborted))
                start_request(p);
        } else {
            next = &p->retry_next;
        }
    }
}

static int retry_poll_timeout(struct curl_ctx *ctx)
{
    int timeout = 1000;
    int64_t now = mp_time_ns();
    for (struct priv *p = ctx->retries; p; p = p->retry_next) {
        int64_t remaining = MPMAX(p->retry_at - now, 0);
        timeout = MPMIN(timeout, (remaining + 999999) / 1000000);
    }
    return timeout;
}

static bool dash_request_stopped(struct priv *p)
{
    return p->dash_kind &&
        (atomic_load_explicit(&p->aborted, memory_order_relaxed) ||
         mp_dash_source_terminal(p->global));
}

static void run_cmd(void *arg)
{
    struct cmd *c = arg;
    struct curl_ctx *ctx = c->ctx;
    switch (c->kind) {
    case CMD_ADD:
        MP_TRACE(c->p, "starting curl request at %" PRIu64 "\n", c->p->request_start);
        start_request(c->p);
        break;
    case CMD_REMOVE:
        if (c->p->retry_at)
            report_request(c->p, "retry_removed", -1);
        remove_retry(c->p);
        if (c->p->active) {
            report_request(c->p, "removed", -1);
            MP_TRACE(c->p, "removing curl handle\n");
            curl_multi_remove_handle(ctx->multi, c->p->curl);
            c->p->active = false;
        }
        break;
    case CMD_UNPAUSE:
        if (dash_request_stopped(c->p))
            break;
        // The consumer freed enough buffer space. Clear the pause flag and
        // resume the transfer.
        mp_mutex_lock(&c->p->mtx);
        MP_TRACE(c->p, "resuming curl transfer\n");
        c->p->paused = false;
        mp_mutex_unlock(&c->p->mtx);
        note_request_progress(c->p);
        curl_easy_pause(c->p->curl, CURLPAUSE_CONT);
        break;
    case CMD_SEEK:
        if (c->p->retry_at)
            report_request(c->p, "retry_seek", -1);
        remove_retry(c->p);
        MP_TRACE(c->p, "seeking to %" PRIu64 "\n", c->pos);
        if (c->p->active) {
            report_request(c->p, "seek", -1);
            curl_multi_remove_handle(ctx->multi, c->p->curl);
            c->p->active = false;
        }
        mp_mutex_lock(&c->p->mtx);
        if (c->drop)
            c->p->head = c->p->tail = c->p->count = 0;
        c->p->paused = false;
        c->p->stream_eof = false;
        c->p->stream_error = false;
        c->p->request_pending = false;
        mp_cond_broadcast(&c->p->cond);
        mp_mutex_unlock(&c->p->mtx);
        c->p->request_start = c->pos;
        c->p->request_received = 0;
        c->p->retry_count = 0;
        c->p->retry_deadline = 0;
        c->p->finished = false;
        start_request(c->p);
        break;
    case CMD_EXIT:
        ctx->exit = true;
        mp_dispatch_interrupt(ctx->dispatch);
        break;
    }
}

static void cmd_async(struct priv *p, enum cmd_kind kind)
{
    struct cmd *c = talloc_zero(NULL, struct cmd);
    c->kind = kind;
    c->ctx = p->ctx;
    c->p = p;
    mp_dispatch_enqueue_autofree(p->ctx->dispatch, run_cmd, c);
}

static void cmd_sync(struct priv *p, enum cmd_kind kind, int64_t pos, bool drop)
{
    struct cmd c = {
        .kind = kind,
        .ctx = p->ctx,
        .p = p,
        .pos = pos,
        .drop = drop,
    };
    mp_dispatch_run(p->ctx->dispatch, run_cmd, &c);
}

static void curl_wakeup(void *arg)
{
    struct curl_ctx *ctx = arg;
    curl_multi_wakeup(ctx->multi);
}

static void set_failed(struct curl_ctx *ctx, const char *error)
{
    ctx->error = error;
    atomic_store_explicit(&ctx->failed, true, memory_order_release);
}

static bool curl_failed(struct curl_ctx *ctx, struct mp_log *log, int level)
{
    if (!atomic_load_explicit(&ctx->failed, memory_order_acquire))
        return false;
    mp_msg(log, level, "libcurl failed (%s), falling back to lavf\n", ctx->error);
    return true;
}

static MP_THREAD_VOID curl_thread(void *arg)
{
    mp_thread_set_name("curl");
    struct curl_ctx *ctx = arg;

    CURLcode res = curl_global_init(CURL_GLOBAL_ALL);
    if (res == CURLE_OK) {
        ctx->multi = curl_multi_init();
        if (!ctx->multi) {
            res = CURLE_OUT_OF_MEMORY;
            curl_global_cleanup();
        }
    }
    if (res != CURLE_OK) {
        set_failed(ctx, curl_easy_strerror(res));
    } else {
        curl_multi_setopt(ctx->multi, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);
        mp_dispatch_set_wakeup_fn(ctx->dispatch, curl_wakeup, ctx);
    }

    while (!ctx->exit) {
        // libcurl is unusable, but commands are still sent to this thread, so
        // keep serving them until CMD_EXIT, or their senders would wait forever.
        if (atomic_load_explicit(&ctx->failed, memory_order_relaxed)) {
            mp_dispatch_queue_process(ctx->dispatch, INFINITY);
            continue;
        }

        mp_dispatch_queue_process(ctx->dispatch, 0);
        start_pending_retries(ctx);

        // Stop early to avoid delays, this happens only when player is closing.
        if (ctx->exit)
            break;

        int running = 0;
        CURLMcode mres = curl_multi_perform(ctx->multi, &running);
        if (mres != CURLM_OK && mres != CURLM_CALL_MULTI_PERFORM) {
            set_failed(ctx, curl_multi_strerror(mres));
            continue;
        }

        CURLMsg *msg;
        int left = 0;
        while ((msg = curl_multi_info_read(ctx->multi, &left))) {
            if (msg->msg != CURLMSG_DONE)
                continue;
            struct priv *p = NULL;
            curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &p);
            mp_assert(p);
            curl_multi_remove_handle(ctx->multi, msg->easy_handle);
            p->active = false;
            on_done(p, msg->data.result);
        }

        curl_multi_poll(ctx->multi, NULL, 0, retry_poll_timeout(ctx), NULL);
    }

    if (ctx->multi) {
        curl_multi_cleanup(ctx->multi);
        curl_global_cleanup();
    }
    MP_THREAD_RETURN();
}

static void mp_curl_destroy(void *ptr)
{
    struct curl_ctx *ctx = ptr;
#ifdef MPV_DASH_TEST_HOOKS
    mp_mutex_lock(&ctx->test_lock);
    ctx->test_release = true;
    mp_cond_broadcast(&ctx->test_cond);
    mp_mutex_unlock(&ctx->test_lock);
#endif
    struct cmd c = { .kind = CMD_EXIT, .ctx = ctx };
    mp_dispatch_run(ctx->dispatch, run_cmd, &c);
    mp_thread_join(ctx->thread);
#ifdef MPV_DASH_TEST_HOOKS
    mp_cond_destroy(&ctx->test_cond);
    mp_mutex_destroy(&ctx->test_lock);
#endif
}

void mp_curl_global_init(struct mpv_global *global)
{
    struct curl_ctx *ctx = talloc_zero(global, struct curl_ctx);
    talloc_set_destructor(ctx, mp_curl_destroy);
    ctx->dispatch = mp_dispatch_create(ctx);
    global->curl = ctx;
#ifdef MPV_DASH_TEST_HOOKS
    mp_mutex_init(&ctx->test_lock);
    mp_cond_init(&ctx->test_cond);
#endif
    mp_require(!mp_thread_create(&ctx->thread, curl_thread, ctx));
}

#ifdef MPV_DASH_TEST_HOOKS
int mp_curl_dash_test_control(struct mpv_global *global, int command, int value)
{
    if (!global || !global->curl)
        return -1;
    struct curl_ctx *ctx = global->curl;
    int result = 0;
    mp_mutex_lock(&ctx->test_lock);
    switch (command) {
    case MP_CURL_DASH_TEST_ARM:
        if ((value != MPV_DASH_TRACK_VIDEO && value != MPV_DASH_TRACK_AUDIO) ||
            ctx->test_armed)
        {
            result = -1;
            break;
        }
        ctx->test_role = value;
        ctx->test_armed = true;
        ctx->test_entered = false;
        ctx->test_release = false;
        ctx->test_success_branches = 0;
        ctx->test_blocked_continuations = 0;
        ctx->test_added_after_stop = 0;
        break;
    case MP_CURL_DASH_TEST_WAIT: {
        if (value <= 0 || value > 15000) {
            result = -1;
            break;
        }
        int64_t deadline = mp_time_ns_add(mp_time_ns(), value / 1000.0);
        while (!ctx->test_entered &&
               !mp_cond_timedwait_until(&ctx->test_cond, &ctx->test_lock, deadline))
        {
        }
        result = ctx->test_entered ? 1 : 0;
        break;
    }
    case MP_CURL_DASH_TEST_RELEASE:
        ctx->test_release = true;
        mp_cond_broadcast(&ctx->test_cond);
        break;
    case MP_CURL_DASH_TEST_SUCCESS_BRANCHES:
        result = ctx->test_success_branches;
        break;
    case MP_CURL_DASH_TEST_BLOCKED_CONTINUATIONS:
        result = ctx->test_blocked_continuations;
        break;
    case MP_CURL_DASH_TEST_ADDED_AFTER_STOP:
        result = ctx->test_added_after_stop;
        break;
    case MP_CURL_DASH_TEST_AUDIO_WINDOW:
    case MP_CURL_DASH_TEST_VIDEO_WINDOW:
        if (value <= 0) {
            result = -1;
            break;
        }
        ctx->test_window_role = command == MP_CURL_DASH_TEST_AUDIO_WINDOW ?
                                MPV_DASH_TRACK_AUDIO : MPV_DASH_TRACK_VIDEO;
        ctx->test_window_end = value;
        break;
    case MP_CURL_DASH_TEST_COMPLETE_RECV_ERROR:
        if (value != MPV_DASH_TRACK_VIDEO && value != MPV_DASH_TRACK_AUDIO)
            result = -1;
        else
            ctx->test_complete_error_role = value;
        break;
    case MP_CURL_DASH_TEST_ABORT_WAITING:
        if (!ctx->test_waiting)
            result = -1;
        else
            atomic_store(&ctx->test_waiting->aborted, true);
        break;
    case MP_CURL_DASH_TEST_OTHER_TRACK_RISK:
        if (!ctx->test_waiting ||
            (value != MPV_DASH_TRACK_VIDEO && value != MPV_DASH_TRACK_AUDIO) ||
            value == ctx->test_waiting->dash_kind)
        {
            result = -1;
        } else {
            mp_dash_source_response(global, value, 412,
                MPV_DASH_FAILURE_HTTP_RISK, MPV_DASH_ORIGIN_OTHER);
        }
        break;
    default:
        result = -1;
        break;
    }
    mp_mutex_unlock(&ctx->test_lock);
    return result;
}

static void dash_test_before_continuation(struct priv *p)
{
    // Only a test build and an explicit loopback source can pause the real
    // continuation path; release is bounded if the test fails.
    if (!p->dash_kind || !p->dash.allow_loopback_http ||
        strncmp(p->url, "http://127.0.0.1:", 17))
        return;

    struct curl_ctx *ctx = p->ctx;
    mp_mutex_lock(&ctx->test_lock);
    if (ctx->test_armed && ctx->test_role == p->dash_kind) {
        ctx->test_armed = false;
        ctx->test_entered = true;
        ctx->test_waiting = p;
        ctx->test_success_branches++;
        mp_cond_broadcast(&ctx->test_cond);
        int64_t deadline = mp_time_ns_add(mp_time_ns(), 10);
        while (!ctx->test_release &&
               !mp_cond_timedwait_until(&ctx->test_cond, &ctx->test_lock, deadline))
        {
        }
        ctx->test_waiting = NULL;
    }
    mp_mutex_unlock(&ctx->test_lock);
}

static void dash_test_record_continuation(struct priv *p, bool added)
{
    struct curl_ctx *ctx = p->ctx;
    mp_mutex_lock(&ctx->test_lock);
    if (ctx->test_entered && ctx->test_role == p->dash_kind) {
        if (added)
            ctx->test_added_after_stop++;
        else
            ctx->test_blocked_continuations++;
        mp_cond_broadcast(&ctx->test_cond);
    }
    mp_mutex_unlock(&ctx->test_lock);
}
#endif

// Curl callbacks

static bool is_http_success(long resp)
{
    return resp >= 200 && resp < 300;
}

// Append `len` bytes to the ring buffer. Caller must hold p->mtx and have
// verified that there is enough free space.
static void ring_write(void *ctx, const char *data, size_t len)
{
    struct priv *p = ctx;
    size_t tail_chunk = MPMIN(p->buffer_size - p->tail, len);
    memcpy(p->buffer + p->tail, data, tail_chunk);
    memcpy(p->buffer, data + tail_chunk, len - tail_chunk);
    p->tail = (p->tail + len) % p->buffer_size;
    p->count += len;
}

// Called per chunk of body data.
static size_t write_callback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    struct priv *p = userdata;
    size_t bytes = size * nmemb;

    // header_callback validated the response and logged any error status,
    // we don't care about error body.
    if (!p->stream_ok || p->reply_rejected || (p->dash_kind &&
                          (!p->dash_headers_ok ||
                           mp_dash_source_terminal(p->global))))
        return CURL_WRITEFUNC_ERROR;

    if (atomic_load_explicit(&p->aborted, memory_order_relaxed))
        return CURL_WRITEFUNC_ERROR;

    if (p->dash_kind && p->request_end &&
        (p->request_start >= p->request_end ||
         p->request_received > p->request_end - p->request_start ||
         bytes > p->request_end - p->request_start - p->request_received))
    {
        mp_dash_source_fail_with_origin(p->global, p->dash_kind,
            MPV_DASH_FAILURE_HTTP_RANGE, MPV_DASH_ORIGIN_CONSUMER_WINDOW);
        return CURL_WRITEFUNC_ERROR;
    }
    if (p->dash_kind && p->dash_response_length_known &&
        (p->request_received > p->dash_response_length ||
         bytes > p->dash_response_length - p->request_received))
    {
        mp_dash_source_fail_with_origin(p->global, p->dash_kind,
            p->dash_ordinary_response ? MPV_DASH_FAILURE_TRANSPORT :
                                        MPV_DASH_FAILURE_HTTP_RANGE,
            p->dash_ordinary_response ? MPV_DASH_ORIGIN_BODY_LENGTH :
                                        MPV_DASH_ORIGIN_INVALID_PARTIAL_RANGE);
        return CURL_WRITEFUNC_ERROR;
    }

    mp_mutex_lock(&p->mtx);

    if (p->buffer_size - p->count < bytes) {
        // No room in the buffer. Pause the transfer and wait for the consumer.
        p->paused = true;
        mp_mutex_unlock(&p->mtx);
        MP_TRACE(p, "pausing curl transfer, buffer full (%zu bytes)\n", p->count);
        return CURL_WRITEFUNC_PAUSE;
    }

    mp_icy_process(p->icy, ptr, bytes, ring_write, p);

    p->paused = false;
    p->request_received += bytes;

    mp_cond_broadcast(&p->cond);
    mp_mutex_unlock(&p->mtx);
    note_request_progress(p);
    if (!p->diagnostic_body) {
        p->diagnostic_body = true;
        report_request(p, "first_body", -1);
    }

    return bytes;
}

static int xferinfo_callback(void *userdata, curl_off_t dl_total, curl_off_t dl_now,
                             curl_off_t ul_total, curl_off_t ul_now)
{
    struct priv *p = userdata;
    if (p->diagnostic_reports < 3 &&
        mp_time_ns() >= p->diagnostic_next_report)
    {
        mp_mutex_lock(&p->mtx);
        bool paused = p->paused;
        mp_mutex_unlock(&p->mtx);
        if (!paused && !atomic_load(&p->aborted)) {
            p->diagnostic_reports++;
            p->diagnostic_next_report = mp_time_ns() + MP_TIME_S_TO_NS(30);
            report_request(p, "pending", -1);
        }
    }
    return atomic_load_explicit(&p->aborted, memory_order_relaxed) ||
           (p->dash_kind && mp_dash_source_terminal(p->global));
}

static const char *header_value(CURL *c, const char *name)
{
    struct curl_header *h = NULL;
    if (curl_easy_header(c, name, 0, CURLH_HEADER, -1, &h) == CURLHE_OK)
        return h->value;
    return NULL;
}

static bool dash_identity_encoding(CURL *curl)
{
    struct curl_header *header = NULL;
    CURLHcode result = curl_easy_header(curl, "Content-Encoding", 0,
                                       CURLH_HEADER, -1, &header);
    if (result == CURLHE_MISSING)
        return true;
    return result == CURLHE_OK && header->amount == 1 &&
           (!header->value[0] || !strcasecmp(header->value, "identity"));
}

// "bytes <start>-<end>/<total>", where <total> is "*" if unknown, or
// "bytes */<total>" on a 416 reply. Absent or unparsable fields are -1.
static void parse_content_range(CURL *c, int64_t *out_start, int64_t *out_end,
                                int64_t *out_total)
{
    *out_start = *out_end = *out_total = -1;
    const char *value = header_value(c, "Content-Range");
    if (!value)
        return;
    bstr range, total;
    if (!bstr_split_tok(bstr0(value), "/", &range, &total))
        return;
    bstr rest;
    long long v = bstrtoll(total, &rest, 10);
    if (rest.len == 0 && v > 0)
        *out_total = v;
    bstr_eatstart0(&range, "bytes");
    bstr start, end;
    if (!bstr_split_tok(bstr_lstrip(range), "-", &start, &end))
        return;
    v = bstrtoll(start, &rest, 10);
    if (start.len > 0 && rest.len == 0 && v >= 0)
        *out_start = v;
    v = bstrtoll(end, &rest, 10);
    if (end.len > 0 && rest.len == 0 && v >= 0)
        *out_end = v;
}

static double retry_after_seconds(struct priv *p)
{
    const char *value = header_value(p->curl, "Retry-After");
    if (!value)
        return -1;
    char *end;
    errno = 0;
    long long seconds = strtoll(value, &end, 10);
    if (value != end && !*end && !errno && seconds >= 0)
        return seconds;
    time_t date = curl_getdate(value, NULL);
    if (date != (time_t)-1)
        return MPMAX(difftime(date, time(NULL)), 0);
    MP_WARN(p, "Ignoring an invalid Retry-After header.\n");
    return -1;
}

static void finalize_probe(struct priv *p)
{
    if (mp_msg_test(p->log, MSGL_DEBUG)) {
        long resp = 0;
        curl_easy_getinfo(p->curl, CURLINFO_RESPONSE_CODE, &resp);
        if (p->dash_kind) {
            MP_DBG(p, "DASH track=%d ok=%d code=%ld size=%" PRId64 "\n",
                   p->dash_kind, p->stream_ok, resp, p->content_size);
        } else {
            char *ctype = NULL;
            curl_easy_getinfo(p->curl, CURLINFO_CONTENT_TYPE, &ctype);
            MP_DBG(p, "proto=%.*s ok=%d code=%ld size=%" PRId64 " seekable=%d type=%s\n",
                   BSTR_P(p->scheme->scheme), p->stream_ok, resp,
                   p->content_size, p->seekable, ctype ? ctype : "-");
        }
    }

    mp_mutex_lock(&p->mtx);
    p->initial_probe_ok = p->stream_ok;
    p->initial_probe_seekable = p->seekable;
    p->probed = true;
    mp_cond_broadcast(&p->cond);
    mp_mutex_unlock(&p->mtx);
}

static void check_http_reply(struct priv *p, long resp, int64_t range_start,
                             int64_t range_total)
{
    if (resp == 416 && p->content_size < 0 &&
        (range_total < 0 || range_total <= p->request_start))
    {
        // The request started past the end of a file of unknown size, so this
        // is EOF. The reply tells the size if the server knows it by now.
        if (range_total > 0 && range_total <= p->request_start) {
            mp_mutex_lock(&p->mtx);
            p->content_size = range_total;
            mp_mutex_unlock(&p->mtx);
        }
        p->finished = true;
        p->reply_rejected = true;
        return;
    }
    if (!is_http_success(resp)) {
        MP_ERR(p, "HTTP error %ld\n", resp);
        p->reply_rejected = true;
        return;
    }
    int64_t start = resp == 206 ? range_start : 0;
    if (start != p->request_start) {
        MP_ERR(p, "Server sent back unexpected reply with offset %" PRId64
               " (expected %" PRIu64 ")\n", start, p->request_start);
        p->reply_rejected = true;
    }
}

// Empty line is the end of the header. Skip intermediate 1xx and 3xx responses,
// we care about the final one.
static void on_http_header(struct priv *p, struct bstr line)
{
    if (line.len > 0) {
        // ICY metadata is set up from the initial reply only. A new status line
        // resets per-response state so that intermediate 1xx/3xx responses
        // don't leak ICY metadata into the final one.
        if (p->probed)
            return;
        mp_mutex_lock(&p->mtx);
        if (bstr_startswith0(line, "HTTP/")) {
            mp_icy_reset(p->icy);
        } else {
            mp_icy_add_header(p->icy, line);
        }
        mp_mutex_unlock(&p->mtx);
        return;
    }

    long resp = 0;
    curl_easy_getinfo(p->curl, CURLINFO_RESPONSE_CODE, &resp);
    if (resp < 200 || (resp >= 300 && resp < 400))
        return;

    int64_t range_start, range_end, range_total;
    parse_content_range(p->curl, &range_start, &range_end, &range_total);
    p->reply_end = resp == 206 ? range_end : -1;
    p->diagnostic_headers = true;
    p->diagnostic_range_start = range_start;
    p->diagnostic_range_total = range_total;
    note_request_progress(p);
    report_request(p, "headers", -1);

    if (resp == 429 && p->opts->retry_http_429) {
        p->retry_http = true;
        p->retry_after = retry_after_seconds(p);
        p->reply_rejected = true;
        return;
    }

    if (p->probed) {
        check_http_reply(p, resp, range_start, range_total);
        return;
    }

    if (!is_http_success(resp)) {
        MP_ERR(p, "HTTP error %ld\n", resp);
        p->reply_rejected = true;
        goto done;
    }

    // A request with an explicit start offset must be honored by the server
    if (p->start_offset > 0 && range_start != p->start_offset) {
        MP_ERR(p, "Server ignored range request at offset %" PRId64 "\n",
               p->start_offset);
        p->reply_rejected = true;
        goto done;
    }

    // Compressed responses are byte-addressed in the encoded representation,
    // which our caller can't translate, so they are non-seekable.
    const char *ce = header_value(p->curl, "Content-Encoding");
    bool compressed = ce && ce[0] && strcasecmp(ce, "identity") != 0;
    const char *ar = header_value(p->curl, "Accept-Ranges");
    bool accept_ranges = ar && strcasecmp(ar, "bytes") == 0;

    // Some servers reply 200 to an open-ended "Range: 0-" but 206 to explicit
    // byte ranges, so trust either. ICY metadata is interleaved with the body,
    // so byte ranges from the server don't line up with consumer offsets.
    p->seekable = !compressed && !mp_icy_active(p->icy) &&
                  (resp == 206 || accept_ranges);

    if (p->seekable) {
        // Content-Range carries the full size on a partial response. On any
        // non-206 success code use Content-Length.
        int64_t total = range_total;
        if (total < 0 && resp != 206) {
            curl_off_t cl = -1;
            if (curl_easy_getinfo(p->curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T,
                                  &cl) == CURLE_OK && cl >= 0)
                total = cl;
        }
        p->content_size = total;
    }

    const char *cd = header_value(p->curl, "Content-Disposition");
    if (cd)
        p->server_filename = mp_parse_content_disposition(p, p->s, bstr0(cd));

    p->stream_ok = true;
done:
    finalize_probe(p);
}

static void probe_ftp(struct priv *p, struct bstr line)
{
    if (line.len < 4 || line.start[3] != ' ')
        return;
    // Parse the line directly: libcurl only stamps CURLINFO_RESPONSE_CODE after
    // a reply is fully processed, so polling it from header_callback returns
    // the previous code.
    struct bstr code = {line.start, 3};
    if (!bstr_equals0(code, "150") && !bstr_equals0(code, "125"))
        return;

    curl_off_t cl = -1;
    if (curl_easy_getinfo(p->curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T,
                          &cl) == CURLE_OK && cl >= 0)
        p->content_size = cl;

    p->seekable = p->content_size > 0;
    p->stream_ok = true;
    finalize_probe(p);
}

static bool parse_range_number(const char **at, uint64_t *value)
{
    if (**at < '0' || **at > '9')
        return false;
    errno = 0;
    char *end = NULL;
    unsigned long long n = strtoull(*at, &end, 10);
    if (errno || end == *at || n > INT64_MAX)
        return false;
    *value = n;
    *at = end;
    return true;
}

static bool dash_content_range(struct priv *p, uint64_t *length, int64_t *total)
{
    const char *at = header_value(p->curl, "Content-Range");
    if (!at || strncmp(at, "bytes ", 6))
        return false;
    at += 6;
    uint64_t start, end, size;
    if (!parse_range_number(&at, &start) || *at++ != '-' ||
        !parse_range_number(&at, &end) || *at++ != '/' ||
        !parse_range_number(&at, &size) || *at ||
        !size || start != p->request_start || end < start ||
        end >= size || end > p->dash_requested_end ||
        (p->content_size > 0 && size != p->content_size))
        return false;

    curl_off_t content_length = -1;
    if (curl_easy_getinfo(p->curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T,
                          &content_length) != CURLE_OK ||
        (content_length >= 0 && (uint64_t)content_length != end - start + 1))
        return false;
    if (!dash_identity_encoding(p->curl))
        return false;
    *length = end - start + 1;
    *total = size;
    return true;
}

static mpv_dash_failure_origin dash_media_length(struct priv *p,
                                                 uint64_t *length, bool *known)
{
    const char *declared = header_value(p->curl, "Content-Length");
    curl_off_t size = -1;
    if (curl_easy_getinfo(p->curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T,
                          &size) != CURLE_OK)
        return MPV_DASH_ORIGIN_DECLARED_LENGTH;
    if (declared) {
        if (size < 0 || (uint64_t)size > INT64_MAX)
            return MPV_DASH_ORIGIN_DECLARED_LENGTH;
        *length = size;
        *known = true;
    } else {
        *length = 0;
        *known = false;
    }
    // A per-request size hint is not a consumer-owned end_offset window.
    if (p->request_end && *known &&
        *length > p->request_end - p->request_start)
        return MPV_DASH_ORIGIN_CONSUMER_WINDOW;
    return MPV_DASH_ORIGIN_NONE;
}

static size_t dash_header_callback(struct priv *p, struct bstr line, size_t bytes)
{
    if (mp_dash_source_terminal(p->global))
        return 0;
    bool aborted = atomic_load_explicit(&p->aborted, memory_order_relaxed);
    if (bstr_startswith0(line, "HTTP/")) {
        bstr rest;
        int space = bstrchr(line, ' ');
        if (space < 0) {
            if (aborted)
                return 0;
            mp_dash_source_fail_with_origin(p->global, p->dash_kind,
                MPV_DASH_FAILURE_HTTP_STATUS, MPV_DASH_ORIGIN_MALFORMED_STATUS);
            return 0;
        }
        rest = bstr_cut(line, space + 1);
        if (rest.len < 3 || rest.start[0] < '0' || rest.start[0] > '9' ||
            rest.start[1] < '0' || rest.start[1] > '9' ||
            rest.start[2] < '0' || rest.start[2] > '9' ||
            (rest.len > 3 && rest.start[3] != ' '))
        {
            if (aborted)
                return 0;
            mp_dash_source_fail_with_origin(p->global, p->dash_kind,
                MPV_DASH_FAILURE_HTTP_STATUS, MPV_DASH_ORIGIN_MALFORMED_STATUS);
            return 0;
        }
        int status = (rest.start[0] - '0') * 100 +
                     (rest.start[1] - '0') * 10 + rest.start[2] - '0';
        // Keep observed auth/risk, not ordinary failures of a canceled consumer.
        if (aborted && status != 401 && status != 403 && status != 412)
            return 0;
        if (status >= 100 && status < 200 && !p->dash_status_seen &&
            p->dash_interim_count++ < 8)
        {
            p->dash_interim_headers = true;
            return bytes;
        }
        if (p->dash_status_seen || status < 200) {
            mp_dash_source_fail_with_origin(p->global, p->dash_kind,
                MPV_DASH_FAILURE_HTTP_STATUS,
                p->dash_status_seen ? MPV_DASH_ORIGIN_DUPLICATE_STATUS :
                                      MPV_DASH_ORIGIN_INTERIM_LIMIT);
            return 0;
        }
        p->dash_interim_headers = false;
        p->dash_status_seen = true;
        p->dash_ordinary_response = status != 206;
        mpv_dash_source_failure failure = MPV_DASH_FAILURE_NONE;
        mpv_dash_failure_origin origin = MPV_DASH_ORIGIN_OTHER;
        if (status == 401 || status == 403)
            failure = MPV_DASH_FAILURE_HTTP_AUTH;
        else if (status == 412)
            failure = MPV_DASH_FAILURE_HTTP_RISK;
        else if (status >= 300 && status < 400) {
            failure = MPV_DASH_FAILURE_HTTP_STATUS;
            origin = MPV_DASH_ORIGIN_REDIRECT;
        } else if (p->dash_ordinary_response &&
                   (p->request_start || p->start_offset)) {
            failure = MPV_DASH_FAILURE_HTTP_RANGE;
            origin = MPV_DASH_ORIGIN_NONZERO_OFFSET;
        } else if (status == 416 && p->request_end) {
            failure = MPV_DASH_FAILURE_HTTP_RANGE;
            origin = MPV_DASH_ORIGIN_CONSUMER_WINDOW;
        }
        mp_dash_source_response(p->global, p->dash_kind, status, failure, origin);
        return failure == MPV_DASH_FAILURE_NONE ? bytes : 0;
    }
    if (aborted)
        return 0;
    if (line.len)
        return bytes;
    if (p->dash_interim_headers) {
        p->dash_interim_headers = false;
        return bytes;
    }
    int64_t total = -1;
    mpv_dash_failure_origin origin = p->dash_status_seen ?
                                    MPV_DASH_ORIGIN_NONE :
                                    MPV_DASH_ORIGIN_MALFORMED_STATUS;
    if (origin == MPV_DASH_ORIGIN_NONE) {
        if (p->dash_ordinary_response) {
            origin = dash_media_length(p, &p->dash_response_length,
                                       &p->dash_response_length_known);
        } else if (!dash_content_range(p, &p->dash_response_length, &total)) {
            origin = MPV_DASH_ORIGIN_INVALID_PARTIAL_RANGE;
        }
    }
    bool valid = origin == MPV_DASH_ORIGIN_NONE;
    if (valid && !p->dash_ordinary_response)
        p->dash_response_length_known = true;
    if (!valid)
    {
        mp_dash_source_fail_with_origin(p->global, p->dash_kind,
            p->dash_ordinary_response ?
                (origin == MPV_DASH_ORIGIN_CONSUMER_WINDOW ?
                    MPV_DASH_FAILURE_HTTP_RANGE : MPV_DASH_FAILURE_TRANSPORT) :
                MPV_DASH_FAILURE_HTTP_RANGE, origin);
        if (!p->probed)
            finalize_probe(p);
        return 0;
    }
    if (!p->dash_ordinary_response) {
        mp_dash_source_range_validated(p->global, p->dash_kind);
        // An initially unknown-length 200 can become byte-addressable later.
        // Retain the validated total so EOF is not probed past the object.
        mp_mutex_lock(&p->mtx);
        p->content_size = total;
        mp_mutex_unlock(&p->mtx);
    }
    if (!p->probed) {
        p->content_size = p->dash_ordinary_response ?
            (p->dash_response_length_known ? (int64_t)p->dash_response_length : -1) :
            total;
        const char *ar = header_value(p->curl, "Accept-Ranges");
        // As in probe_http, 200 at offset zero may advertise later byte seeks.
        // Every actual response still passes the typed byte-window checks.
        p->seekable = !p->dash_ordinary_response ||
            (ar && strcasecmp(ar, "bytes") == 0);
        p->stream_ok = true;
    }
    p->dash_headers_ok = true;
    if (!p->probed)
        finalize_probe(p);
    return bytes;
}

// Called per header line.
static size_t header_callback(char *buffer, size_t size, size_t nitems, void *userdata)
{
    struct priv *p = userdata;
    size_t bytes = size * nitems;

    if (p->dash_kind)
        return dash_header_callback(p, bstr_strip_linebreaks((bstr){buffer, bytes}),
                                    bytes);
    if (p->probed && p->scheme->proto != MP_CURL_PROTO_HTTP)
        return bytes;

    struct bstr line = bstr_strip_linebreaks((bstr){buffer, bytes});
    switch (p->scheme->proto) {
    case MP_CURL_PROTO_HTTP:
        on_http_header(p, line);
        // Reject unusable replies now, even if their body never arrives.
        if (p->reply_rejected)
            return CURL_WRITEFUNC_ERROR;
        break;
    case MP_CURL_PROTO_FTP:
        probe_ftp(p, line);
        break;
    default:
        break;
    }

    return bytes;
}

static int debug_callback(CURL *handle, curl_infotype type, char *data, size_t size,
                          void *userdata)
{
    struct priv *p = userdata;
    const char *prefix;
    switch (type) {
    case CURLINFO_TEXT:       prefix = "* "; break;
    case CURLINFO_HEADER_IN:  prefix = "< "; break;
    case CURLINFO_HEADER_OUT: prefix = "> "; break;
    default:                  return 0;
    }
    bstr msg = bstr_strip_linebreaks((bstr){data, size});
    MP_TRACE(p, "%s%.*s\n", prefix, BSTR_P(msg));
    return 0;
}

// Request handling

static void finish_dash_terminal(struct priv *p)
{
    bool failed = mp_dash_source_failed(p->global);
    p->finished = true;
    mp_mutex_lock(&p->mtx);
    p->head = p->tail = p->count = 0;
    p->stream_error = failed;
    p->stream_eof = !failed;
    p->probed = true;
    mp_cond_broadcast(&p->cond);
    mp_mutex_unlock(&p->mtx);
}

static bool is_recoverable_error(CURLcode code)
{
    switch (code) {
    case CURLE_RECV_ERROR:
    case CURLE_SEND_ERROR:
    case CURLE_PARTIAL_FILE:
    case CURLE_OPERATION_TIMEDOUT:
    case CURLE_GOT_NOTHING:
    case CURLE_COULDNT_CONNECT:
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_HTTP2:
    case CURLE_HTTP2_STREAM:
        return true;
    default:
        return false;
    }
}

static bool check_setopt(struct priv *p, const char *name, CURLcode res)
{
    if (res == CURLE_OK)
        return true;
    MP_ERR(p, "Failed to set %s: %s\n", name, curl_easy_strerror(res));
    return false;
}

// The request could not be started. This also ends the probe in curl_open().
static void request_failed(struct priv *p)
{
    mp_mutex_lock(&p->mtx);
    p->probed = true;
    p->stream_error = true;
    mp_cond_broadcast(&p->cond);
    mp_mutex_unlock(&p->mtx);
}

static void start_request(struct priv *p)
{
    if (dash_request_stopped(p)) {
#ifdef MPV_DASH_TEST_HOOKS
        dash_test_record_continuation(p, false);
#endif
        finish_dash_terminal(p);
        return;
    }
    if (p->finished) {
        mp_mutex_lock(&p->mtx);
        p->stream_eof = true;
        mp_cond_broadcast(&p->cond);
        mp_mutex_unlock(&p->mtx);
        return;
    }

    if (atomic_load_explicit(&p->ctx->failed, memory_order_relaxed)) {
        request_failed(p);
        return;
    }

    uint64_t start = p->request_start;

    bool ranged = p->dash_kind || !p->probed || p->seekable;
    bool chunked = ranged && p->opts->max_request_size > 0;
    bool capped = ranged && p->request_end > 0;

    bool past_size = p->seekable && p->content_size > 0 && start >= p->content_size;
    bool past_end = capped && start >= p->request_end;
    if (past_size || past_end) {
        p->finished = true;
        mp_mutex_lock(&p->mtx);
        p->stream_eof = true;
        mp_cond_broadcast(&p->cond);
        mp_mutex_unlock(&p->mtx);
        return;
    }

    char range[64], *range_arg = NULL;
    p->dash_requested_end = INT64_MAX;
    if (chunked || capped) {
        uint64_t end = UINT64_MAX;
        if (chunked) {
            uint64_t cap = p->opts->max_request_size - 1;
            end = cap > INT64_MAX - start ? INT64_MAX : start + cap;
        }
        if (p->content_size > 0)
            end = MPMIN(end, p->content_size - 1);
        if (capped)
            end = MPMIN(end, p->request_end - 1);
        p->dash_requested_end = end;
        snprintf(range, sizeof(range), "%" PRIu64 "-%" PRIu64, start, end);
        range_arg = range;
    } else if (ranged) {
        snprintf(range, sizeof(range), "%" PRIu64 "-", start);
        range_arg = range;
    }
    if (!check_setopt(p, "CURLOPT_RANGE",
                      curl_easy_setopt(p->curl, CURLOPT_RANGE, range_arg)))
    {
        request_failed(p);
        return;
    }

    p->request_received = 0;
    p->reply_end = -1;
    p->reply_rejected = false;
    p->retry_http = false;
    p->retry_after = -1;
    if (p->dash_kind) {
        p->dash_status_seen = false;
        p->dash_interim_headers = false;
        p->dash_interim_count = 0;
        p->dash_ordinary_response = false;
        p->dash_headers_ok = false;
        p->dash_response_length_known = false;
        p->dash_response_length = 0;
    }
    if (dash_request_stopped(p)) {
        finish_dash_terminal(p);
        return;
    }
    CURLMcode mres = curl_multi_add_handle(p->ctx->multi, p->curl);
    if (mres != CURLM_OK) {
        MP_ERR(p, "Failed to start the request: %s\n", curl_multi_strerror(mres));
        request_failed(p);
        return;
    }
    p->active = true;
    p->diagnostic_id = ++p->ctx->next_request_id;
    p->diagnostic_offset = start;
    p->diagnostic_started = mp_time_ns();
    p->diagnostic_reports = 0;
    p->diagnostic_headers = false;
    p->diagnostic_body = false;
    p->diagnostic_range_start = -1;
    p->diagnostic_range_total = -1;
    note_request_progress(p);
    MP_INFO(p, "curl_request begin id=%"PRIu64" offset=%"PRIu64
            " ranged=%d retry=%d connect_timeout_s=%.3f"
            " network_timeout_s=%.3f\n",
            p->diagnostic_id, start, ranged, p->retry_count,
            p->opts->connect_timeout, p->net_opts->timeout);
    if (dash_request_stopped(p)) {
#ifdef MPV_DASH_TEST_HOOKS
        dash_test_record_continuation(p, true);
#endif
        curl_multi_remove_handle(p->ctx->multi, p->curl);
        p->active = false;
        finish_dash_terminal(p);
    }
}

static void log_curl_error(struct priv *p, const char *what, CURLcode code)
{
    MP_ERR(p, "%s: %s\n", what, curl_easy_strerror(code));
    if (code == CURLE_PEER_FAILED_VERIFICATION ||
        code == CURLE_SSL_CACERT_BADFILE)
    {
        MP_ERR(p,
            "TLS certificate verification failed.\n"
            "This usually means an outdated CA bundle, a self-signed "
            "certificate,\nor a MITM proxy on your network. To bypass at "
            "your own risk, pass\n--tls-verify=no.\n");
    }
}

static bool schedule_http_retry(struct priv *p)
{
    if (p->retry_count >= p->opts->max_retries)
        return false;
    double delay = p->retry_count ? ldexp(1, p->retry_count) - 1 : 0;
    if (p->opts->respect_retry_after && p->retry_after >= 0)
        delay = p->retry_after;
    if (delay > p->opts->retry_delay_max)
        return false;
    int64_t now = mp_time_ns();
    if (!p->retry_deadline && p->net_opts->timeout > 0)
        p->retry_deadline = mp_time_ns_add(now, p->net_opts->timeout);
    int64_t deadline = p->probed ? p->retry_deadline : p->open_deadline;
    int64_t retry_at = mp_time_ns_add(now, delay);
    if (deadline && retry_at >= deadline)
        return false;
    p->retry_count++;
    p->retry_at = retry_at;
    p->retry_next = p->ctx->retries;
    p->ctx->retries = p;
    MP_WARN(p, "HTTP 429, retrying (#%d) in %.3f seconds at offset %" PRIu64 "\n",
            p->retry_count, delay, p->request_start);
    return true;
}

static bool resume_dash_request(struct priv *p, CURLcode code)
{
    if (!p->dash_kind || dash_request_stopped(p) || !p->dash_headers_ok ||
        !p->seekable || !is_recoverable_error(code) ||
        p->retry_count >= p->opts->max_retries ||
        !p->dash_response_length_known || !p->request_received ||
        p->request_received >= p->dash_response_length || p->content_size <= 0)
        return false;

    struct curl_header *transfer = NULL;
    if (!dash_identity_encoding(p->curl) ||
        curl_easy_header(p->curl, "Transfer-Encoding", 0,
                         CURLH_HEADER, -1, &transfer) != CURLHE_MISSING)
        return false;

    uint64_t total = p->content_size;
    if (p->request_start >= total ||
        p->request_received >= total - p->request_start ||
        (p->request_end &&
         (p->request_start >= p->request_end ||
          p->request_received >= p->request_end - p->request_start)))
        return false;

    // The ring retains these identity bytes. Resume after the producer's
    // accepted bytes, not the consumer cursor or a past-size EOF shortcut.
    p->request_start += p->request_received;
    p->request_received = 0;
    p->retry_count++;
    MP_WARN(p, "DASH transfer interrupted (curl %d), resuming (#%d) at %" PRIu64 "\n",
            code, p->retry_count, p->request_start);
#ifdef MPV_DASH_TEST_HOOKS
    dash_test_before_continuation(p);
#endif
    start_request(p);
    return true;
}

static void on_done(struct priv *p, CURLcode code)
{
#ifdef MPV_DASH_TEST_HOOKS
    if (p->dash_kind && p->dash.allow_loopback_http &&
        !strncmp(p->url, "http://127.0.0.1:", 17) &&
        (code == CURLE_OK || code == CURLE_PARTIAL_FILE))
    {
        mp_mutex_lock(&p->ctx->test_lock);
        if (p->ctx->test_complete_error_role == p->dash_kind) {
            p->ctx->test_complete_error_role = 0;
            code = CURLE_RECV_ERROR;
        }
        mp_mutex_unlock(&p->ctx->test_lock);
    }
#endif
    report_request(p, "end", code);
    bool aborted = atomic_load_explicit(&p->aborted, memory_order_relaxed);

    if (!p->dash_kind && p->retry_http && !aborted) {
        if (schedule_http_retry(p))
            return;
        MP_ERR(p, "HTTP error 429; retry count or wait budget exhausted.\n");
        mp_mutex_lock(&p->mtx);
        p->error_code = CURLE_HTTP_RETURNED_ERROR;
        mp_mutex_unlock(&p->mtx);
    }

    struct mp_dash_transfer_diagnostic diagnostic = {
        .track = p->dash_kind,
        .curl_code = code,
        .length_known = p->dash_response_length_known,
        .expected = p->dash_response_length,
        .received = p->request_received,
        .request_start = p->request_start,
        .request_end = p->request_end,
        .headers_ok = p->dash_headers_ok,
        .aborted = aborted,
    };

    if (dash_request_stopped(p)) {
        finish_dash_terminal(p);
        return;
    }
    if (!p->probed) {
        // Connection died before any headers arrived.
        if (p->dash_kind && !aborted && !mp_dash_source_terminal(p->global))
            mp_dash_source_fail_transfer(p->global, MPV_DASH_FAILURE_TRANSPORT,
                MPV_DASH_ORIGIN_OTHER, diagnostic);
        if (code != CURLE_OK && !aborted && !p->dash_kind)
            log_curl_error(p, "error", code);
        mp_mutex_lock(&p->mtx);
        p->probed = true;
        mp_cond_broadcast(&p->cond);
        mp_mutex_unlock(&p->mtx);
        return;
    }

    if (resume_dash_request(p, code))
        return;

    if (p->dash_kind &&
        (!p->dash_headers_ok || code != CURLE_OK ||
         (p->dash_response_length_known &&
          p->request_received != p->dash_response_length) ||
         mp_dash_source_terminal(p->global)))
    {
        if (!aborted && !mp_dash_source_terminal(p->global))
            mp_dash_source_fail_transfer(p->global,
                MPV_DASH_FAILURE_TRANSPORT,
                p->dash_response_length_known &&
                    p->request_received != p->dash_response_length ?
                    MPV_DASH_ORIGIN_BODY_LENGTH : MPV_DASH_ORIGIN_OTHER,
                diagnostic);
        mp_mutex_lock(&p->mtx);
        p->head = p->tail = p->count = 0;
        p->stream_error = true;
        p->error_code = code;
        mp_cond_broadcast(&p->cond);
        mp_mutex_unlock(&p->mtx);
        return;
    }

    // Roll completed bytes into request_start so retries and chunk
    // continuations resume at the next missing byte.
    p->request_start += p->request_received;
    p->request_received = 0;

    if (p->reply_rejected && !aborted) {
        // The reply check cut the transfer short, so the curl result means
        // nothing. This is either the end of a file of unknown size or an
        // error that was logged already.
        mp_mutex_lock(&p->mtx);
        if (p->finished) {
            p->stream_eof = true;
        } else {
            p->stream_error = true;
        }
        mp_cond_broadcast(&p->cond);
        mp_mutex_unlock(&p->mtx);
        return;
    }

    if (code == CURLE_OK && !aborted) {
        p->retry_count = 0;
        p->retry_deadline = 0;

        if (p->dash_kind && p->dash_ordinary_response) {
            p->finished = true;
            mp_mutex_lock(&p->mtx);
            p->stream_eof = true;
            mp_cond_broadcast(&p->cond);
            mp_mutex_unlock(&p->mtx);
            return;
        }

        bool past_size = p->content_size > 0 && p->request_start >= p->content_size;
        bool past_end = p->request_end > 0 && p->request_start >= p->request_end;
        if ((p->dash_kind || (p->seekable && p->reply_end >= 0)) &&
            !past_size && !past_end) {
#ifdef MPV_DASH_TEST_HOOKS
            dash_test_before_continuation(p);
#endif
            if (p->dash_kind)
                start_request(p);
            else
                defer_request(p);
            return;
        }

        p->finished = true;
        mp_mutex_lock(&p->mtx);
        p->stream_eof = true;
        mp_cond_broadcast(&p->cond);
        mp_mutex_unlock(&p->mtx);
        return;
    }

    // Try to recover if the stream is seekable and the failure looks
    // recoverable.
    bool recoverable = !aborted && p->seekable &&
                       is_recoverable_error(code) &&
                       p->retry_count < p->opts->max_retries;
    if (recoverable) {
        p->retry_count++;
        MP_WARN(p, "%s, retrying (#%d) from %" PRIu64 "\n",
                curl_easy_strerror(code), p->retry_count, p->request_start);
        defer_request(p);
        return;
    }

    if (!aborted)
        log_curl_error(p, "transfer failed", code);

    mp_mutex_lock(&p->mtx);
    p->stream_error = true;
    p->error_code = code;
    mp_cond_broadcast(&p->cond);
    mp_mutex_unlock(&p->mtx);
}

static void on_cancel(void *ctx)
{
    struct priv *p = ctx;
    atomic_store(&p->aborted, true);
    mp_mutex_lock(&p->mtx);
    mp_cond_broadcast(&p->cond);
    mp_mutex_unlock(&p->mtx);
    if (p->ctx && p->ctx->multi)
        curl_multi_wakeup(p->ctx->multi);
}

// Configuration and initial setup

static struct curl_slist *slist_append(struct curl_slist *list, const char *str)
{
    list = curl_slist_append(list, str);
    MP_HANDLE_OOM(list);
    return list;
}

static struct curl_slist *build_header_list(struct priv *p)
{
    struct curl_slist *list = NULL;
    const char *referer = p->dash_kind ? p->dash.referer : p->net_opts->referrer;
    if (referer && referer[0]) {
        char *h = talloc_asprintf(NULL, "Referer: %s", referer);
        list = slist_append(list, h);
        talloc_free(h);
    }
    if (!p->dash_kind && p->net_opts->http_header_fields) {
        for (int i = 0; p->net_opts->http_header_fields[i]; i++)
            list = slist_append(list, p->net_opts->http_header_fields[i]);
    }
    if (!p->dash_kind && p->scheme->proto == MP_CURL_PROTO_HTTP)
        list = slist_append(list, "Icy-MetaData: 1");
    return list;
}

#define SETOPT(opt, val)                                                    \
    do {                                                                    \
        if (!check_setopt(p, #opt, curl_easy_setopt(c, opt, val)))          \
            return false;                                                   \
    } while (0)

static bool setup_curl(struct priv *p)
{
    curl_version_info_data *info = curl_version_info(CURLVERSION_NOW);
    CURL *c = p->curl;

    SETOPT(CURLOPT_URL, p->url);
    SETOPT(CURLOPT_NOSIGNAL, 1L);
    SETOPT(CURLOPT_PRIVATE, p);

    SETOPT(CURLOPT_WRITEFUNCTION, write_callback);
    SETOPT(CURLOPT_WRITEDATA, p);
    SETOPT(CURLOPT_HEADERFUNCTION, header_callback);
    SETOPT(CURLOPT_HEADERDATA, p);
    // enable progress callback, so we can cancel transfer at any point
    SETOPT(CURLOPT_NOPROGRESS, 0L);
    SETOPT(CURLOPT_XFERINFOFUNCTION, xferinfo_callback);
    SETOPT(CURLOPT_XFERINFODATA, p);

    // Enable verbose output with trace level logging.
    SETOPT(CURLOPT_VERBOSE,
           !p->dash_kind && mp_msg_test(p->log, MSGL_TRACE) ? 1L : 0L);
    SETOPT(CURLOPT_DEBUGFUNCTION, debug_callback);
    SETOPT(CURLOPT_DEBUGDATA, p);

    bool identity = p->dash_kind || p->start_offset > 0 || p->request_end > 0;
    SETOPT(CURLOPT_ACCEPT_ENCODING, identity ? "identity" : "");
    SETOPT(CURLOPT_FOLLOWLOCATION, p->dash_kind ? 0L : 1L);
    SETOPT(CURLOPT_MAXREDIRS, p->dash_kind ? 0L : (long)p->opts->max_redirects);
    if (p->dash_kind) {
        SETOPT(CURLOPT_PROTOCOLS_STR,
               p->dash.allow_loopback_http ? "https,http" : "https");
        SETOPT(CURLOPT_PROXY, "");
        SETOPT(CURLOPT_FRESH_CONNECT, 1L);
        SETOPT(CURLOPT_FORBID_REUSE, 1L);
    }
    SETOPT(CURLOPT_HTTP_VERSION, (long)p->opts->http_version);
    if (info->features & CURL_VERSION_HSTS)
        SETOPT(CURLOPT_HSTS_CTRL, (long)CURLHSTS_ENABLE);
    SETOPT(CURLOPT_TCP_KEEPALIVE, 1L);
    SETOPT(CURLOPT_CONNECTTIMEOUT_MS, (long)(p->opts->connect_timeout * 1000));
    if (!p->dash_kind && p->scheme->proto == MP_CURL_PROTO_HTTP) {
        SETOPT(CURLOPT_FRESH_CONNECT, p->opts->http_keepalive ? 0L : 1L);
        SETOPT(CURLOPT_FORBID_REUSE, p->opts->http_keepalive ? 0L : 1L);
    }
    if (!p->dash_kind && p->opts->prefer_family) {
#if defined(CURL_IPRESOLVE_V4_PREFERRED) && defined(CURL_IPRESOLVE_V6_PREFERRED)
        SETOPT(CURLOPT_IPRESOLVE, p->opts->prefer_family == 1 ?
               CURL_IPRESOLVE_V4_PREFERRED : CURL_IPRESOLVE_V6_PREFERRED);
#else
        MP_ERR(p, "This libcurl build cannot prefer an address family while retaining fallback.\n");
        return false;
#endif
    }

    const char *ua = p->dash_kind ? p->dash.user_agent : p->net_opts->useragent;
    if (!ua || !ua[0])
        ua = p->dash_kind ? "libmpv" : p->net_opts->default_useragent;
    if (ua && ua[0])
        SETOPT(CURLOPT_USERAGENT, ua);
    // An explicitly empty proxy disables the backend's environment fallback.
    if (!p->dash_kind && p->net_opts->http_proxy)
        SETOPT(CURLOPT_PROXY, p->net_opts->http_proxy);

    if (info->features & CURL_VERSION_SSL)
        SETOPT(CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);
    SETOPT(CURLOPT_SSL_VERIFYPEER, p->net_opts->tls_verify ? 1L : 0L);
    SETOPT(CURLOPT_SSL_VERIFYHOST, p->net_opts->tls_verify ? 2L : 0L);
    if (p->net_opts->tls_ca_file) {
        char *path = mp_get_user_path(p, p->global, p->net_opts->tls_ca_file);
        SETOPT(CURLOPT_CAINFO, path);
    }
    if (!p->dash_kind && p->net_opts->tls_cert_file) {
        char *path = mp_get_user_path(p, p->global, p->net_opts->tls_cert_file);
        SETOPT(CURLOPT_SSLCERT, path);
    }
    if (!p->dash_kind && p->net_opts->tls_key_file) {
        char *path = mp_get_user_path(p, p->global, p->net_opts->tls_key_file);
        SETOPT(CURLOPT_SSLKEY, path);
    }

    if (!p->dash_kind && p->net_opts->cookies_enabled) {
        SETOPT(CURLOPT_COOKIEFILE, "");
        char *file = p->net_opts->cookies_file;
        if (file && file[0]) {
            void *tmp = talloc_new(NULL);
            char *path = mp_get_user_path(tmp, p->global, file);
            bstr data = stream_read_file2(path, tmp,
                STREAM_READ_FILE_FLAGS_DEFAULT & ~STREAM_LOCAL_FS_ONLY,
                p->global, 1000000);
            bstr buf = data;
            bool ok = true;
            while (ok && buf.len) {
                bstr line = bstr_strip_linebreaks(bstr_getline(buf, &buf));
                if (!line.len)
                    continue;
                char *line_str = bstrto0(tmp, line);
                ok = check_setopt(p, "CURLOPT_COOKIELIST",
                                  curl_easy_setopt(c, CURLOPT_COOKIELIST, line_str));
            }
            talloc_free(tmp);
            if (!ok)
                return false;
        }
    }

    p->headers = build_header_list(p);
    if (p->headers)
        SETOPT(CURLOPT_HTTPHEADER, p->headers);
    return true;
}

#undef SETOPT

// stream_curl implementation

static void fail_dash_timeout(struct priv *p)
{
    bool aborted = atomic_load_explicit(&p->aborted, memory_order_relaxed);
    if (!p->dash_kind || aborted || mp_dash_source_terminal(p->global))
        return;
    mp_dash_source_fail_transfer(p->global, MPV_DASH_FAILURE_TRANSPORT,
        MPV_DASH_ORIGIN_OTHER, (struct mp_dash_transfer_diagnostic){
            .track = p->dash_kind,
            .curl_code = CURLE_OPERATION_TIMEDOUT,
            .length_known = p->dash_response_length_known,
            .expected = p->dash_response_length,
            .received = p->request_received,
            .request_start = p->request_start,
            .request_end = p->request_end,
            .headers_ok = p->dash_headers_ok,
            .aborted = false,
        });
}

static int curl_fill_buffer(struct stream *s, void *buffer, int max_len)
{
    struct priv *p = s->priv;
    if (max_len <= 0)
        return 0;
    if (p->dash_kind && mp_dash_source_terminal(p->global)) {
        s->error = mp_dash_source_failed(p->global) ? -1 : 0;
        return 0;
    }

    int64_t deadline = stream_get_io_deadline(s);
    bool timed_out = false;
    mp_mutex_lock(&p->mtx);

    while (p->count == 0 && !p->stream_eof && !p->stream_error &&
           !atomic_load_explicit(&p->aborted, memory_order_relaxed))
    {
        if (deadline && mp_time_ns() >= deadline) {
            p->stream_error = true;
            p->error_code = CURLE_OPERATION_TIMEDOUT;
            timed_out = true;
            break;
        }
        if (p->request_pending) {
            p->request_pending = false;
            mp_mutex_unlock(&p->mtx);
            cmd_sync(p, CMD_ADD, 0, false);
            mp_mutex_lock(&p->mtx);
            continue;
        }
        if (deadline)
            mp_cond_timedwait_until(&p->cond, &p->mtx, deadline);
        else
            mp_cond_wait(&p->cond, &p->mtx);
    }

    size_t copy = MPMIN((size_t)max_len, p->count);
    if (copy > 0) {
        size_t head_chunk = MPMIN(p->buffer_size - p->head, copy);
        memcpy(buffer, p->buffer + p->head, head_chunk);
        memcpy((char *)buffer + head_chunk, p->buffer, copy - head_chunk);
        p->head = (p->head + copy) % p->buffer_size;
        p->count -= copy;
    }

    bool had_error = p->stream_error;
    CURLcode err_code = p->error_code;

    bool unpause = p->paused && !p->stream_eof && !p->stream_error &&
                   p->buffer_size - p->count >= p->buffer_size / 2;

    mp_mutex_unlock(&p->mtx);

    if (timed_out) {
        cmd_sync(p, CMD_REMOVE, 0, false);
        fail_dash_timeout(p);
        MP_WARN(s, "Network data wait exhausted its timeout budget.\n");
    }
    if (p->dash_kind && mp_dash_source_terminal(p->global)) {
        s->error = mp_dash_source_failed(p->global) ? -1 : 0;
        return 0;
    }
    if (unpause)
        cmd_async(p, CMD_UNPAUSE);

    // Distinguish a hard transport error from a clean EOF, so the stream layer
    // and the byte-range LRU cache don't mistake a dropped connection or an
    // expired cloud-drive link for end-of-file (which would silently truncate
    // playback). Only signal once the ring buffer is drained -- any buffered
    // bytes must be delivered first -- and never flag a user cancel as an
    // error. Mirrors stream_lavf::fill_buffer's s->error contract.
    if (copy > 0) {
        s->error = 0;
    } else if (had_error &&
               !atomic_load_explicit(&p->aborted, memory_order_relaxed)) {
        s->error = err_code ? (int)err_code : -1;
    } else {
        s->error = 0;
    }

    return copy;
}

static bool curl_read_ready(struct stream *s)
{
    struct priv *p = s->priv;
    mp_mutex_lock(&p->mtx);
    bool ready = p->count > 0;
    mp_mutex_unlock(&p->mtx);
    return ready;
}

static int curl_seek(struct stream *s, int64_t pos)
{
    struct priv *p = s->priv;
    if (pos < 0 || (p->dash_kind && mp_dash_source_terminal(p->global)))
        return 0;
    cmd_sync(p, CMD_SEEK, pos, true);
    return 1;
}

static int curl_reconnect(struct stream *s, int64_t pos);

static int64_t curl_get_size(struct stream *s)
{
    struct priv *p = s->priv;
    mp_mutex_lock(&p->mtx);
    int64_t size = p->content_size;
    mp_mutex_unlock(&p->mtx);
    return size;
}

static int curl_control(struct stream *s, int cmd, void *arg)
{
    struct priv *p = s->priv;
    switch (cmd) {
    case STREAM_CTRL_GET_METADATA: {
        mp_mutex_lock(&p->mtx);
        struct mp_tags *tags = mp_icy_get_metadata(p->icy, s);
        mp_mutex_unlock(&p->mtx);
        if (!tags)
            break;
        *(struct mp_tags **)arg = tags;
        return STREAM_OK;
    }
    }
    return STREAM_UNSUPPORTED;
}

static void priv_destructor(void *ptr)
{
    struct priv *p = ptr;
    if (p->s->cancel)
        mp_cancel_set_cb(p->s->cancel, NULL, NULL);
    if (p->curl) {
        cmd_sync(p, CMD_REMOVE, 0, false);
        curl_easy_cleanup(p->curl);
    }
    if (p->headers)
        curl_slist_free_all(p->headers);
    mp_mutex_destroy(&p->mtx);
    mp_cond_destroy(&p->cond);
}

static void curl_close(struct stream *s)
{
    struct priv *p = s->priv;
    if (!p)
        return;
    if (s->cancel)
        mp_cancel_set_cb(s->cancel, NULL, NULL);
    if (p->curl) {
        cmd_sync(p, CMD_REMOVE, 0, false);
        curl_easy_cleanup(p->curl);
        p->curl = NULL;
    }
}

static int curl_open(stream_t *s, const struct stream_open_args *args)
{
    if (s->mode != STREAM_READ)
        return STREAM_UNSUPPORTED;
    if (!s->global || !s->global->curl) {
        MP_ERR(s, "curl backend not initialized\n");
        return STREAM_ERROR;
    }

    bool dash = !strcmp(s->url, MP_DASH_VIDEO_URL) ||
                !strcmp(s->url, MP_DASH_AUDIO_URL);
    if (!dash && !strncasecmp(s->url, "dash://", 7)) {
        MP_ERR(s, "Unsupported DASH source alias\n");
        return STREAM_ERROR;
    }
    struct curl_opts *opts = mp_get_config_group(s, s->global, &curl_conf);
    if (!opts->enabled && !dash)
        return STREAM_NO_MATCH;
    if (curl_failed(s->global->curl, s->log, MSGL_ERR))
        return STREAM_NO_MATCH;

    struct priv *p = talloc_zero(s, struct priv);
    s->priv = p;
    talloc_set_destructor(p, priv_destructor);

    p->log = s->log;
    p->global = s->global;
    p->ctx = s->global->curl;
    p->s = s;
    p->opts = talloc_steal(p, opts);
    p->net_opts = mp_get_config_group(p, s->global, &mp_network_conf);
    if (dash) {
        if (!mp_dash_source_get_track(s->global, s->url, p, &p->dash_kind,
                                      &p->dash))
            return STREAM_ERROR;
        p->url = p->dash.url;
    } else {
        p->url = normalize_url(p, s->url);
    }
    p->scheme = curl_scheme_lookup(bstr0(p->url));
    // Only supported URLs are supposed to reach here.
    mp_assert(p->scheme);
    p->content_size = -1;
    p->buffer_size = p->opts->buffer_size;
    p->buffer = talloc_size(p, p->buffer_size);
    p->icy = mp_icy_new(p);

    if (args->special_arg) {
        const struct curl_open_args *oa = args->special_arg;
        if (oa->offset > 0) {
            p->start_offset = oa->offset;
            p->request_start = oa->offset;
        }
        if (oa->end_offset > 0)
            p->request_end = oa->end_offset;
    } else if (args->open_offset > 0) {
        // Additional connection of the byte-range LRU cache: start there.
        p->start_offset = args->open_offset;
        p->request_start = args->open_offset;
    }

#ifdef MPV_DASH_TEST_HOOKS
    if (dash && !p->request_end && p->dash.allow_loopback_http &&
        !strncmp(p->url, "http://127.0.0.1:", 17))
    {
        mp_mutex_lock(&p->ctx->test_lock);
        if (p->ctx->test_window_role == p->dash_kind)
            p->request_end = p->ctx->test_window_end;
        mp_mutex_unlock(&p->ctx->test_lock);
    }
#endif

    mp_mutex_init(&p->mtx);
    mp_cond_init(&p->cond);
    p->aborted = false;

    p->curl = curl_easy_init();
    if (!p->curl) {
        MP_ERR(s, "curl_easy_init failed\n");
        return STREAM_NO_MATCH;
    }

    if (!setup_curl(p))
        return STREAM_ERROR;
    if (s->cancel)
        mp_cancel_set_cb(s->cancel, on_cancel, p);

    int64_t deadline = stream_get_io_deadline(s);
    p->open_deadline = deadline;
    cmd_sync(p, CMD_ADD, 0, false);

    mp_mutex_lock(&p->mtx);
    bool timed_out = false;
    while (!p->probed && !atomic_load_explicit(&p->aborted, memory_order_relaxed)) {
        if (deadline && mp_time_ns() >= deadline) {
            timed_out = true;
            break;
        }
        if (deadline)
            mp_cond_timedwait_until(&p->cond, &p->mtx, deadline);
        else
            mp_cond_wait(&p->cond, &p->mtx);
    }
    bool initial_ok = p->initial_probe_ok;
    bool initial_seekable = p->initial_probe_seekable;
    mp_mutex_unlock(&p->mtx);

    if (timed_out) {
        cmd_sync(p, CMD_REMOVE, 0, false);
        fail_dash_timeout(p);
        MP_WARN(s, "Network open exhausted its timeout budget.\n");
        return STREAM_ERROR;
    }
    if (!p->dash_kind && !initial_ok &&
        curl_failed(s->global->curl, s->log, MSGL_ERR))
        return STREAM_NO_MATCH;
    if (!initial_ok || atomic_load(&p->aborted) ||
        (p->dash_kind && mp_dash_source_terminal(p->global)))
        return STREAM_ERROR;

    if (p->dash_kind) {
        s->mime_type = talloc_strdup(s, p->dash_kind == MPV_DASH_TRACK_VIDEO ?
                                    "video/mp4" : "audio/mp4");
    } else {
        char *content_type = NULL;
        curl_easy_getinfo(p->curl, CURLINFO_CONTENT_TYPE, &content_type);
        bstr mime = bstr_strip(bstr_split(bstr0(content_type), ";", NULL));
        if (mime.len)
            s->mime_type = bstrto0(s, mime);
        s->server_filename = talloc_strdup(s, p->server_filename);
    }

    const char *effective_url = NULL;
    curl_easy_getinfo(p->curl, CURLINFO_EFFECTIVE_URL, &effective_url);
    s->effective_url = talloc_strdup(s, p->dash_kind ? s->url : effective_url);

    s->seekable = p->dash_kind ? initial_seekable : p->seekable;
    s->is_network = true;
    s->streaming = true;
    s->fast_skip = true;
    s->fill_buffer = curl_fill_buffer;
    s->read_ready = curl_read_ready;
    s->seek = p->seekable ? curl_seek : NULL;
    s->get_size = curl_get_size;
    s->control = curl_control;
    s->close = curl_close;
    s->pos = p->start_offset;
    // The first request already started at the requested offset (a
    // mismatching range was rejected above).
    s->open_offset_ok = args->open_offset > 0 && !args->special_arg;

    // Opt in to the byte-range LRU cache for seekable, full-file HTTP(S)
    // streams. Without it, every demuxer "yo-yo" seek -- a poorly-interleaved
    // mkv/mp4 reading audio and video from far-apart regions makes the demuxer
    // hop back and forth constantly -- tears down the transfer and starts a
    // fresh request from the ORIGINAL url. Because Emby/Jellyfin answer with a
    // 302 to a short-lived cloud-drive link, each hop re-runs the whole
    // redirect chain, so a single episode can hammer the origin with hundreds
    // of requests. The cache turns those repeated hops into in-memory lookups,
    // issuing a real backend request only when a genuinely new region is
    // touched. The stream layer makes the final call based on seekability, a
    // known file size and --stream-lru-cache.
    //
    // Recovery reopens the original URL and validates the new ranged response.
    // Skip capped sub-range opens (e.g. nested HLS byte-range segments):
    // those are short-lived, read forward and would only waste a cache buffer
    // each. FTP pays no redirect cost, so it is left out too.
    if (!p->dash_kind && s->mode == STREAM_READ && s->seekable && p->request_end == 0 &&
        p->scheme->proto == MP_CURL_PROTO_HTTP)
    {
        s->reconnect = curl_reconnect;
        s->wants_lru_cache = true;
    }

    return STREAM_OK;
}

static int curl_reconnect(struct stream *s, int64_t pos)
{
    curl_close(s);
    talloc_free(s->priv);
    s->priv = NULL;
    struct stream_open_args args = {.open_offset = pos};
    return curl_open(s, &args);
}

static bool curl_has_proto(bstr scheme)
{
    curl_version_info_data *info = curl_version_info(CURLVERSION_NOW);
    mp_require(info && info->protocols);
    return bstr_in_list0(scheme, (char **)info->protocols);
}

static char **curl_get_protocols(void)
{
    int num = 0;
    char **protocols = NULL;
    for (int i = 0; i < MP_ARRAY_SIZE(curl_schemes); i++) {
        bstr scheme = curl_schemes[i].scheme;
        if (curl_has_proto(scheme))
            MP_TARRAY_APPEND(NULL, protocols, num, bstrdup0(protocols, scheme));
    }
    MP_TARRAY_APPEND(NULL, protocols, num, NULL);
    return protocols;
}

const stream_info_t stream_info_curl = {
    .name = "curl",
    .open2 = curl_open,
    .get_protocols = curl_get_protocols,
    .stream_origin = STREAM_ORIGIN_NET,
};

const stream_info_t stream_info_dash = {
    .name = "dash",
    .open2 = curl_open,
    .protocols = (const char *const[]){"dash", NULL},
    .stream_origin = STREAM_ORIGIN_NET,
};

// FFmpeg AVIOContext implementation
// Allows demuxers to use our stream_curl in nested io and sub-demuxers. This
// should route all traffic through our implementation.

struct curl_avio_cookie {
    const AVClass *av_class;
    struct stream *stream;
    struct mp_cancel *cancel;
    const char *location; // final URL after redirects, exposed via the "location" opt
    AVIOContext *transport;
};

static const AVClass curl_avio_cookie_class = {
    .class_name = "mpv_curl_avio",
    .item_name  = av_default_item_name,
    .option     = (const AVOption[]) {
        {"location", "The actual location of the data received",
         offsetof(struct curl_avio_cookie, location),
         AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, AV_OPT_FLAG_DECODING_PARAM},
        {0}
    },
    .version = LIBAVUTIL_VERSION_INT,
};

static void *curl_avio_child_next(void *obj, void *prev)
{
    AVIOContext *pb = obj;
    return prev ? NULL : pb->opaque; // the cookie
}

static const AVClass curl_avio_class = {
    .class_name = "mpv_curl_avio",
    .item_name  = av_default_item_name,
    .child_next = curl_avio_child_next,
    .version    = LIBAVUTIL_VERSION_INT,
};

static bool is_protocol_allowed(struct mp_log *log, bstr scheme,
                                const char *whitelist, const char *blacklist)
{
    // `scheme` is required to be wrapped null-terminated string literal.
    // This is UB otherwise, see curl_schemes.
    mp_assert(scheme.len && scheme.start[scheme.len] == '\0');
    if (whitelist && av_match_list(scheme.start, whitelist, ',') <= 0) {
        mp_err(log, "Protocol '%.*s' not on whitelist '%s'!\n", BSTR_P(scheme), whitelist);
        return false;
    }
    if (blacklist && av_match_list(scheme.start, blacklist, ',') > 0) {
        mp_err(log, "Protocol '%.*s' on blacklist '%s'!\n", BSTR_P(scheme), blacklist);
        return false;
    }
    return true;
}

static int curl_avio_read(void *opaque, uint8_t *buf, int size)
{
    struct curl_avio_cookie *c = opaque;
    int ret = stream_read_partial(c->stream, buf, size);
    return ret > 0 ? ret : AVERROR_EOF;
}

static int64_t curl_avio_seek(void *opaque, int64_t pos, int whence)
{
    struct curl_avio_cookie *c = opaque;
    if (whence == AVSEEK_SIZE) {
        int64_t end = stream_get_size(c->stream);
        return end >= 0 ? end : AVERROR(ENOSYS);
    }
    if (whence == SEEK_END) {
        int64_t end = stream_get_size(c->stream);
        if (end < 0)
            return AVERROR(EINVAL);
        pos += end;
    } else if (whence == SEEK_CUR) {
        pos += stream_tell(c->stream);
    } else if (whence != SEEK_SET) {
        return AVERROR(EINVAL);
    }
    if (pos < 0)
        return AVERROR(EINVAL);
    if (!stream_seek(c->stream, pos))
        return AVERROR(EIO);
    return pos;
}

static int open_curl_transport(struct demuxer *demuxer, AVIOContext **pb_out,
                               void **cookie_out, const char *url, int flags,
                               AVDictionary **options,
                               const char *whitelist, const char *blacklist)
{
    *pb_out = NULL;
    *cookie_out = NULL;

    if (flags & AVIO_FLAG_WRITE)
        return AVERROR(ENOSYS);

    // Check protocol early, to return ENOSYS and allow lavf to fallback.
    const struct curl_scheme *cs = curl_scheme_lookup(bstr0(url));
    if (!cs || !curl_has_proto(cs->scheme))
        return AVERROR(ENOSYS);

    struct curl_opts *opts = mp_get_config_group(NULL, demuxer->global, &curl_conf);
    bool enabled = opts->enabled;
    talloc_free(opts);
    if (!enabled)
        return AVERROR(ENOSYS);

    // The context is required to be initialized in global.
    mp_require(demuxer->global && demuxer->global->curl);
    if (curl_failed(demuxer->global->curl, demuxer->log, MSGL_V))
        return AVERROR(ENOSYS);

    struct curl_open_args oa = {0};
    if (options && *options) {
        AVDictionaryEntry *e;
        // lavf's http demuxer exposes initial/final byte offsets as AVOptions
        // Some demuxers, like lavf/hls.c assume it is always available, even for
        // custom IO... Add support for this.
        if ((e = av_dict_get(*options, "offset", NULL, 0)))
            oa.offset = strtoll(e->value, NULL, 10);
        if ((e = av_dict_get(*options, "end_offset", NULL, 0)))
            oa.end_offset = strtoll(e->value, NULL, 10);
    }

    if (!is_protocol_allowed(demuxer->log, cs->scheme, whitelist, blacklist))
        return AVERROR(EINVAL);

    // Each nested stream gets its own mp_cancel slaved to the main demuxer,
    // so the http backend can install its own wake-up callback without
    // clobbering the top-level stream or any sibling nested stream.
    struct mp_cancel *cancel = mp_cancel_new(NULL);
    mp_cancel_set_parent(cancel, demuxer->cancel);

    struct stream_open_args args = {
        .global = demuxer->global,
        .cancel = cancel,
        .url = url,
        .flags = STREAM_READ | (demuxer->stream_origin & STREAM_ORIGIN_MASK),
        .sinfo = &stream_info_curl,
        .special_arg = &oa,
    };

    struct stream *s = NULL;
    int r = stream_create_with_args(&args, &s);
    if (r != STREAM_OK || !s) {
        talloc_free(cancel);
        return AVERROR(EIO);
    }

    struct curl_avio_cookie *c = talloc_zero(NULL, struct curl_avio_cookie);
    c->av_class = &curl_avio_cookie_class;
    c->stream = s;
    c->cancel = cancel;
    c->location = s->effective_url ? s->effective_url : s->url;

    void *buffer = av_malloc(64 * 1024);
    if (!buffer) {
        free_stream(s);
        talloc_free(cancel);
        talloc_free(c);
        return AVERROR(ENOMEM);
    }

    AVIOContext *pb = avio_alloc_context(buffer, 64 * 1024, 0, c,
                                         curl_avio_read, NULL,
                                         s->seekable ? curl_avio_seek : NULL);
    if (!pb) {
        av_free(buffer);
        free_stream(s);
        talloc_free(cancel);
        talloc_free(c);
        return AVERROR(ENOMEM);
    }
    pb->seekable = s->seekable ? AVIO_SEEKABLE_NORMAL : 0;
    pb->av_class = &curl_avio_class;
    pb->pos = oa.offset;

    *pb_out = pb;
    *cookie_out = c;
    return 0;
}

static void close_curl_transport(AVIOContext *pb, struct curl_avio_cookie *c)
{
    av_freep(&pb->buffer);
    avio_context_free(&pb);
    free_stream(c->stream);
    talloc_free(c->cancel);
    talloc_free(c);
}

// Open a `crypto+...` URL by opening the inner URL with curl and layering
// AES-128-CBC decryption on top using the `key`/`iv` hex strings from the
// AVDictionary. Returns AVERROR(ENOSYS) when curl can't handle the inner or
//the AES options are missing.
static int open_curl_crypto(struct demuxer *demuxer, AVIOContext **pb_out,
                            void **cookie_out, const char *inner_url,
                            int flags, AVDictionary **options,
                            const char *whitelist, const char *blacklist)
{
    if (flags & AVIO_FLAG_WRITE)
        return AVERROR(ENOSYS);
    if (!options || !*options)
        return AVERROR(ENOSYS);

    AVDictionaryEntry *key_e = av_dict_get(*options, "key", NULL, 0);
    AVDictionaryEntry *iv_e = av_dict_get(*options, "iv", NULL, 0);
    if (!key_e || !iv_e)
        return AVERROR(ENOSYS);

    void *tmp = talloc_new(NULL);
    AVIOContext *transport = NULL;
    void *cookie = NULL;
    AVIOContext *wrapper = NULL;

    bstr key = {0}, iv = {0};
    bstr_decode_hex(tmp, bstr0(key_e->value), &key);
    bstr_decode_hex(tmp, bstr0(iv_e->value), &iv);

    int r = open_curl_transport(demuxer, &transport, &cookie, inner_url, flags,
                                options, whitelist, blacklist);
    if (r < 0)
        goto done;

    r = mp_avio_crypto_open(&wrapper, transport, key, iv);
    if (r < 0) {
        MP_ERR(demuxer, "Failed to set up crypto stream: %s\n", av_err2str(r));
        close_curl_transport(transport, cookie);
        goto done;
    }

    // Consume from the dict so demuxer-side mp_avdict_print_unset stays quiet.
    av_dict_set(options, "key", NULL, 0);
    av_dict_set(options, "iv", NULL, 0);

    ((struct curl_avio_cookie *)cookie)->transport = transport;
    *pb_out = wrapper;
    *cookie_out = cookie;

done:
    talloc_free(tmp);
    return r;
}

int mp_curl_avio_open(struct demuxer *demuxer, AVIOContext **pb_out,
                      void **cookie_out, const char *url, int flags,
                      AVDictionary **options,
                      const char *whitelist, const char *blacklist)
{
    *pb_out = NULL;
    *cookie_out = NULL;

    if (flags & AVIO_FLAG_WRITE)
        return AVERROR(ENOSYS);

    // Nested IO plumbs whitelist/blacklist through the AVDictionary, use that
    // if set, same as FFmpeg's implementation.
    if (options && *options) {
        AVDictionaryEntry *e;
        if ((e = av_dict_get(*options, "protocol_whitelist", NULL, 0)))
            whitelist = e->value;
        if ((e = av_dict_get(*options, "protocol_blacklist", NULL, 0)))
            blacklist = e->value;
    }

    bstr rest = bstr0(url);
    if (bstr_eatstart0(&rest, "crypto+") || bstr_eatstart0(&rest, "crypto:")) {
        if (!is_protocol_allowed(demuxer->log, (bstr)bstr0_lit("crypto"), whitelist, blacklist))
            return AVERROR(EINVAL);
        return open_curl_crypto(demuxer, pb_out, cookie_out, rest.start,
                                flags, options, whitelist, blacklist);
    }

    return open_curl_transport(demuxer, pb_out, cookie_out, url, flags,
                               options, whitelist, blacklist);
}

void mp_curl_avio_close(AVIOContext *pb, void *cookie)
{
    struct curl_avio_cookie *c = cookie;
    if (!c)
        return;

    AVIOContext *transport = c->transport;
    if (transport) {
        mp_avio_crypto_close(&pb);
    } else {
        transport = pb;
    }
    close_curl_transport(transport, c);
}
