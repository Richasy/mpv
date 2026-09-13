/*
 * This file is part of mpv.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mpv/client.h>

#include "libmpv_common.h"

#define STATUS_OBSERVER_ID 0x53544254

static char observed_status[1024];

static void reset_status_observation(void)
{
    observed_status[0] = '\0';
}

static void record_status_event(mpv_event *event)
{
    if (event->event_id != MPV_EVENT_PROPERTY_CHANGE ||
        event->reply_userdata != STATUS_OBSERVER_ID)
    {
        return;
    }
    mpv_event_property *property = event->data;
    if (!property || property->format != MPV_FORMAT_STRING ||
        !property->data)
    {
        return;
    }
    char *value = *(char **)property->data;
    snprintf(observed_status, sizeof(observed_status), "%s",
             value ? value : "");
}

static char *get_string(const char *name)
{
    char *value = mpv_get_property_string(ctx, name);
    if (!value)
        fail("Could not read property %s.\n", name);
    return value;
}

static int64_t get_int64(const char *name)
{
    char *text = get_string(name);
    if (strcmp(text, "no") == 0 || strcmp(text, "auto") == 0) {
        mpv_free(text);
        return -2;
    }
    const char *number = text[0] == '(' ? text + 1 : text;
    int64_t value = strtoll(number, NULL, 10);
    mpv_free(text);
    return value;
}

static void set_track_id(const char *name, int64_t value)
{
    char text[32];
    snprintf(text, sizeof(text), "%"PRId64, value);
    set_property_string(name, text);
}

static void wait_for_file_loaded(void)
{
    while (true) {
        mpv_event *event = wrap_wait_event();
        record_status_event(event);
        if (event->event_id == MPV_EVENT_FILE_LOADED)
            return;
    }
}

static char *wait_for_status(const char *state)
{
    char expected[64];
    snprintf(expected, sizeof(expected), "\"state\":\"%s\"", state);
    for (int attempt = 0; attempt < 200; attempt++) {
        if (strstr(observed_status, expected))
            return get_string("sub-translate-status");
        record_status_event(mpv_wait_event(ctx, 0.05));
    }
    char *status = get_string("sub-translate-status");
    fail("Timed out waiting for state %s: %s\n", state, status);
}

static void wait_for_translated_count(int count)
{
    char expected[64];
    snprintf(expected, sizeof(expected), "\"translated\":%d", count);
    for (int attempt = 0; attempt < 200; attempt++) {
        if (strstr(observed_status, expected))
            return;
        record_status_event(mpv_wait_event(ctx, 0.05));
    }
    fail("Timed out waiting for %d translated cues.\n", count);
}

static void advance_to(double target)
{
    int paused = 0;
    mpv_set_property(ctx, "pause", MPV_FORMAT_FLAG, &paused);
    for (int attempt = 0; attempt < 200; attempt++) {
        double time = 0;
        if (mpv_get_property(ctx, "time-pos", MPV_FORMAT_DOUBLE, &time) >= 0 &&
            time >= target)
        {
            paused = 1;
            mpv_set_property(ctx, "pause", MPV_FORMAT_FLAG, &paused);
            return;
        }
        record_status_event(mpv_wait_event(ctx, 0.05));
    }
    fail("Timed out advancing playback to %.3f.\n", target);
}

static void seek_to_start(void)
{
    reset_status_observation();
    command(((const char *[]){"seek", "0", "absolute+exact", NULL}));
    while (true) {
        mpv_event *event = wrap_wait_event();
        record_status_event(event);
        if (event->event_id == MPV_EVENT_PLAYBACK_RESTART)
            return;
    }
}

static void load_file(const char *path)
{
    command(((const char *[]){"loadfile", path, NULL}));
    wait_for_file_loaded();
}

static struct mpv_node *map_value(struct mpv_node *map, const char *key)
{
    if (!map || map->format != MPV_FORMAT_NODE_MAP)
        return NULL;
    for (int n = 0; n < map->u.list->num; n++) {
        if (strcmp(map->u.list->keys[n], key) == 0)
            return &map->u.list->values[n];
    }
    return NULL;
}

static void verify_current_cue(const char *expected,
                               double expected_start,
                               double expected_end)
{
    bool ready = false;
    for (int attempt = 0; attempt < 200; attempt++) {
        char *text = mpv_get_property_string(ctx, "secondary-sub-text");
        ready = text && strcmp(text, expected) == 0;
        mpv_free(text);
        if (ready) {
            double start;
            double end;
            get_property("secondary-sub-start", MPV_FORMAT_DOUBLE, &start);
            get_property("secondary-sub-end", MPV_FORMAT_DOUBLE, &end);
            if (start != expected_start || end != expected_end)
                fail("Translated cue timing changed.\n");
            return;
        }
        mpv_wait_event(ctx, 0.05);
    }
    char *status = get_string("sub-translate-status");
    char *time = get_string("time-pos");
    char *actual = mpv_get_property_string(ctx, "secondary-sub-text");
    fail("Translated cue '%s' was unavailable "
         "(actual=%s status=%s time=%s secondary=%"PRId64").\n",
         expected, actual ? actual : "(null)", status, time,
         get_int64("secondary-sid"));
}

static void verify_whisper_stayed_off(void)
{
    int loading = 1;
    get_property("whisper-loading", MPV_FORMAT_FLAG, &loading);
    if (loading)
        fail("Text subtitle translation started Whisper.\n");
    char *option = get_string("whisper-lookahead");
    if (option[0])
        fail("Text subtitle translation changed whisper-lookahead.\n");
    mpv_free(option);
}

static void configure(const char *endpoint)
{
    char config[2048];
    snprintf(
        config, sizeof(config),
        "{\"provider\":\"ai\",\"source_lang\":\"auto\","
        "\"target_lang\":\"zh\",\"ai\":{\"endpoint\":\"%s\","
        "\"model\":\"fixture\",\"api_key\":\"fixture-secret\","
        "\"source_lang\":\"auto\",\"target_lang\":\"zh\","
        "\"system_prompt\":\"translate\",\"context_size\":0,"
        "\"max_tokens\":64,\"timeout_ms\":2000},"
        "\"limits\":{\"enabled\":true,\"horizon_sec\":60,"
        "\"seek_debounce_ms\":0,\"min_text_chars\":2,"
        "\"reuse_cache_capacity\":32,"
        "\"reuse_cache_window_ms\":60000,"
        "\"repeat_loop_threshold\":3,"
        "\"repeat_loop_window_ms\":30000,"
        "\"rpm_limit\":0,\"session_request_limit\":0}}",
        endpoint);
    set_property_string("sub-translate-config", config);
    char *masked = get_string("sub-translate-config");
    if (strstr(masked, "fixture-secret") || !strstr(masked, "***"))
        fail("sub-translate-config exposed its API key.\n");
    char *before_invalid = malloc(strlen(masked) + 1);
    if (!before_invalid)
        fail("Could not allocate config snapshot.\n");
    strcpy(before_invalid, masked);
    mpv_free(masked);
    if (mpv_set_property_string(
            ctx, "sub-translate-config",
            "{\"provider\":\"invalid\",\"target_lang\":\"zh\"}") >= 0)
    {
        fail("Invalid common translation config was accepted.\n");
    }
    masked = get_string("sub-translate-config");
    if (strcmp(masked, before_invalid) != 0)
        fail("Rejected config changed the active translation config.\n");
    free(before_invalid);
    mpv_free(masked);
}

static void test_embedded(const char *path, const char *endpoint)
{
    load_file(path);
    int64_t source_sid = get_int64("sid");
    int enabled = 1;
    reset_status_observation();
    if (mpv_set_property(ctx, "sub-translate", MPV_FORMAT_FLAG,
                         &enabled) < 0)
    {
        fail("Could not enable text subtitle translation.\n");
    }
    int paused = 0;
    mpv_set_property(ctx, "pause", MPV_FORMAT_FLAG, &paused);
    char *status = wait_for_status("active");
    if (strstr(status, "fixture-secret") ||
        strstr(status, "http://") || strstr(status, "foo"))
    {
        fail("sub-translate-status exposed private or subtitle content.\n");
    }
    mpv_free(status);
    wait_for_translated_count(2);
    paused = 1;
    mpv_set_property(ctx, "pause", MPV_FORMAT_FLAG, &paused);
    if (get_int64("sid") != source_sid)
        fail("Translation replaced the selected primary subtitle.\n");
    int64_t output_sid = get_int64("secondary-sid");
    if (output_sid < 1 || output_sid == source_sid)
        fail("Translated output was not selected as an owned companion.\n");
    verify_current_cue("translated:foo", 0.0, 1.0);
    advance_to(1.1);
    verify_current_cue("translated:bar", 1.0, 2.0);
    seek_to_start();
    wait_for_translated_count(2);
    verify_current_cue("translated:foo", 0.0, 1.0);
    reset_status_observation();
    configure(endpoint);
    status = wait_for_status("active");
    mpv_free(status);
    wait_for_translated_count(2);
    verify_current_cue("translated:foo", 0.0, 1.0);
    verify_whisper_stayed_off();

    enabled = 0;
    reset_status_observation();
    mpv_set_property(ctx, "sub-translate", MPV_FORMAT_FLAG, &enabled);
    status = wait_for_status("disabled");
    mpv_free(status);
    if (get_int64("sid") != source_sid ||
        get_int64("secondary-sid") != -2)
    {
        fail("Disabling translation changed the source or kept its output.\n");
    }
    char *config = get_string("sub-translate-config");
    if (!config[0])
        fail("Disabling text processing cleared common configuration.\n");
    mpv_free(config);

}

static void test_external(const char *video, const char *subtitle)
{
    load_file(video);
    command(((const char *[]){"sub-add", subtitle, "select", NULL}));
    int enabled = 1;
    reset_status_observation();
    mpv_set_property(ctx, "sub-translate", MPV_FORMAT_FLAG, &enabled);
    int paused = 0;
    mpv_set_property(ctx, "pause", MPV_FORMAT_FLAG, &paused);
    char *status = wait_for_status("active");
    mpv_free(status);
    wait_for_translated_count(2);
    paused = 1;
    mpv_set_property(ctx, "pause", MPV_FORMAT_FLAG, &paused);
    verify_current_cue("translated:foo", 0.0, 1.0);
    advance_to(1.1);
    verify_current_cue("translated:bar", 1.0, 2.0);
    verify_whisper_stayed_off();
    enabled = 0;
    reset_status_observation();
    mpv_set_property(ctx, "sub-translate", MPV_FORMAT_FLAG, &enabled);
    status = wait_for_status("disabled");
    mpv_free(status);
}

static void test_bitmap(const char *video, const char *bitmap)
{
    load_file(video);
    command(((const char *[]){"sub-add", bitmap, "select", NULL}));
    int enabled = 1;
    reset_status_observation();
    mpv_set_property(ctx, "sub-translate", MPV_FORMAT_FLAG, &enabled);
    char *status = wait_for_status("unsupported");
    if (!strstr(status, "bitmap-based") ||
        !strstr(status, "\"output_sid\":null") ||
        !strstr(status, "\"translated\":0") ||
        !strstr(status, "\"pending\":0"))
        fail("Bitmap subtitles did not report unsupported status: %s\n",
             status);
    mpv_free(status);
    enabled = 0;
    reset_status_observation();
    mpv_set_property(ctx, "sub-translate", MPV_FORMAT_FLAG, &enabled);
    status = wait_for_status("disabled");
    mpv_free(status);
}

static int count_occurrences(const char *text, const char *needle)
{
    int count = 0;
    size_t length = strlen(needle);
    while ((text = strstr(text, needle))) {
        count++;
        text += length;
    }
    return count;
}

static void test_ass_dialogue(const char *video, const char *subtitle)
{
    load_file(video);
    command(((const char *[]){"sub-add", subtitle, "select", NULL}));
    int enabled = 1;
    reset_status_observation();
    mpv_set_property(ctx, "sub-translate", MPV_FORMAT_FLAG, &enabled);
    int paused = 0;
    mpv_set_property(ctx, "pause", MPV_FORMAT_FLAG, &paused);
    wait_for_translated_count(4);
    advance_to(1.2);

    char *text = get_string("secondary-sub-text");
    if (!strstr(text, "translated:Hello\nworld") ||
        !strstr(text, "translated:I") ||
        count_occurrences(text, "translated:Repeat") != 2 ||
        strstr(text, "{\\i"))
    {
        fail("ASS dialogue translation lost formatting isolation, short "
             "text, repetition, or overlap: %s\n", text);
    }
    mpv_free(text);
    double start;
    double end;
    get_property("secondary-sub-start", MPV_FORMAT_DOUBLE, &start);
    get_property("secondary-sub-end", MPV_FORMAT_DOUBLE, &end);
    if (start != 0.0 || end != 3.0)
        fail("Overlapping ASS cue times changed.\n");

    enabled = 0;
    reset_status_observation();
    mpv_set_property(ctx, "sub-translate", MPV_FORMAT_FLAG, &enabled);
    char *status = wait_for_status("disabled");
    mpv_free(status);
}

static void find_two_subtitles(int64_t *first, int64_t *second)
{
    struct mpv_node tracks = {0};
    get_property("track-list", MPV_FORMAT_NODE, &tracks);
    *first = *second = -1;
    for (int n = 0; n < tracks.u.list->num; n++) {
        struct mpv_node *track = &tracks.u.list->values[n];
        struct mpv_node *type = map_value(track, "type");
        struct mpv_node *id = map_value(track, "id");
        if (!type || !id || type->format != MPV_FORMAT_STRING ||
            id->format != MPV_FORMAT_INT64 ||
            strcmp(type->u.string, "sub") != 0)
        {
            continue;
        }
        if (*first < 0)
            *first = id->u.int64;
        else if (*second < 0)
            *second = id->u.int64;
    }
    mpv_free_node_contents(&tracks);
    if (*first < 0 || *second < 0)
        fail("Dual-subtitle fixture did not expose two text tracks.\n");
}

static void test_secondary_conflict(const char *path)
{
    load_file(path);
    int64_t primary;
    int64_t secondary;
    find_two_subtitles(&primary, &secondary);
    set_track_id("sid", primary);
    set_track_id("secondary-sid", secondary);
    int enabled = 1;
    reset_status_observation();
    mpv_set_property(ctx, "sub-translate", MPV_FORMAT_FLAG, &enabled);
    char *status = wait_for_status("error");
    if (!strstr(status, "secondary subtitle track is already selected") ||
        !strstr(status, "\"output_sid\":null") ||
        !strstr(status, "\"translated\":0") ||
        !strstr(status, "\"pending\":0"))
        fail("Secondary conflict was not explicit: %s\n", status);
    mpv_free(status);
    if (get_int64("sid") != primary ||
        get_int64("secondary-sid") != secondary)
    {
        fail("Translation hijacked a manually selected secondary track.\n");
    }
    enabled = 0;
    reset_status_observation();
    mpv_set_property(ctx, "sub-translate", MPV_FORMAT_FLAG, &enabled);
    status = wait_for_status("disabled");
    mpv_free(status);
}

int main(int argc, char **argv)
{
    if (argc != 8)
        return 1;
    ctx = mpv_create();
    if (!ctx)
        return 1;
    atexit(exit_cleanup);
    set_property_string("config", "no");
    set_property_string("terminal", "no");
    set_property_string("load-scripts", "no");
    set_property_string("pause", "yes");
    set_property_string("speed", "0.5");
    initialize();
    mpv_request_log_messages(ctx, "warn");
    if (mpv_observe_property(
            ctx, STATUS_OBSERVER_ID,
            "sub-translate-status", MPV_FORMAT_STRING) < 0)
    {
        fail("Could not observe sub-translate-status.\n");
    }
    char *initial_status = wait_for_status("disabled");
    mpv_free(initial_status);
    configure(argv[1]);
    test_embedded(argv[2], argv[1]);
    test_external(argv[3], argv[4]);
    test_bitmap(argv[3], argv[5]);
    test_secondary_conflict(argv[6]);
    test_ass_dialogue(argv[3], argv[7]);
    command_string("quit");
    while (wrap_wait_event()->event_id != MPV_EVENT_SHUTDOWN) {}
    return 0;
}
