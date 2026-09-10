/*
 * This file is part of mpv.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "libmpv_common.h"
#include "osdep/timer.h"

struct diagnostics {
    bool opened;
    bool sought;
    bool read_tail;
    bool pending;
    bool cancelled;
    bool failed;
    bool numeric_redacted;
    bool stop_requested;
    int pending_count;
    uint64_t cancel_operation_id;
    int64_t cancel_started;
};

static int64_t tail_offset;

static void optional_option(const char *name, const char *value)
{
    int result = mpv_set_option_string(ctx, name, value);
    if (result < 0 && result != MPV_ERROR_OPTION_NOT_FOUND)
        fail("Could not configure optional native capability %s.\n", name);
}

static void create_client(void)
{
    ctx = mpv_create();
    if (!ctx)
        fail("Could not create native client.\n");
    set_property_string("config", "no");
    set_property_string("terminal", "no");
    optional_option("load-scripts", "no");
    set_property_string("http-proxy", "");
    set_property_string("http-header-fields", "Authorization: Bearer test-secret");
    set_property_string("network-timeout", "30");
    optional_option("curl-enabled", "no");
    set_property_string("stream-lavf-o",
                        "reconnect=0,reconnect_on_network_error=0,multiple_requests=0");
    // Keep the fixture's tail outside both header buffering and tail prefetch.
    set_property_string("stream-buffer-size", "16384");
    set_property_string("stream-lru-cache-tail-prefetch", "0");
    initialize();
}

static bool at_tail(const char *text)
{
    const char *field = strstr(text, " offset=");
    return field && strtoll(field + 8, NULL, 10) == tail_offset;
}

static void observe(mpv_event_log_message *msg, struct diagnostics *d,
                    const char *mode)
{
    const char *text = msg->text;
    if (strncmp(text, "network_io ", 11))
        return;
    if (strstr(text, "test-secret") || strstr(text, "http://") ||
        strstr(text, "127.0.0.1") || strstr(text, "Authorization") ||
        strstr(text, "private-timeout"))
        fail("Network diagnostics retained private request input.\n");
    fputs(text, stdout);

    bool end = strstr(text, "network_io end ") != NULL;
    bool tail = at_tail(text);
    d->opened |= end && strstr(text, "operation=open ") &&
                 strstr(text, " result=0 ");
    if (end && tail && strstr(text, "operation=seek ")) {
        const char *result = strstr(text, " result=");
        d->sought |= result && strtoll(result + 8, NULL, 10) == tail_offset;
    }
    if (end && tail && strstr(text, "operation=read ")) {
        const char *result = strstr(text, " result=");
        d->read_tail |= result && strtoll(result + 8, NULL, 10) > 0;
    }
    d->failed |= end && strstr(text, "operation=open ") &&
                 strstr(text, " result=-") && strstr(text, "403");
    d->numeric_redacted |= strstr(text, "timeout_us=non-numeric") != NULL;
    if (end && tail && strstr(text, " cancelled=1 ") &&
        strstr(text, "operation=read ")) {
        const char *id = strstr(text, " id=");
        const char *result = strstr(text, " result=");
        if (!d->stop_requested || !id || !result ||
            strtoull(id + 4, NULL, 10) != d->cancel_operation_id ||
            strtoll(result + 8, NULL, 10) >= 0 ||
            mp_time_ns() - d->cancel_started >= MP_TIME_S_TO_NS(5))
            fail("The blocked tail read did not promptly return cancellation.\n");
        if (msg->log_level < MPV_LOG_LEVEL_V)
            fail("Expected cancellation was reported as a warning/error.\n");
        d->cancelled = true;
    }
    if (strstr(text, "network_io pending ")) {
        d->pending_count++;
        if (!tail || msg->log_level != MPV_LOG_LEVEL_WARN ||
            d->pending_count > 3)
            fail("Invalid pending network operation diagnostic.\n");
        const char *elapsed = strstr(text, " elapsed_ms=");
        if (!elapsed || strtod(elapsed + 12, NULL) < 15000)
            fail("Pending diagnostic appeared before its threshold.\n");
        d->pending = true;
        if (!strcmp(mode, "stall")) {
            puts("RELEASE_TAIL");
            fflush(stdout);
        } else if (!strcmp(mode, "cancel") && !d->stop_requested) {
            const char *id = strstr(text, " id=");
            if (!id || !strstr(text, "operation=read "))
                fail("Cancellation did not target a blocked tail read.\n");
            d->stop_requested = true;
            d->cancel_operation_id = strtoull(id + 4, NULL, 10);
            d->cancel_started = mp_time_ns();
            puts("CANCEL_REQUESTED");
            fflush(stdout);
            command_string("stop");
        }
    }
}

static void run_case(const char *base, const char *mode)
{
    char url[1024];
    snprintf(url, sizeof(url), "%s/%s?api_key=test-secret", base, mode);
    if (!strcmp(mode, "numeric"))
        set_property_string("stream-lavf-o", "timeout=private-timeout");
    command(((const char *[]){"loadfile", url, NULL}));

    struct diagnostics d = {0};
    bool loaded = false;
    bool ended = false;
    bool expect_end = !strcmp(mode, "cancel") || !strcmp(mode, "denied") ||
                      !strcmp(mode, "numeric");
    while (!(expect_end ? ended : loaded)) {
        mpv_event *event = mpv_wait_event(ctx, 1);
        if (event->event_id == MPV_EVENT_LOG_MESSAGE)
            observe(event->data, &d, mode);
        if (event->event_id == MPV_EVENT_FILE_LOADED)
            loaded = true;
        if (event->event_id == MPV_EVENT_END_FILE) {
            ended = true;
            if (!expect_end && !loaded)
                fail("Fixture ended before it loaded (%s).\n", mode);
        }
    }
    while (true) {
        mpv_event *event = mpv_wait_event(ctx, 0);
        if (event->event_id == MPV_EVENT_NONE)
            break;
        if (event->event_id == MPV_EVENT_LOG_MESSAGE)
            observe(event->data, &d, mode);
    }
    if (expect_end) {
        if (loaded)
            fail("Cancelled/failed open unexpectedly loaded (%s).\n", mode);
        if (!strcmp(mode, "cancel") && !d.cancelled)
            fail("Missing native cancellation result.\n");
        if (!strcmp(mode, "denied") && !d.failed)
            fail("Missing native HTTP failure result.\n");
        if (!strcmp(mode, "numeric") && !d.numeric_redacted)
            fail("Non-numeric option input was not redacted.\n");
    } else if (!d.opened || !d.sought || !d.read_tail ||
               (!strcmp(mode, "stall") && !d.pending)) {
        fail("Missing instance-owned diagnostics (%s): open=%d seek=%d "
             "tail_read=%d pending=%d expected_offset=%"PRId64".\n",
             mode, d.opened, d.sought, d.read_tail, d.pending, tail_offset);
    }
    exit_cleanup();
    if (!strcmp(mode, "cancel") &&
        mp_time_ns() - d.cancel_started >= MP_TIME_S_TO_NS(5))
        fail("Native client destruction exceeded the cancellation budget.\n");
    printf("PASS %s: open=%d seek=%d tail_read=%d pending=%d cancel=%d http_error=%d\n",
           mode, d.opened, d.sought, d.read_tail, d.pending_count,
           d.cancelled, d.failed);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    if (argc != 3)
        return 1;
    mp_time_init();
    tail_offset = strtoll(argv[2], NULL, 10);
    atexit(exit_cleanup);

    create_client();
    run_case(argv[1], "healthy");

    // Exercise stream logging after the process-global FFmpeg log owner exits.
    create_client();
    mpv_handle *old_owner = ctx;
    create_client();
    mpv_terminate_destroy(old_owner);
    run_case(argv[1], "healthy");

    const char *modes[] = {"stall", "cancel", "denied", "numeric"};
    for (size_t n = 0; n < sizeof(modes) / sizeof(modes[0]); n++) {
        create_client();
        run_case(argv[1], modes[n]);
    }
    return 0;
}
