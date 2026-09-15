/*
 * This file is part of mpv.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <mpv/client.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define PATH_CAP 4096
#define UTF8_PATH_CAP (PATH_CAP * 3)
#define SELFTEST_TIMEOUT_MS 120000ULL
#define OPERATION_TIMEOUT_MS 8000ULL
#define SCREENSHOT_TIMEOUT_MS 2500ULL
#define PROBE_TIME_US INT64_C(2000000)
#define MIN_MOVEMENT_PX 8
#define FNV_OFFSET UINT64_C(14695981039346656037)
#define FNV_PRIME UINT64_C(1099511628211)

// Full-resolution chroma keeps a stack's placement on the video's chroma grid
// from changing the shape compared with an independently rendered reference.
static const char video_url[] =
    "av://lavfi:color=c=black:s=640x360:r=30:d=12,format=yuv444p";

enum fixture_id {
    FIXTURE_PRIMARY_SRT,
    FIXTURE_SECONDARY_SRT,
    FIXTURE_PRIMARY_INLINE_TOP_SRT,
    FIXTURE_PRIMARY_GLOBAL_TOP_SRT,
    FIXTURE_ASS_BOTTOM,
    FIXTURE_ASS_TOP,
    FIXTURE_ASS_POSITION,
    FIXTURE_ASS_MOVE,
    FIXTURE_COUNT,
    FIXTURE_NONE = -1,
};

static const char primary_srt[] = "1\r\n"
                                  "00:00:00,000 --> 00:00:08,000\r\n"
                                  "Primary plain sample\r\n";

static const char secondary_srt[] = "1\r\n"
                                    "00:00:00,000 --> 00:00:08,000\r\n"
                                    "Secondary plain sample\r\n";

static const char primary_inline_top_srt[] =
    "1\r\n"
    "00:00:00,000 --> 00:00:08,000\r\n"
    "{\\an8}Primary inline top sample\r\n";

static const char primary_global_top_srt[] = "1\r\n"
                                             "00:00:00,000 --> 00:00:08,000\r\n"
                                             "Primary global top sample\r\n";

static const char ass_header[] =
    "[Script Info]\r\n"
    "ScriptType: v4.00+\r\n"
    "PlayResX: 640\r\n"
    "PlayResY: 360\r\n"
    "ScaledBorderAndShadow: yes\r\n"
    "WrapStyle: 2\r\n"
    "\r\n"
    "[V4+ Styles]\r\n"
    "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, "
    "OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, "
    "ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
    "Alignment, MarginL, MarginR, MarginV, Encoding\r\n";

static const char ass_events[] =
    "\r\n"
    "[Events]\r\n"
    "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, "
    "Effect, Text\r\n";

struct fixture_spec {
    const wchar_t *name;
    const char *prefix;
    const char *style;
    const char *events;
    const char *plain;
};

static const struct fixture_spec fixture_specs[FIXTURE_COUNT] = {
    [FIXTURE_PRIMARY_SRT] =
        {
            .name = L"primary.srt",
            .plain = primary_srt,
        },
    [FIXTURE_SECONDARY_SRT] =
        {
            .name = L"secondary.srt",
            .plain = secondary_srt,
        },
    [FIXTURE_PRIMARY_INLINE_TOP_SRT] =
        {
            .name = L"primary-inline-top.srt",
            .plain = primary_inline_top_srt,
        },
    [FIXTURE_PRIMARY_GLOBAL_TOP_SRT] =
        {
            .name = L"primary-global-top.srt",
            .plain = primary_global_top_srt,
        },
    [FIXTURE_ASS_BOTTOM] =
        {
            .name = L"bottom.ass",
            .prefix = ass_header,
            .style = "Style: Probe,Arial,40,&H000000FF,&H000000FF,&H00FFFF00,"
                     "&H00800000,-1,0,0,0,100,100,0,0,1,2,1,2,20,20,28,1\r\n",
            .events = "Dialogue: 0,0:00:00.00,0:00:08.00,Probe,,0,0,0,,"
                      "Bottom styled sample\r\n",
        },
    [FIXTURE_ASS_TOP] =
        {
            .name = L"top.ass",
            .prefix = ass_header,
            .style = "Style: Probe,Arial,40,&H000000FF,&H000000FF,&H00FFFF00,"
                     "&H00800000,-1,0,0,0,100,100,0,0,1,2,1,8,20,20,28,1\r\n",
            .events = "Dialogue: 0,0:00:00.00,0:00:08.00,Probe,,0,0,0,,"
                      "Top styled sample\r\n",
        },
    [FIXTURE_ASS_POSITION] =
        {
            .name = L"position.ass",
            .prefix = ass_header,
            .style = "Style: Probe,Arial,40,&H000000FF,&H000000FF,&H00FFFF00,"
                     "&H00800000,-1,0,0,0,100,100,0,0,1,2,1,2,20,20,28,1\r\n",
            .events = "Dialogue: 0,0:00:00.00,0:00:08.00,Probe,,0,0,0,,"
                      "{\\pos(220,118)\\fs44\\frz-6}Position styled sample\r\n"
                      "Dialogue: 0,0:00:00.00,0:00:08.00,Probe,,0,0,0,,"
                      "{\\an7\\pos(172,148)\\p1\\c&H00FF8000&\\bord0\\shad0}"
                      "m 0 0 l 28 0 28 10 0 10{\\p0}\r\n",
        },
    [FIXTURE_ASS_MOVE] =
        {
            .name = L"move.ass",
            .prefix = ass_header,
            .style = "Style: Probe,Arial,40,&H000000FF,&H000000FF,&H00FFFF00,"
                     "&H00800000,-1,0,0,0,100,100,0,0,1,2,1,2,20,20,28,1\r\n",
            .events = "Dialogue: 0,0:00:00.00,0:00:08.00,Probe,,0,0,0,,"
                      "{\\move(170,205,330,205,0,4000)\\fs42\\frz5}"
                      "Moving styled sample\r\n",
        },
};

struct fixture_file {
    wchar_t path[PATH_CAP];
    char utf8_path[UTF8_PATH_CAP];
    bool created;
};

struct mpv_api {
    HMODULE module;
    unsigned long (*client_api_version)(void);
    const char *(*error_string)(int);
    void (*free_value)(void *);
    void (*free_node_contents)(mpv_node *);
    mpv_handle *(*create)(void);
    int (*initialize)(mpv_handle *);
    void (*terminate_destroy)(mpv_handle *);
    int (*set_option_string)(mpv_handle *, const char *, const char *);
    int (*set_property_string)(mpv_handle *, const char *, const char *);
    int (*get_property)(mpv_handle *, const char *, mpv_format, void *);
    char *(*get_property_string)(mpv_handle *, const char *);
    int (*command)(mpv_handle *, const char **);
    int (*command_ret)(mpv_handle *, const char **, mpv_node *);
    mpv_event *(*wait_event)(mpv_handle *, double);
};

struct metric {
    bool present;
    int x0;
    int y0;
    int x1;
    int y1;
    uint64_t pixels;
    uint64_t mask_hash;
    uint64_t rgba_hash;
};

struct snapshot {
    int width;
    int height;
    int64_t time_us;
    uint64_t frame_hash;
    uint8_t *frame_data;
    size_t frame_size;
    struct metric primary;
    struct metric secondary;
    struct metric ink;
};

enum anchor {
    ANCHOR_ANY,
    ANCHOR_TOP,
    ANCHOR_BOTTOM,
};

enum expectation {
    EXPECT_PRIMARY_UP,
    EXPECT_SECONDARY_UP,
    EXPECT_SECONDARY_ANY,
    EXPECT_GROUP_UP,
    EXPECT_GROUP_ANY,
    EXPECT_SPLIT_INWARD,
    EXPECT_PRIMARY_UP_SECONDARY_STILL,
    EXPECT_PRIMARY_STILL_SECONDARY_ANY,
    EXPECT_PRIMARY_STATIC,
    EXPECT_PRIMARY_OBSERVE,
};

struct position_state {
    int primary;
    int secondary;
    int margin;
};

struct movement_case {
    const char *id;
    const char *proposition;
    const char *ambiguity;
    enum fixture_id primary_fixture;
    const char *primary_text;
    enum fixture_id secondary_fixture;
    const char *secondary_text;
    const char *layout;
    const char *order;
    const char *align_y;
    struct position_state before;
    struct position_state after;
    enum expectation expectation;
    enum anchor primary_anchor;
    enum anchor secondary_anchor;
};

struct evaluation {
    const char *status;
    const char *classification;
    bool primary_shape_stable;
    bool secondary_shape_stable;
    bool ink_shape_stable;
};

struct suite {
    struct mpv_api api;
    mpv_handle *ctx;
    wchar_t executable_dir[PATH_CAP];
    wchar_t fixture_dir[PATH_CAP];
    wchar_t dll_path[PATH_CAP];
    struct fixture_file fixtures[FIXTURE_COUNT];
    uint64_t deadline_ms;
    unsigned long api_version;
    bool relative_mode;
    int retained_frames;
    int64_t current_playlist_entry_id;
    int passed;
    int failed;
    int ambiguous;
    char error_code[96];
    char error_message[512];
};

static bool set_error(struct suite *suite, const char *code, const char *format,
                      ...)
{
    if (suite->error_code[0])
        return false;

    snprintf(suite->error_code, sizeof(suite->error_code), "%s", code);
    va_list args;
    va_start(args, format);
    vsnprintf(suite->error_message, sizeof(suite->error_message), format, args);
    va_end(args);
    return false;
}

static void print_json_string(const char *value)
{
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        switch (*p) {
        case '"':
            fputs("\\\"", stdout);
            break;
        case '\\':
            fputs("\\\\", stdout);
            break;
        case '\b':
            fputs("\\b", stdout);
            break;
        case '\f':
            fputs("\\f", stdout);
            break;
        case '\n':
            fputs("\\n", stdout);
            break;
        case '\r':
            fputs("\\r", stdout);
            break;
        case '\t':
            fputs("\\t", stdout);
            break;
        default:
            if (*p < 0x20)
                printf("\\u%04x", *p);
            else
                putchar(*p);
            break;
        }
    }
    putchar('"');
}

static bool join_wide_path(wchar_t *destination, size_t capacity,
                           const wchar_t *directory, const wchar_t *name)
{
    int written = swprintf(destination, capacity, L"%ls\\%ls", directory, name);
    return written > 0 && (size_t)written < capacity;
}

static bool wide_to_utf8(const wchar_t *source, char *destination,
                         size_t capacity)
{
    if (capacity > INT_MAX)
        return false;
    int written = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, source, -1,
                                      destination, (int)capacity, NULL, NULL);
    return written > 0;
}

static bool initialize_paths(struct suite *suite)
{
    DWORD length = GetModuleFileNameW(NULL, suite->executable_dir, PATH_CAP);
    if (!length || length >= PATH_CAP)
        return set_error(suite, "executable-path",
                         "GetModuleFileNameW failed with %" PRIu32 ".",
                         (uint32_t)GetLastError());

    wchar_t *separator = wcsrchr(suite->executable_dir, L'\\');
    if (!separator)
        return set_error(suite, "executable-path",
                         "The executable path has no directory separator.");
    *separator = L'\0';

    if (!join_wide_path(suite->dll_path, ARRAY_SIZE(suite->dll_path),
                        suite->executable_dir, L"libmpv-2.dll")) {
        return set_error(suite, "dll-path", "The libmpv path is too long.");
    }

    for (unsigned int attempt = 0; attempt < 16; attempt++) {
        wchar_t name[128];
        int written = swprintf(name, ARRAY_SIZE(name),
                               L"subtitle-positioning-fixtures-%lu-%llu-%u",
                               GetCurrentProcessId(),
                               (unsigned long long)GetTickCount64(), attempt);
        if (written <= 0 || (size_t)written >= ARRAY_SIZE(name))
            return set_error(suite, "fixture-path",
                             "The fixture directory name is too long.");
        if (!join_wide_path(suite->fixture_dir, ARRAY_SIZE(suite->fixture_dir),
                            suite->executable_dir, name)) {
            return set_error(suite, "fixture-path",
                             "The fixture directory path is too long.");
        }
        if (CreateDirectoryW(suite->fixture_dir, NULL))
            return true;
        if (GetLastError() != ERROR_ALREADY_EXISTS) {
            return set_error(suite, "fixture-directory",
                             "CreateDirectoryW failed with %" PRIu32 ".",
                             (uint32_t)GetLastError());
        }
    }

    return set_error(suite, "fixture-directory",
                     "Could not reserve a unique fixture directory.");
}

static bool write_bytes(FILE *file, const char *data)
{
    size_t size = strlen(data);
    return fwrite(data, 1, size, file) == size;
}

static bool create_fixtures(struct suite *suite)
{
    for (int index = 0; index < FIXTURE_COUNT; index++) {
        const struct fixture_spec *spec = &fixture_specs[index];
        struct fixture_file *fixture = &suite->fixtures[index];

        if (!join_wide_path(fixture->path, ARRAY_SIZE(fixture->path),
                            suite->fixture_dir, spec->name) ||
            !wide_to_utf8(fixture->path, fixture->utf8_path,
                          sizeof(fixture->utf8_path))) {
            return set_error(suite, "fixture-path",
                             "A fixture path could not be encoded.");
        }

        FILE *file = NULL;
        if (_wfopen_s(&file, fixture->path, L"wb") != 0 || !file) {
            return set_error(suite, "fixture-create",
                             "Could not create fixture %d.", index);
        }
        fixture->created = true;

        bool written;
        if (spec->plain) {
            written = write_bytes(file, spec->plain);
        } else {
            written = write_bytes(file, spec->prefix) &&
                      write_bytes(file, spec->style) &&
                      write_bytes(file, ass_events) &&
                      write_bytes(file, spec->events);
        }
        bool closed = fclose(file) == 0;
        if (!written || !closed) {
            return set_error(suite, "fixture-write",
                             "Could not finish fixture %d.", index);
        }
    }
    return true;
}

static bool cleanup_fixtures(struct suite *suite)
{
    bool success = true;
    for (int index = 0; index < FIXTURE_COUNT; index++) {
        struct fixture_file *fixture = &suite->fixtures[index];
        if (!fixture->created)
            continue;
        if (!DeleteFileW(fixture->path) &&
            GetLastError() != ERROR_FILE_NOT_FOUND) {
            success = false;
        }
        fixture->created = false;
    }
    if (suite->fixture_dir[0] && !RemoveDirectoryW(suite->fixture_dir) &&
        GetLastError() != ERROR_PATH_NOT_FOUND) {
        success = false;
    }
    return success;
}

static int count_owned_resources(const struct suite *suite)
{
    int leaked = suite->retained_frames;
    if (suite->ctx)
        leaked++;
    if (suite->api.module)
        leaked++;
    for (int index = 0; index < FIXTURE_COUNT; index++) {
        if (suite->fixtures[index].path[0] &&
            GetFileAttributesW(suite->fixtures[index].path) !=
                INVALID_FILE_ATTRIBUTES) {
            leaked++;
        }
    }
    if (suite->fixture_dir[0] &&
        GetFileAttributesW(suite->fixture_dir) != INVALID_FILE_ATTRIBUTES) {
        leaked++;
    }
    return leaked;
}

static bool load_symbol(struct suite *suite, void *destination,
                        size_t destination_size, const char *name)
{
    FARPROC symbol = GetProcAddress(suite->api.module, name);
    if (!symbol)
        return set_error(suite, "libmpv-symbol", "Missing public symbol %s.",
                         name);
    if (sizeof(symbol) != destination_size) {
        return set_error(suite, "libmpv-symbol-size",
                         "Unexpected function pointer size for %s.", name);
    }
    memcpy(destination, &symbol, destination_size);
    return true;
}

#define LOAD_MPV_SYMBOL(field, name)                                           \
    load_symbol(suite, &suite->api.field, sizeof(suite->api.field), "mpv_" name)

static bool load_mpv_api(struct suite *suite)
{
    suite->api.module = LoadLibraryExW(suite->dll_path, NULL,
                                       LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                           LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!suite->api.module) {
        return set_error(suite, "libmpv-load",
                         "Adjacent libmpv-2.dll or its closure could not load "
                         "(win32=%" PRIu32 ").",
                         (uint32_t)GetLastError());
    }

    if (!LOAD_MPV_SYMBOL(client_api_version, "client_api_version") ||
        !LOAD_MPV_SYMBOL(error_string, "error_string") ||
        !LOAD_MPV_SYMBOL(free_value, "free") ||
        !LOAD_MPV_SYMBOL(free_node_contents, "free_node_contents") ||
        !LOAD_MPV_SYMBOL(create, "create") ||
        !LOAD_MPV_SYMBOL(initialize, "initialize") ||
        !LOAD_MPV_SYMBOL(terminate_destroy, "terminate_destroy") ||
        !LOAD_MPV_SYMBOL(set_option_string, "set_option_string") ||
        !LOAD_MPV_SYMBOL(set_property_string, "set_property_string") ||
        !LOAD_MPV_SYMBOL(get_property, "get_property") ||
        !LOAD_MPV_SYMBOL(get_property_string, "get_property_string") ||
        !LOAD_MPV_SYMBOL(command, "command") ||
        !LOAD_MPV_SYMBOL(command_ret, "command_ret") ||
        !LOAD_MPV_SYMBOL(wait_event, "wait_event")) {
        return false;
    }

    suite->api_version = suite->api.client_api_version();
    if ((suite->api_version >> 16) != (MPV_CLIENT_API_VERSION >> 16)) {
        return set_error(
            suite, "libmpv-api",
            "libmpv API major %lu does not match header major %lu.",
            suite->api_version >> 16, MPV_CLIENT_API_VERSION >> 16);
    }
    return true;
}

static void unload_mpv_api(struct suite *suite)
{
    if (suite->api.module)
        FreeLibrary(suite->api.module);
    memset(&suite->api, 0, sizeof(suite->api));
}

static bool mpv_failure(struct suite *suite, const char *operation, int status)
{
    return set_error(suite, "libmpv-api-call", "%s failed: %s (%d).", operation,
                     suite->api.error_string(status), status);
}

static bool set_option(struct suite *suite, const char *name, const char *value)
{
    int status = suite->api.set_option_string(suite->ctx, name, value);
    return status >= 0 || mpv_failure(suite, name, status);
}

static bool set_property(struct suite *suite, const char *name,
                         const char *value)
{
    int status = suite->api.set_property_string(suite->ctx, name, value);
    return status >= 0 || mpv_failure(suite, name, status);
}

static bool run_command(struct suite *suite, const char **arguments)
{
    int status = suite->api.command(suite->ctx, arguments);
    return status >= 0 || mpv_failure(suite, arguments[0], status);
}

static bool start_player(struct suite *suite)
{
    if (suite->ctx)
        return set_error(suite, "client-ownership",
                         "A previous case still owns an mpv client.");
    suite->current_playlist_entry_id = -1;
    suite->ctx = suite->api.create();
    if (!suite->ctx)
        return set_error(suite, "libmpv-create", "mpv_create returned NULL.");

    const struct {
        const char *name;
        const char *value;
    } options[] = {
        {"config", "no"},
        {"terminal", "no"},
        {"msg-level", "all=no"},
        {"load-scripts", "no"},
        {"osc", "no"},
        {"input-conf", ""},
        {"input-default-bindings", "no"},
        {"input-terminal", "no"},
        {"autoload-files", "no"},
        {"sub-auto", "no"},
        {"audio-file-auto", "no"},
        {"cover-art-auto", "no"},
        {"vo", "null"},
        {"ao", "null"},
        {"hwdec", "no"},
        {"screenshot-sw", "yes"},
        {"sws-allow-zimg", "no"},
        {"force-rgba-osd-rendering", suite->relative_mode ? "no" : "yes"},
        {"pause", "yes"},
        {"keep-open", "yes"},
        {"idle", "yes"},
        {"osd-level", "0"},
        {"sub-font", "Arial"},
        {"sub-font-size", "36"},
        {"sub-bold", "yes"},
        {"sub-outline-size", "0"},
        {"sub-shadow-offset", "0"},
        {"sub-blur", "0"},
        {"sub-margin-x", "20"},
        {"sub-margin-y", "28"},
        {"sub-color", "#FF2020"},
        {"sub-outline-color", "#000000"},
        {"sub-back-color", "#00000000"},
        {"sub-ass-override", "scale"},
        {"secondary-sub-ass-override", "strip"},
        {"sub-scale", "0.75"},
        {"secondary-sub-scale", "1.0"},
        {"sub-stack-gap", "12"},
        {"sub-stack-margin", "100"},
        {"sub-avoid-bottom-px", "0"},
    };

    for (size_t index = 0; index < ARRAY_SIZE(options); index++) {
        if (!set_option(suite, options[index].name, options[index].value))
            return false;
    }

    int status = suite->api.initialize(suite->ctx);
    return status >= 0 || mpv_failure(suite, "mpv_initialize", status);
}

static void stop_player(struct suite *suite)
{
    if (suite->ctx)
        suite->api.terminate_destroy(suite->ctx);
    suite->ctx = NULL;
    suite->current_playlist_entry_id = -1;
}

static bool deadline_expired(struct suite *suite, uint64_t operation_deadline)
{
    uint64_t now = GetTickCount64();
    return now >= operation_deadline || now >= suite->deadline_ms;
}

static double wait_slice(struct suite *suite, uint64_t operation_deadline)
{
    uint64_t deadline = operation_deadline < suite->deadline_ms
                            ? operation_deadline
                            : suite->deadline_ms;
    uint64_t now = GetTickCount64();
    if (now >= deadline)
        return 0;
    uint64_t remaining = deadline - now;
    return remaining < 50 ? remaining / 1000.0 : 0.05;
}

static bool drain_events(struct suite *suite)
{
    for (int count = 0; count < 10000; count++) {
        mpv_event *event = suite->api.wait_event(suite->ctx, 0);
        if (event->event_id == MPV_EVENT_NONE)
            return true;
        if (event->event_id == MPV_EVENT_SHUTDOWN)
            return set_error(suite, "unexpected-shutdown",
                             "libmpv shut down while draining events.");
        if (event->event_id == MPV_EVENT_QUEUE_OVERFLOW)
            return set_error(suite, "event-overflow",
                             "The libmpv event queue overflowed.");
    }
    return set_error(suite, "event-drain",
                     "The libmpv event queue did not become empty.");
}

static bool ignore_stale_or_report_owned_end(struct suite *suite,
                                             const mpv_event *event,
                                             int64_t owned_entry_id,
                                             const char *phase)
{
    if (!event->data)
        return set_error(suite, "end-file-shape",
                         "END_FILE had no generation data during %s.", phase);
    const mpv_event_end_file *end = event->data;
    if (owned_entry_id < 0 || end->playlist_entry_id != owned_entry_id)
        return true;
    return set_error(suite, "owned-media-ended",
                     "Owned media %" PRId64
                     " ended during %s (reason=%d,error=%d).",
                     owned_entry_id, phase, end->reason, end->error);
}

static bool wait_for_load(struct suite *suite)
{
    int64_t loading_entry_id = -1;
    bool loaded = false;
    bool restarted = false;
    uint64_t deadline = GetTickCount64() + OPERATION_TIMEOUT_MS;

    while (loading_entry_id < 0 || !loaded || !restarted) {
        if (deadline_expired(suite, deadline))
            return set_error(suite, "load-timeout",
                             "Timed out waiting for file-loaded and "
                             "playback-restart.");
        mpv_event *event =
            suite->api.wait_event(suite->ctx, wait_slice(suite, deadline));
        if (event->event_id == MPV_EVENT_START_FILE) {
            if (!event->data)
                return set_error(suite, "start-file-shape",
                                 "START_FILE had no generation data.");
            const mpv_event_start_file *start = event->data;
            if (loading_entry_id >= 0 &&
                start->playlist_entry_id != loading_entry_id) {
                return set_error(
                    suite, "unexpected-start-file",
                    "A second media generation started before the owned "
                    "load completed.");
            }
            loading_entry_id = start->playlist_entry_id;
            loaded = false;
            restarted = false;
        } else if (event->event_id == MPV_EVENT_FILE_LOADED &&
                   loading_entry_id >= 0) {
            loaded = true;
        } else if (event->event_id == MPV_EVENT_PLAYBACK_RESTART &&
                   loading_entry_id >= 0) {
            restarted = true;
        } else if (event->event_id == MPV_EVENT_END_FILE) {
            if (!ignore_stale_or_report_owned_end(suite, event,
                                                  loading_entry_id, "load"))
                return false;
        } else if (event->event_id == MPV_EVENT_SHUTDOWN) {
            return set_error(suite, "unexpected-shutdown",
                             "libmpv shut down during load.");
        } else if (event->event_id == MPV_EVENT_QUEUE_OVERFLOW) {
            return set_error(suite, "event-overflow",
                             "The libmpv event queue overflowed.");
        }
    }
    suite->current_playlist_entry_id = loading_entry_id;
    return true;
}

static bool wait_for_seek_restart(struct suite *suite)
{
    bool saw_seek = false;
    uint64_t deadline = GetTickCount64() + OPERATION_TIMEOUT_MS;
    while (true) {
        if (deadline_expired(suite, deadline))
            return set_error(suite, "seek-timeout",
                             "Timed out waiting for playback-restart.");
        mpv_event *event =
            suite->api.wait_event(suite->ctx, wait_slice(suite, deadline));
        if (event->event_id == MPV_EVENT_SEEK)
            saw_seek = true;
        if (event->event_id == MPV_EVENT_PLAYBACK_RESTART && saw_seek)
            return true;
        if (event->event_id == MPV_EVENT_END_FILE &&
            !ignore_stale_or_report_owned_end(
                suite, event, suite->current_playlist_entry_id, "exact seek"))
            return false;
        if (event->event_id == MPV_EVENT_SHUTDOWN)
            return set_error(suite, "unexpected-shutdown",
                             "libmpv shut down during seek.");
        if (event->event_id == MPV_EVENT_QUEUE_OVERFLOW)
            return set_error(suite, "event-overflow",
                             "The libmpv event queue overflowed.");
    }
}

static bool get_time_us(struct suite *suite, int64_t *time_us)
{
    double time_position = 0;
    int status = suite->api.get_property(suite->ctx, "time-pos",
                                         MPV_FORMAT_DOUBLE, &time_position);
    if (status < 0)
        return mpv_failure(suite, "time-pos", status);
    *time_us = (int64_t)(time_position * 1000000.0 +
                         (time_position >= 0 ? 0.5 : -0.5));
    return true;
}

static bool contains_property_text(struct suite *suite, const char *property,
                                   const char *expected)
{
    char *value = suite->api.get_property_string(suite->ctx, property);
    bool found = value && strstr(value, expected) != NULL;
    suite->api.free_value(value);
    return found;
}

static bool wait_for_subtitles(struct suite *suite, const char *primary,
                               const char *secondary)
{
    uint64_t deadline = GetTickCount64() + OPERATION_TIMEOUT_MS;
    while (true) {
        bool primary_ready =
            !primary || contains_property_text(suite, "sub-text", primary);
        bool secondary_ready =
            !secondary ||
            contains_property_text(suite, "secondary-sub-text", secondary);
        if (primary_ready && secondary_ready)
            return true;
        if (deadline_expired(suite, deadline)) {
            return set_error(suite, "subtitle-timeout",
                             "Timed out waiting for decoded subtitle text.");
        }
        mpv_event *event =
            suite->api.wait_event(suite->ctx, wait_slice(suite, deadline));
        if (event->event_id == MPV_EVENT_END_FILE &&
            !ignore_stale_or_report_owned_end(suite, event,
                                              suite->current_playlist_entry_id,
                                              "subtitle readiness"))
            return false;
        if (event->event_id == MPV_EVENT_SHUTDOWN)
            return set_error(suite, "unexpected-shutdown",
                             "libmpv shut down while waiting for subtitles.");
        if (event->event_id == MPV_EVENT_QUEUE_OVERFLOW)
            return set_error(suite, "event-overflow",
                             "The libmpv event queue overflowed.");
    }
}

static bool selected_subtitle_id(struct suite *suite, int64_t *id)
{
    char *value = suite->api.get_property_string(suite->ctx, "sid");
    if (!value)
        return set_error(suite, "track-id", "Could not read selected sid.");
    char *end = NULL;
    int64_t parsed = strtoll(value, &end, 10);
    bool valid = end && *end == '\0' && parsed > 0;
    suite->api.free_value(value);
    if (!valid)
        return set_error(suite, "track-id",
                         "The selected subtitle track has no numeric id.");
    *id = parsed;
    return true;
}

static bool set_track_id(struct suite *suite, const char *property, int64_t id)
{
    char value[32];
    snprintf(value, sizeof(value), "%" PRId64, id);
    return set_property(suite, property, value);
}

static bool add_subtitle(struct suite *suite, enum fixture_id fixture,
                         int64_t *id)
{
    const char *arguments[] = {
        "sub-add",
        suite->fixtures[fixture].utf8_path,
        "select",
        NULL,
    };
    return run_command(suite, arguments) && selected_subtitle_id(suite, id);
}

static bool set_position_state(struct suite *suite, struct position_state state)
{
    char primary[16];
    char secondary[16];
    char margin[16];
    snprintf(primary, sizeof(primary), "%d", state.primary);
    snprintf(secondary, sizeof(secondary), "%d", state.secondary);
    snprintf(margin, sizeof(margin), "%d", state.margin);
    return set_property(suite, "sub-pos", primary) &&
           set_property(suite, "secondary-sub-pos", secondary) &&
           set_property(suite, "sub-stack-margin", margin);
}

static bool prepare_case(struct suite *suite, const struct movement_case *test)
{
    bool has_secondary = test->secondary_fixture != FIXTURE_NONE;
    const char *plain_color =
        has_secondary || test->primary_fixture != FIXTURE_PRIMARY_SRT
            ? "#20FF20"
            : "#FF2020";

    if ((suite->relative_mode &&
         (!set_property(suite, "sub-pos-mode", "relative") ||
          !set_property(suite, "secondary-sub-pos-mode", "relative"))) ||
        !set_property(suite, "pause", "yes") ||
        !set_property(suite, "sid", "no") ||
        !set_property(suite, "secondary-sid", "no") ||
        !set_property(suite, "sub-visibility", "yes") ||
        !set_property(suite, "secondary-sub-visibility", "yes") ||
        !set_property(suite, "sub-color", plain_color) ||
        !set_property(suite, "sub-ass-override", "scale") ||
        !set_property(suite, "secondary-sub-ass-override", "strip") ||
        !set_property(suite, "sub-scale", "0.75") ||
        !set_property(suite, "secondary-sub-scale", "1.0") ||
        !set_property(suite, "sub-align-y",
                      test->align_y ? test->align_y : "bottom") ||
        !set_property(suite, "sub-stack-layout", test->layout) ||
        !set_property(suite, "sub-stack-order", test->order) ||
        !set_position_state(suite, test->before) || !drain_events(suite)) {
        return false;
    }

    const char *load[] = {"loadfile", video_url, NULL};
    if (!run_command(suite, load) || !wait_for_load(suite))
        return false;

    int64_t primary_id = 0;
    int64_t secondary_id = 0;
    if (test->primary_fixture != FIXTURE_NONE &&
        !add_subtitle(suite, test->primary_fixture, &primary_id)) {
        return false;
    }
    if (test->secondary_fixture != FIXTURE_NONE &&
        !add_subtitle(suite, test->secondary_fixture, &secondary_id)) {
        return false;
    }

    if (test->primary_fixture != FIXTURE_NONE) {
        if (!set_track_id(suite, "sid", primary_id))
            return false;
    } else if (!set_property(suite, "sid", "no")) {
        return false;
    }
    if (test->secondary_fixture != FIXTURE_NONE) {
        if (!set_track_id(suite, "secondary-sid", secondary_id))
            return false;
    } else if (!set_property(suite, "secondary-sid", "no")) {
        return false;
    }

    if (!drain_events(suite))
        return false;
    const char *seek[] = {"seek", "2", "absolute+exact", NULL};
    if (!run_command(suite, seek) || !wait_for_seek_restart(suite) ||
        !wait_for_subtitles(suite, test->primary_text, test->secondary_text)) {
        return false;
    }

    int64_t time_us = 0;
    if (!get_time_us(suite, &time_us))
        return false;
    if (llabs(time_us - PROBE_TIME_US) > 50000) {
        return set_error(suite, "seek-position",
                         "Exact seek settled at %" PRId64 " us.", time_us);
    }
    return true;
}

static const mpv_node *map_value(const mpv_node *map, const char *key)
{
    if (!map || map->format != MPV_FORMAT_NODE_MAP || !map->u.list ||
        !map->u.list->keys || !map->u.list->values)
        return NULL;
    for (int index = 0; index < map->u.list->num; index++) {
        if (strcmp(map->u.list->keys[index], key) == 0)
            return &map->u.list->values[index];
    }
    return NULL;
}

enum mask_kind {
    MASK_PRIMARY,
    MASK_SECONDARY,
    MASK_INK,
};

static bool pixel_matches(enum mask_kind kind, const uint8_t *pixel)
{
    unsigned int red = pixel[0];
    unsigned int green = pixel[1];
    unsigned int blue = pixel[2];
    if (kind == MASK_PRIMARY)
        return red >= 48 && red >= green + 24 && red >= blue + 24;
    if (kind == MASK_SECONDARY)
        return green >= 48 && green >= red + 24 && green >= blue + 24;
    unsigned int maximum = red > green ? red : green;
    maximum = maximum > blue ? maximum : blue;
    return maximum >= 24;
}

static uint64_t hash_byte(uint64_t hash, uint8_t value)
{
    return (hash ^ value) * FNV_PRIME;
}

static uint64_t hash_u32(uint64_t hash, uint32_t value)
{
    for (int shift = 0; shift < 32; shift += 8)
        hash = hash_byte(hash, (uint8_t)(value >> shift));
    return hash;
}

static uint64_t hash_frame(const uint8_t *data, int width, int height,
                           ptrdiff_t stride)
{
    uint64_t hash = hash_u32(FNV_OFFSET, (uint32_t)width);
    hash = hash_u32(hash, (uint32_t)height);
    for (int y = 0; y < height; y++) {
        const uint8_t *row = data + (ptrdiff_t)y * stride;
        for (int x = 0; x < width * 4; x++)
            hash = hash_byte(hash, row[x]);
    }
    return hash;
}

static void free_snapshot(struct suite *suite, struct snapshot *snapshot)
{
    if (snapshot->frame_data) {
        free(snapshot->frame_data);
        suite->retained_frames--;
    }
    snapshot->frame_data = NULL;
    snapshot->frame_size = 0;
}

static bool frame_bytes_equal(const struct snapshot *left,
                              const struct snapshot *right)
{
    return left->width == right->width && left->height == right->height &&
           left->frame_size == right->frame_size &&
           left->frame_hash == right->frame_hash && left->frame_data &&
           right->frame_data &&
           memcmp(left->frame_data, right->frame_data, left->frame_size) == 0;
}

static void scan_metric_rows(struct metric *metric, enum mask_kind kind,
                             const uint8_t *data, int width, int height,
                             ptrdiff_t stride, int first_y, int last_y)
{
    metric->x0 = width;
    metric->y0 = height;
    metric->x1 = 0;
    metric->y1 = 0;

    for (int y = first_y; y < last_y; y++) {
        const uint8_t *row = data + (ptrdiff_t)y * stride;
        for (int x = 0; x < width; x++) {
            const uint8_t *pixel = row + x * 4;
            if (!pixel_matches(kind, pixel))
                continue;
            metric->present = true;
            metric->pixels++;
            if (x < metric->x0)
                metric->x0 = x;
            if (y < metric->y0)
                metric->y0 = y;
            if (x + 1 > metric->x1)
                metric->x1 = x + 1;
            if (y + 1 > metric->y1)
                metric->y1 = y + 1;
        }
    }

    if (!metric->present)
        return;

    uint64_t mask_hash = FNV_OFFSET;
    uint64_t rgba_hash = FNV_OFFSET;
    int box_width = metric->x1 - metric->x0;
    int box_height = metric->y1 - metric->y0;
    mask_hash = hash_u32(mask_hash, (uint32_t)box_width);
    mask_hash = hash_u32(mask_hash, (uint32_t)box_height);
    rgba_hash = hash_u32(rgba_hash, (uint32_t)box_width);
    rgba_hash = hash_u32(rgba_hash, (uint32_t)box_height);

    for (int y = metric->y0; y < metric->y1; y++) {
        const uint8_t *row = data + (ptrdiff_t)y * stride;
        for (int x = metric->x0; x < metric->x1; x++) {
            const uint8_t *pixel = row + x * 4;
            bool matches = pixel_matches(kind, pixel);
            mask_hash = hash_byte(mask_hash, matches ? 1 : 0);
            if (!matches)
                continue;
            rgba_hash = hash_u32(rgba_hash, (uint32_t)(x - metric->x0));
            rgba_hash = hash_u32(rgba_hash, (uint32_t)(y - metric->y0));
            for (int channel = 0; channel < 4; channel++)
                rgba_hash = hash_byte(rgba_hash, pixel[channel]);
        }
    }

    metric->mask_hash = mask_hash;
    metric->rgba_hash = rgba_hash;
}

static void scan_metric(struct metric *metric, enum mask_kind kind,
                        const uint8_t *data, int width, int height,
                        ptrdiff_t stride)
{
    scan_metric_rows(metric, kind, data, width, height, stride, 0, height);
}

static int scan_ink_vertical_groups(const struct snapshot *snapshot,
                                    struct metric groups[2])
{
    int starts[2] = {0};
    int ends[2] = {0};
    int count = 0;
    int last_active = -100;

    for (int y = 0; y < snapshot->height; y++) {
        const uint8_t *row =
            snapshot->frame_data + (size_t)y * (size_t)snapshot->width * 4;
        bool active = false;
        for (int x = 0; x < snapshot->width; x++) {
            if (pixel_matches(MASK_INK, row + x * 4)) {
                active = true;
                break;
            }
        }
        if (!active)
            continue;
        if (count == 0 || y - last_active > 3) {
            if (count == 2)
                return 3;
            starts[count] = y;
            count++;
        }
        ends[count - 1] = y + 1;
        last_active = y;
    }

    for (int index = 0; index < count; index++) {
        scan_metric_rows(&groups[index], MASK_INK, snapshot->frame_data,
                         snapshot->width, snapshot->height,
                         (ptrdiff_t)snapshot->width * 4, starts[index],
                         ends[index]);
    }
    return count;
}

static int split_secondary_top_stack(const struct snapshot *snapshot,
                                     struct metric *primary,
                                     struct metric *secondary)
{
    struct metric groups[2] = {0};
    int count = scan_ink_vertical_groups(snapshot, groups);
    if (count == 1) {
        *primary = groups[0];
    } else if (count == 2) {
        *secondary = groups[0];
        *primary = groups[1];
    }
    return count;
}

static bool parse_screenshot(struct suite *suite, const mpv_node *result,
                             struct snapshot *snapshot)
{
    const mpv_node *width_node = map_value(result, "w");
    const mpv_node *height_node = map_value(result, "h");
    const mpv_node *stride_node = map_value(result, "stride");
    const mpv_node *format_node = map_value(result, "format");
    const mpv_node *data_node = map_value(result, "data");
    if (!width_node || width_node->format != MPV_FORMAT_INT64 || !height_node ||
        height_node->format != MPV_FORMAT_INT64 || !stride_node ||
        stride_node->format != MPV_FORMAT_INT64 || !format_node ||
        format_node->format != MPV_FORMAT_STRING || !format_node->u.string ||
        !data_node || data_node->format != MPV_FORMAT_BYTE_ARRAY ||
        !data_node->u.ba || !data_node->u.ba->data) {
        return set_error(suite, "screenshot-shape",
                         "screenshot-raw returned an unexpected node map.");
    }
    if (strcmp(format_node->u.string, "rgba") != 0)
        return set_error(suite, "screenshot-format",
                         "screenshot-raw did not return rgba.");

    int64_t width = width_node->u.int64;
    int64_t height = height_node->u.int64;
    int64_t stride = stride_node->u.int64;
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096 ||
        stride == 0 || stride > PTRDIFF_MAX || stride < PTRDIFF_MIN) {
        return set_error(suite, "screenshot-dimensions",
                         "screenshot-raw returned invalid dimensions.");
    }
    uint64_t absolute_stride =
        stride < 0 ? (uint64_t)(-(stride + 1)) + 1 : (uint64_t)stride;
    if (absolute_stride < (uint64_t)width * 4 ||
        (stride > 0 &&
         (absolute_stride > SIZE_MAX / (uint64_t)height ||
          data_node->u.ba->size < absolute_stride * (uint64_t)height))) {
        return set_error(suite, "screenshot-stride",
                         "screenshot-raw returned an invalid stride.");
    }

    snapshot->width = (int)width;
    snapshot->height = (int)height;
    const uint8_t *pixels = data_node->u.ba->data;
    if ((uint64_t)width > SIZE_MAX / 4 ||
        (uint64_t)height > SIZE_MAX / ((uint64_t)width * 4)) {
        return set_error(
            suite, "screenshot-size",
            "screenshot-raw dimensions overflow addressable memory.");
    }
    snapshot->frame_size = (size_t)width * (size_t)height * 4;
    snapshot->frame_data = malloc(snapshot->frame_size);
    if (!snapshot->frame_data)
        return set_error(suite, "screenshot-memory",
                         "Could not retain screenshot bytes.");
    suite->retained_frames++;
    for (int y = 0; y < snapshot->height; y++) {
        memcpy(snapshot->frame_data + (size_t)y * (size_t)snapshot->width * 4,
               pixels + (ptrdiff_t)y * (ptrdiff_t)stride,
               (size_t)snapshot->width * 4);
    }
    snapshot->frame_hash = hash_frame(pixels, snapshot->width, snapshot->height,
                                      (ptrdiff_t)stride);
    scan_metric(&snapshot->primary, MASK_PRIMARY, pixels, snapshot->width,
                snapshot->height, (ptrdiff_t)stride);
    scan_metric(&snapshot->secondary, MASK_SECONDARY, pixels, snapshot->width,
                snapshot->height, (ptrdiff_t)stride);
    scan_metric(&snapshot->ink, MASK_INK, pixels, snapshot->width,
                snapshot->height, (ptrdiff_t)stride);
    return true;
}

static bool capture_snapshot(struct suite *suite, struct snapshot *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    int64_t before_time = 0;
    if (!get_time_us(suite, &before_time))
        return false;

    uint64_t deadline = GetTickCount64() + SCREENSHOT_TIMEOUT_MS;
    int last_status = MPV_ERROR_COMMAND;
    while (true) {
        mpv_node result = {0};
        const char *arguments[] = {
            "screenshot-raw",
            "subtitles",
            "rgba",
            NULL,
        };
        int status = suite->api.command_ret(suite->ctx, arguments, &result);
        if (status >= 0) {
            bool parsed = parse_screenshot(suite, &result, snapshot);
            suite->api.free_node_contents(&result);
            if (!parsed)
                return false;
            break;
        }
        last_status = status;
        if (deadline_expired(suite, deadline)) {
            return set_error(suite, "screenshot-timeout",
                             "screenshot-raw did not become ready: %s (%d).",
                             suite->api.error_string(last_status), last_status);
        }
        mpv_event *event =
            suite->api.wait_event(suite->ctx, wait_slice(suite, deadline));
        if (event->event_id == MPV_EVENT_END_FILE &&
            !ignore_stale_or_report_owned_end(
                suite, event, suite->current_playlist_entry_id, "screenshot"))
            return false;
        if (event->event_id == MPV_EVENT_SHUTDOWN)
            return set_error(suite, "unexpected-shutdown",
                             "libmpv shut down while waiting for screenshot.");
        if (event->event_id == MPV_EVENT_QUEUE_OVERFLOW)
            return set_error(suite, "event-overflow",
                             "The libmpv event queue overflowed.");
    }

    int64_t after_time = 0;
    if (!get_time_us(suite, &after_time)) {
        free_snapshot(suite, snapshot);
        return false;
    }
    if (llabs(after_time - before_time) > 500) {
        free_snapshot(suite, snapshot);
        return set_error(suite, "capture-time-drift",
                         "Playback advanced during screenshot-raw.");
    }
    snapshot->time_us = before_time;
    return true;
}

static bool metric_same_shape(const struct metric *left,
                              const struct metric *right)
{
    if (left->present != right->present)
        return false;
    if (!left->present)
        return true;
    return left->x1 - left->x0 == right->x1 - right->x0 &&
           left->y1 - left->y0 == right->y1 - right->y0 &&
           left->pixels == right->pixels &&
           left->mask_hash == right->mask_hash &&
           left->rgba_hash == right->rgba_hash;
}

static bool metric_same_absolute(const struct metric *left,
                                 const struct metric *right)
{
    return metric_same_shape(left, right) &&
           (!left->present || (left->x0 == right->x0 && left->y0 == right->y0 &&
                               left->x1 == right->x1 && left->y1 == right->y1));
}

static bool metric_same_bounds(const struct metric *left,
                               const struct metric *right)
{
    return left->present == right->present &&
           (!left->present || (left->x0 == right->x0 && left->y0 == right->y0 &&
                               left->x1 == right->x1 && left->y1 == right->y1));
}

static int delta_top(const struct metric *before, const struct metric *after)
{
    return before->present && after->present ? after->y0 - before->y0 : 0;
}

static int delta_bottom(const struct metric *before, const struct metric *after)
{
    return before->present && after->present ? after->y1 - before->y1 : 0;
}

static bool stationary(const struct metric *before, const struct metric *after)
{
    return before->present && after->present &&
           abs(delta_top(before, after)) <= 1 &&
           abs(delta_bottom(before, after)) <= 1;
}

static bool moved_up(const struct metric *before, const struct metric *after)
{
    return before->present && after->present &&
           delta_top(before, after) <= -MIN_MOVEMENT_PX &&
           delta_bottom(before, after) <= -MIN_MOVEMENT_PX;
}

static bool moved_down(const struct metric *before, const struct metric *after)
{
    return before->present && after->present &&
           delta_top(before, after) >= MIN_MOVEMENT_PX &&
           delta_bottom(before, after) >= MIN_MOVEMENT_PX;
}

static bool moved_any(const struct metric *before, const struct metric *after)
{
    return moved_up(before, after) || moved_down(before, after);
}

static bool same_delta(const struct metric *primary_before,
                       const struct metric *primary_after,
                       const struct metric *secondary_before,
                       const struct metric *secondary_after)
{
    return abs(delta_top(primary_before, primary_after) -
               delta_top(secondary_before, secondary_after)) <= 2 &&
           abs(delta_bottom(primary_before, primary_after) -
               delta_bottom(secondary_before, secondary_after)) <= 2;
}

static bool anchor_matches(const struct metric *metric, enum anchor anchor,
                           int frame_height)
{
    if (anchor == ANCHOR_ANY)
        return true;
    if (!metric->present)
        return false;
    int center_times_two = metric->y0 + metric->y1;
    return anchor == ANCHOR_TOP ? center_times_two < frame_height
                                : center_times_two > frame_height;
}

static struct evaluation evaluate_case(const struct movement_case *test,
                                       const struct snapshot *before,
                                       const struct snapshot *after)
{
    struct evaluation result = {
        .status = "FAIL",
        .classification = "unknown",
        .primary_shape_stable =
            metric_same_shape(&before->primary, &after->primary),
        .secondary_shape_stable =
            metric_same_shape(&before->secondary, &after->secondary),
        .ink_shape_stable = metric_same_shape(&before->ink, &after->ink),
    };

    bool expect_primary = test->primary_fixture != FIXTURE_NONE;
    bool expect_secondary = test->secondary_fixture != FIXTURE_NONE;
    if (before->primary.present != expect_primary ||
        after->primary.present != expect_primary ||
        before->secondary.present != expect_secondary ||
        after->secondary.present != expect_secondary || !before->ink.present ||
        !after->ink.present) {
        result.classification = "missing-or-unexpected-color";
        return result;
    }
    if (before->width != after->width || before->height != after->height ||
        llabs(before->time_us - after->time_us) > 500) {
        result.classification = "time-or-frame-drift";
        return result;
    }
    if ((expect_primary &&
         (!anchor_matches(&before->primary, test->primary_anchor,
                          before->height) ||
          !anchor_matches(&after->primary, test->primary_anchor,
                          after->height))) ||
        (expect_secondary &&
         (!anchor_matches(&before->secondary, test->secondary_anchor,
                          before->height) ||
          !anchor_matches(&after->secondary, test->secondary_anchor,
                          after->height)))) {
        result.classification = "misanchored";
        return result;
    }
    const struct metric *pb = &before->primary;
    const struct metric *pa = &after->primary;
    const struct metric *sb = &before->secondary;
    const struct metric *sa = &after->secondary;

    switch (test->expectation) {
    case EXPECT_PRIMARY_UP:
        if (!moved_up(pb, pa)) {
            result.classification =
                stationary(pb, pa) ? "non-moving" : "wrong-primary-motion";
        } else if (expect_secondary && !stationary(sb, sa)) {
            result.classification = "double-moving";
        } else {
            result.status = "PASS";
            result.classification = "primary-moved-up";
        }
        break;
    case EXPECT_SECONDARY_UP:
        if (!moved_up(sb, sa)) {
            result.classification =
                stationary(sb, sa) ? "non-moving" : "wrong-secondary-motion";
        } else if (expect_primary && !stationary(pb, pa)) {
            result.classification = "double-moving";
        } else {
            result.status = "PASS";
            result.classification = "secondary-moved-up";
        }
        break;
    case EXPECT_SECONDARY_ANY:
        if (!moved_any(sb, sa)) {
            result.classification =
                stationary(sb, sa) ? "non-moving" : "wrong-secondary-motion";
        } else if (expect_primary && !stationary(pb, pa)) {
            result.classification = "double-moving";
        } else {
            result.status = "PASS";
            result.classification = moved_up(sb, sa) ? "secondary-moved-up"
                                                     : "secondary-moved-down";
        }
        break;
    case EXPECT_GROUP_UP:
        if (stationary(pb, pa) && stationary(sb, sa)) {
            result.classification = "non-moving";
        } else if (!moved_up(pb, pa) || !moved_up(sb, sa)) {
            result.classification = "partial-or-wrong-group-motion";
        } else if (!same_delta(pb, pa, sb, sa)) {
            result.classification = "desynchronized-group-motion";
        } else {
            result.status = "PASS";
            result.classification = "group-moved-up";
        }
        break;
    case EXPECT_GROUP_ANY:
        if (stationary(pb, pa) && stationary(sb, sa)) {
            result.classification = "non-moving";
        } else if (!((moved_up(pb, pa) && moved_up(sb, sa)) ||
                     (moved_down(pb, pa) && moved_down(sb, sa)))) {
            result.classification = "partial-or-opposed-group-motion";
        } else if (!same_delta(pb, pa, sb, sa)) {
            result.classification = "desynchronized-group-motion";
        } else {
            result.status = "PASS";
            result.classification =
                moved_up(pb, pa) ? "group-moved-up" : "group-moved-down";
        }
        break;
    case EXPECT_SPLIT_INWARD:
        if (!moved_up(pb, pa) || !moved_down(sb, sa)) {
            result.classification = stationary(pb, pa) && stationary(sb, sa)
                                        ? "non-moving"
                                        : "wrong-split-motion";
        } else {
            result.status = "PASS";
            result.classification = "split-moved-inward";
        }
        break;
    case EXPECT_PRIMARY_UP_SECONDARY_STILL:
        if (!moved_up(pb, pa)) {
            result.classification =
                stationary(pb, pa) ? "non-moving" : "wrong-primary-motion";
        } else if (!stationary(sb, sa)) {
            result.classification = "double-moving";
        } else {
            result.status = "PASS";
            result.classification = "primary-only-moved-up";
        }
        break;
    case EXPECT_PRIMARY_STILL_SECONDARY_ANY:
        if (!moved_any(sb, sa)) {
            result.classification =
                stationary(sb, sa) ? "non-moving" : "wrong-secondary-motion";
        } else if (!stationary(pb, pa)) {
            result.classification = "double-moving";
        } else {
            result.status = "PASS";
            result.classification = moved_up(sb, sa)
                                        ? "secondary-only-moved-up"
                                        : "secondary-only-moved-down";
        }
        break;
    case EXPECT_PRIMARY_STATIC:
        if (!stationary(pb, pa)) {
            result.classification = "author-position-overridden";
        } else {
            result.status = "PASS";
            result.classification = "author-position-preserved";
        }
        break;
    case EXPECT_PRIMARY_OBSERVE:
        result.status = "AMBIGUOUS";
        if (stationary(pb, pa))
            result.classification = "non-moving";
        else if (moved_up(pb, pa))
            result.classification = "primary-moved-up";
        else if (moved_down(pb, pa))
            result.classification = "primary-moved-down";
        else
            result.classification = "non-rigid-motion";
        break;
    }

    return result;
}

static void print_metric(const struct metric *metric)
{
    if (!metric->present) {
        fputs("null", stdout);
        return;
    }
    printf("{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,"
           "\"pixels\":%" PRIu64 ",\"mask\":\"%016" PRIx64
           "\",\"rgba\":\"%016" PRIx64 "\"}",
           metric->x0, metric->y0, metric->x1 - metric->x0,
           metric->y1 - metric->y0, metric->pixels, metric->mask_hash,
           metric->rgba_hash);
}

static void print_snapshot(const struct snapshot *snapshot)
{
    printf("{\"frameHash\":\"%016" PRIx64 "\",\"primary\":",
           snapshot->frame_hash);
    print_metric(&snapshot->primary);
    fputs(",\"secondary\":", stdout);
    print_metric(&snapshot->secondary);
    fputs(",\"ink\":", stdout);
    print_metric(&snapshot->ink);
    putchar('}');
}

static void print_delta_metric(const struct metric *before,
                               const struct metric *after)
{
    if (!before->present || !after->present) {
        fputs("null", stdout);
        return;
    }
    printf("[%d,%d]", delta_top(before, after), delta_bottom(before, after));
}

static void print_case_result(const struct movement_case *test,
                              const struct snapshot *before,
                              const struct snapshot *after,
                              const struct evaluation *evaluation)
{
    fputs("{\"type\":\"bbox\",\"case\":", stdout);
    print_json_string(test->id);
    fputs(",\"status\":", stdout);
    print_json_string(evaluation->status);
    fputs(",\"classification\":", stdout);
    print_json_string(evaluation->classification);
    fputs(",\"proposition\":", stdout);
    print_json_string(test->proposition);
    fputs(",\"ambiguity\":", stdout);
    if (test->ambiguity)
        print_json_string(test->ambiguity);
    else
        fputs("null", stdout);
    printf(",\"positions\":{\"before\":[%d,%d,%d],\"after\":[%d,%d,%d]},"
           "\"frame\":[%d,%d],\"timeUs\":%" PRId64 ",\"delta\":{"
           "\"primary\":",
           test->before.primary, test->before.secondary, test->before.margin,
           test->after.primary, test->after.secondary, test->after.margin,
           before->width, before->height, before->time_us);
    print_delta_metric(&before->primary, &after->primary);
    fputs(",\"secondary\":", stdout);
    print_delta_metric(&before->secondary, &after->secondary);
    printf("},\"shapeStable\":{\"primary\":%s,\"secondary\":%s,\"ink\":%s},"
           "\"before\":",
           evaluation->primary_shape_stable ? "true" : "false",
           evaluation->secondary_shape_stable ? "true" : "false",
           evaluation->ink_shape_stable ? "true" : "false");
    print_snapshot(before);
    fputs(",\"after\":", stdout);
    print_snapshot(after);
    fputs("}\n", stdout);
    fflush(stdout);
}

static void print_case_start(const struct suite *suite, const char *case_id)
{
    fputs("{\"type\":\"case-start\",\"mode\":", stdout);
    print_json_string(suite->relative_mode ? "relative" : "auto");
    fputs(",\"case\":", stdout);
    print_json_string(case_id);
    fputs("}\n", stdout);
    fflush(stdout);
}

static bool run_movement_case(struct suite *suite,
                              const struct movement_case *test)
{
    struct snapshot before = {0};
    struct snapshot after = {0};
    bool success = false;
    print_case_start(suite, test->id);
    if (!start_player(suite) || !prepare_case(suite, test) ||
        !capture_snapshot(suite, &before) ||
        !set_position_state(suite, test->after) ||
        !capture_snapshot(suite, &after))
        goto done;

    struct evaluation evaluation = evaluate_case(test, &before, &after);
    print_case_result(test, &before, &after, &evaluation);
    if (strcmp(evaluation.status, "PASS") == 0)
        suite->passed++;
    else if (strcmp(evaluation.status, "AMBIGUOUS") == 0)
        suite->ambiguous++;
    else
        suite->failed++;
    success = true;

done:
    free_snapshot(suite, &after);
    free_snapshot(suite, &before);
    stop_player(suite);
    return success;
}

static const char *movement_label(const struct metric *before,
                                  const struct metric *after)
{
    if (!before->present || !after->present)
        return "unavailable";
    if (stationary(before, after))
        return "non-moving";
    if (delta_top(before, after) < 0 && delta_bottom(before, after) < 0)
        return "moved-up";
    if (delta_top(before, after) > 0 && delta_bottom(before, after) > 0)
        return "moved-down";
    return "non-rigid";
}

static void print_top_anchor_result(
    const struct movement_case *test, const char *status,
    const char *classification, const struct snapshot *dual_before,
    const struct snapshot *dual_after,
    const struct snapshot *secondary_reference,
    const struct metric *primary_before, const struct metric *primary_after,
    const struct metric *secondary_before, const struct metric *secondary_after)
{
    const struct metric *secondary_full = &secondary_reference->ink;
    uint64_t reference_pixels = secondary_full->pixels;
    bool clipped_before = !secondary_before->present ||
                          (secondary_before->y0 == 0 &&
                           secondary_before->pixels < reference_pixels);
    bool clipped_after = !secondary_after->present ||
                         (secondary_after->y0 == 0 &&
                          secondary_after->pixels < reference_pixels);
    bool shape_comparable = secondary_before->present &&
                            secondary_after->present &&
                            secondary_before->pixels == reference_pixels &&
                            secondary_after->pixels == reference_pixels;
    bool primary_top_anchored =
        anchor_matches(primary_before, ANCHOR_TOP, dual_before->height);

    fputs("{\"type\":\"top-anchor-bbox\",\"case\":", stdout);
    print_json_string(test->id);
    fputs(",\"status\":", stdout);
    print_json_string(status);
    fputs(",\"classification\":", stdout);
    print_json_string(classification);
    fputs(",\"proposition\":", stdout);
    print_json_string(test->proposition);
    printf(",\"positions\":{\"before\":[%d,%d,%d],\"after\":[%d,%d,%d]},"
           "\"scales\":[0.75,1.0],\"anchorSource\":\"%s\","
           "\"frame\":[%d,%d],\"timeUs\":%" PRId64
           ",\"primaryTopAnchored\":%s,\"rawMovement\":{\"primary\":",
           test->before.primary, test->before.secondary, test->before.margin,
           test->after.primary, test->after.secondary, test->after.margin,
           test->align_y ? "global-sub-align-y-top" : "inline-an8",
           dual_before->width, dual_before->height, dual_before->time_us,
           primary_top_anchored ? "true" : "false");
    print_json_string(movement_label(primary_before, primary_after));
    fputs(",\"secondary\":", stdout);
    print_json_string(movement_label(secondary_before, secondary_after));
    printf("},\"secondaryVisibility\":{\"beforePixels\":%" PRIu64
           ",\"afterPixels\":%" PRIu64 ",\"referencePixels\":%" PRIu64
           ",\"clippedBefore\":%s,\"clippedAfter\":%s,"
           "\"recoveredPixels\":%s},\"shapeComparison\":{"
           "\"comparable\":%s,\"stable\":",
           secondary_before->pixels, secondary_after->pixels, reference_pixels,
           clipped_before ? "true" : "false", clipped_after ? "true" : "false",
           secondary_after->pixels > secondary_before->pixels ? "true"
                                                              : "false",
           shape_comparable ? "true" : "false");
    if (shape_comparable)
        fputs(metric_same_shape(secondary_before, secondary_after) ? "true"
                                                                   : "false",
              stdout);
    else
        fputs("null", stdout);
    fputs("},\"primary\":{\"before\":", stdout);
    print_metric(primary_before);
    fputs(",\"after\":", stdout);
    print_metric(primary_after);
    fputs("},\"secondary\":{\"before\":", stdout);
    print_metric(secondary_before);
    fputs(",\"after\":", stdout);
    print_metric(secondary_after);
    fputs(",\"reference\":", stdout);
    print_metric(secondary_full);
    printf("},\"frameHashes\":{\"dualBefore\":\"%016" PRIx64
           "\",\"dualAfter\":\"%016" PRIx64
           "\",\"secondaryReference\":\"%016" PRIx64 "\"}",
           dual_before->frame_hash, dual_after->frame_hash,
           secondary_reference->frame_hash);
    fputs("}\n", stdout);
    fflush(stdout);
}

static bool run_top_anchor_case(struct suite *suite,
                                const struct movement_case *test)
{
    enum {
        DUAL_BEFORE,
        DUAL_AFTER,
        SECONDARY_REFERENCE,
        TOP_ANCHOR_CAPTURE_COUNT,
    };
    struct snapshot captures[TOP_ANCHOR_CAPTURE_COUNT] = {0};
    struct metric primary_before = {0};
    struct metric primary_after = {0};
    struct metric secondary_before = {0};
    struct metric secondary_after = {0};
    bool success = false;
    print_case_start(suite, test->id);
    if (!start_player(suite) || !prepare_case(suite, test) ||
        !capture_snapshot(suite, &captures[DUAL_BEFORE]) ||
        !set_position_state(suite, test->after) ||
        !capture_snapshot(suite, &captures[DUAL_AFTER]) ||
        !set_position_state(suite, test->before) ||
        !set_property(suite, "sub-visibility", "no") ||
        !set_property(suite, "secondary-sub-visibility", "yes") ||
        !capture_snapshot(suite, &captures[SECONDARY_REFERENCE]) ||
        !set_property(suite, "sub-visibility", "yes"))
        goto done;

    const char *status = "AMBIGUOUS";
    const char *classification;
    bool compatible = true;
    for (int index = 1; index < TOP_ANCHOR_CAPTURE_COUNT; index++) {
        if (captures[index].width != captures[DUAL_BEFORE].width ||
            captures[index].height != captures[DUAL_BEFORE].height ||
            llabs(captures[index].time_us - captures[DUAL_BEFORE].time_us) >
                500) {
            compatible = false;
            break;
        }
    }
    if (!compatible) {
        status = "FAIL";
        classification = "time-or-frame-drift";
    } else {
        int before_count = split_secondary_top_stack(
            &captures[DUAL_BEFORE], &primary_before, &secondary_before);
        int after_count = split_secondary_top_stack(
            &captures[DUAL_AFTER], &primary_after, &secondary_after);
        const struct metric *secondary_full =
            &captures[SECONDARY_REFERENCE].ink;

        if ((before_count != 1 && before_count != 2) ||
            (after_count != 1 && after_count != 2) || !primary_before.present ||
            !primary_after.present || !secondary_full->present) {
            status = "FAIL";
            classification = "reference-render-missing";
        } else if (!anchor_matches(&primary_before, ANCHOR_TOP,
                                   captures[DUAL_BEFORE].height)) {
            classification = "primary-top-anchor-not-realized";
        } else if (!secondary_before.present) {
            classification = "secondary-fully-clipped-before";
        } else if (secondary_before.y0 == 0 &&
                   secondary_before.pixels < secondary_full->pixels) {
            classification = "secondary-partially-clipped-before";
        } else if (secondary_before.pixels < secondary_full->pixels) {
            classification = "secondary-pixel-deficit-without-edge-contact";
        } else {
            classification = "secondary-in-frame-before";
        }
    }

    print_top_anchor_result(
        test, status, classification, &captures[DUAL_BEFORE],
        &captures[DUAL_AFTER], &captures[SECONDARY_REFERENCE], &primary_before,
        &primary_after, &secondary_before, &secondary_after);
    if (strcmp(status, "FAIL") == 0)
        suite->failed++;
    else
        suite->ambiguous++;
    success = true;
done:
    for (int index = TOP_ANCHOR_CAPTURE_COUNT - 1; index >= 0; index--)
        free_snapshot(suite, &captures[index]);
    stop_player(suite);
    return success;
}

struct relative_evaluation {
    const char *status;
    const char *classification;
    int primary_expected_delta;
    int secondary_expected_delta;
    bool primary_shape_stable;
    bool secondary_shape_stable;
    bool ink_comparable;
    bool ink_shape_stable;
    bool shape_comparable;
    bool restored_byte_identical;
};

static bool metric_inside_vertical_frame(const struct metric *metric,
                                         int frame_height);

static int rounded_relative_delta(int before_position, int after_position,
                                  int output_height)
{
    int64_t numerator =
        (int64_t)(after_position - before_position) * output_height;
    if (numerator >= 0)
        return (int)((numerator + 50) / 100);
    return -(int)((-numerator + 50) / 100);
}

static bool translated_by(const struct metric *before,
                          const struct metric *after, int expected_delta)
{
    return before->present && after->present &&
           abs(after->x0 - before->x0) <= 1 &&
           abs(after->x1 - before->x1) <= 1 &&
           abs(delta_top(before, after) - expected_delta) <= 1 &&
           abs(delta_bottom(before, after) - expected_delta) <= 1 &&
           metric_same_shape(before, after);
}

static bool translated_geometry_by(const struct metric *before,
                                   const struct metric *after,
                                   int expected_delta)
{
    return before->present && after->present &&
           abs(after->x0 - before->x0) <= 1 &&
           abs(after->x1 - before->x1) <= 1 &&
           abs(delta_top(before, after) - expected_delta) <= 1 &&
           abs(delta_bottom(before, after) - expected_delta) <= 1;
}

static bool observed_double_shift(const struct metric *before,
                                  const struct metric *after,
                                  int expected_delta)
{
    return expected_delta != 0 && before->present && after->present &&
           abs(delta_top(before, after) - expected_delta * 2) <= 1 &&
           abs(delta_bottom(before, after) - expected_delta * 2) <= 1;
}

static bool restored_exactly(const struct snapshot *before,
                             const struct snapshot *restored)
{
    return before->time_us == restored->time_us &&
           frame_bytes_equal(before, restored) &&
           metric_same_absolute(&before->primary, &restored->primary) &&
           metric_same_absolute(&before->secondary, &restored->secondary) &&
           metric_same_absolute(&before->ink, &restored->ink);
}

static struct relative_evaluation evaluate_relative_case(
    const struct movement_case *test, const struct snapshot *before,
    const struct snapshot *moved, const struct snapshot *restored)
{
    bool expect_primary = test->primary_fixture != FIXTURE_NONE;
    bool expect_secondary = test->secondary_fixture != FIXTURE_NONE;
    struct relative_evaluation result = {
        .status = "FAIL",
        .classification = "unknown",
        .primary_expected_delta =
            expect_primary
                ? rounded_relative_delta(test->before.primary,
                                         test->after.primary, before->height)
                : 0,
        .secondary_expected_delta =
            expect_secondary
                ? rounded_relative_delta(test->before.secondary,
                                         test->after.secondary, before->height)
                : 0,
        .primary_shape_stable =
            metric_same_shape(&before->primary, &moved->primary),
        .secondary_shape_stable =
            metric_same_shape(&before->secondary, &moved->secondary),
        .ink_shape_stable = metric_same_shape(&before->ink, &moved->ink),
        .restored_byte_identical = restored_exactly(before, restored),
    };
    result.shape_comparable =
        metric_inside_vertical_frame(&before->ink, before->height) &&
        metric_inside_vertical_frame(&moved->ink, moved->height);
    result.ink_comparable =
        result.shape_comparable &&
        (!expect_primary || !expect_secondary ||
         result.primary_expected_delta == result.secondary_expected_delta);

    if (before->primary.present != expect_primary ||
        moved->primary.present != expect_primary ||
        restored->primary.present != expect_primary ||
        before->secondary.present != expect_secondary ||
        moved->secondary.present != expect_secondary ||
        restored->secondary.present != expect_secondary ||
        !before->ink.present || !moved->ink.present || !restored->ink.present) {
        result.classification = "missing-or-unexpected-color";
        return result;
    }
    if (before->width != moved->width || before->height != moved->height ||
        before->width != restored->width ||
        before->height != restored->height ||
        llabs(before->time_us - moved->time_us) > 500 ||
        llabs(before->time_us - restored->time_us) > 500) {
        result.classification = "time-or-frame-drift";
        return result;
    }
    if ((expect_primary &&
         (!anchor_matches(&before->primary, test->primary_anchor,
                          before->height) ||
          !anchor_matches(&moved->primary, test->primary_anchor,
                          moved->height) ||
          !anchor_matches(&restored->primary, test->primary_anchor,
                          restored->height))) ||
        (expect_secondary &&
         (!anchor_matches(&before->secondary, test->secondary_anchor,
                          before->height) ||
          !anchor_matches(&moved->secondary, test->secondary_anchor,
                          moved->height) ||
          !anchor_matches(&restored->secondary, test->secondary_anchor,
                          restored->height)))) {
        result.classification = "misanchored";
        return result;
    }
    if (result.shape_comparable &&
        ((expect_primary && !result.primary_shape_stable) ||
         (expect_secondary && !result.secondary_shape_stable))) {
        result.classification = "shape-changed";
        return result;
    }
    if (result.ink_comparable && !result.ink_shape_stable) {
        result.classification = "styled-pixels-changed";
        return result;
    }
    if (!result.restored_byte_identical) {
        result.classification = "restore-not-byte-identical";
        return result;
    }

    bool primary_ok =
        !expect_primary ||
        (result.shape_comparable
             ? translated_by(&before->primary, &moved->primary,
                             result.primary_expected_delta)
             : translated_geometry_by(&before->primary, &moved->primary,
                                      result.primary_expected_delta));
    bool secondary_ok =
        !expect_secondary ||
        (result.shape_comparable
             ? translated_by(&before->secondary, &moved->secondary,
                             result.secondary_expected_delta)
             : translated_geometry_by(&before->secondary, &moved->secondary,
                                      result.secondary_expected_delta));
    int ink_expected_delta = expect_primary ? result.primary_expected_delta
                                            : result.secondary_expected_delta;
    bool ink_ok = !result.ink_comparable ||
                  translated_by(&before->ink, &moved->ink, ink_expected_delta);
    if (primary_ok && secondary_ok && ink_ok) {
        result.status = "PASS";
        result.classification = result.shape_comparable
                                    ? "exact-relative-translation"
                                    : "exact-relative-translation-clipped";
        return result;
    }

    if ((expect_primary &&
         observed_double_shift(&before->primary, &moved->primary,
                               result.primary_expected_delta)) ||
        (expect_secondary &&
         observed_double_shift(&before->secondary, &moved->secondary,
                               result.secondary_expected_delta))) {
        result.classification = "double-shift";
    } else if ((expect_primary && result.primary_expected_delta != 0 &&
                stationary(&before->primary, &moved->primary)) ||
               (expect_secondary && result.secondary_expected_delta != 0 &&
                stationary(&before->secondary, &moved->secondary))) {
        result.classification = "non-moving";
    } else if ((expect_primary && result.primary_expected_delta == 0 &&
                !stationary(&before->primary, &moved->primary)) ||
               (expect_secondary && result.secondary_expected_delta == 0 &&
                !stationary(&before->secondary, &moved->secondary))) {
        result.classification = "cross-track-shift";
    } else if ((expect_primary &&
                (abs(moved->primary.x0 - before->primary.x0) > 1 ||
                 abs(moved->primary.x1 - before->primary.x1) > 1)) ||
               (expect_secondary &&
                (abs(moved->secondary.x0 - before->secondary.x0) > 1 ||
                 abs(moved->secondary.x1 - before->secondary.x1) > 1))) {
        result.classification = "horizontal-drift";
    } else {
        result.classification = "wrong-relative-delta";
    }
    return result;
}

static void print_relative_result(const struct movement_case *test,
                                  const struct snapshot *before,
                                  const struct snapshot *moved,
                                  const struct snapshot *restored,
                                  const struct relative_evaluation *evaluation)
{
    fputs("{\"type\":\"relative-bbox\",\"case\":", stdout);
    print_json_string(test->id);
    fputs(",\"status\":", stdout);
    print_json_string(evaluation->status);
    fputs(",\"classification\":", stdout);
    print_json_string(evaluation->classification);
    fputs(",\"proposition\":", stdout);
    print_json_string(test->proposition);
    printf(",\"positions\":{\"before\":[%d,%d,%d],\"after\":[%d,%d,%d]},"
           "\"frame\":[%d,%d],\"timeUs\":%" PRId64
           ",\"expectedDelta\":{\"primary\":%d,\"secondary\":%d},"
           "\"observedDelta\":{\"primary\":",
           test->before.primary, test->before.secondary, test->before.margin,
           test->after.primary, test->after.secondary, test->after.margin,
           before->width, before->height, before->time_us,
           evaluation->primary_expected_delta,
           evaluation->secondary_expected_delta);
    print_delta_metric(&before->primary, &moved->primary);
    fputs(",\"secondary\":", stdout);
    print_delta_metric(&before->secondary, &moved->secondary);
    printf("},\"shapeComparable\":%s,"
           "\"shapeStable\":{\"primary\":%s,\"secondary\":%s,\"ink\":",
           evaluation->shape_comparable ? "true" : "false",
           evaluation->primary_shape_stable ? "true" : "false",
           evaluation->secondary_shape_stable ? "true" : "false");
    if (evaluation->ink_comparable)
        fputs(evaluation->ink_shape_stable ? "true" : "false", stdout);
    else
        fputs("null", stdout);
    printf("},"
           "\"restoredByteIdentical\":%s,\"before\":",
           evaluation->restored_byte_identical ? "true" : "false");
    print_snapshot(before);
    fputs(",\"moved\":", stdout);
    print_snapshot(moved);
    fputs(",\"restored\":", stdout);
    print_snapshot(restored);
    fputs("}\n", stdout);
    fflush(stdout);
}

static bool run_relative_case(struct suite *suite,
                              const struct movement_case *test)
{
    struct snapshot before = {0};
    struct snapshot moved = {0};
    struct snapshot restored = {0};
    bool success = false;
    print_case_start(suite, test->id);
    if (!start_player(suite) || !prepare_case(suite, test) ||
        !capture_snapshot(suite, &before) ||
        !set_position_state(suite, test->after) ||
        !capture_snapshot(suite, &moved) ||
        !set_position_state(suite, test->before) ||
        !capture_snapshot(suite, &restored))
        goto done;

    struct relative_evaluation evaluation =
        evaluate_relative_case(test, &before, &moved, &restored);
    print_relative_result(test, &before, &moved, &restored, &evaluation);
    if (strcmp(evaluation.status, "PASS") == 0)
        suite->passed++;
    else
        suite->failed++;
    success = true;

done:
    free_snapshot(suite, &restored);
    free_snapshot(suite, &moved);
    free_snapshot(suite, &before);
    stop_player(suite);
    return success;
}

static bool metric_inside_vertical_frame(const struct metric *metric,
                                         int frame_height)
{
    return metric->present && metric->y0 > 0 && metric->y1 < frame_height;
}

static bool relative_geometry_preserved(const struct metric *primary_before,
                                        const struct metric *secondary_before,
                                        const struct metric *primary_after,
                                        const struct metric *secondary_after)
{
    if (!primary_before->present || !secondary_before->present ||
        !primary_after->present || !secondary_after->present)
        return false;
    int top_offset_before = primary_before->y0 - secondary_before->y0;
    int top_offset_after = primary_after->y0 - secondary_after->y0;
    int bottom_offset_before = primary_before->y1 - secondary_before->y1;
    int bottom_offset_after = primary_after->y1 - secondary_after->y1;
    int gap_before = primary_before->y0 - secondary_before->y1;
    int gap_after = primary_after->y0 - secondary_after->y1;
    return gap_before >= 0 && gap_after >= 0 &&
           abs(top_offset_before - top_offset_after) <= 1 &&
           abs(bottom_offset_before - bottom_offset_after) <= 1 &&
           abs(gap_before - gap_after) <= 1;
}

static void print_relative_top_anchor_result(
    const struct movement_case *test, const char *status,
    const char *classification, int primary_delta, int nearby_delta,
    int solo_delta, bool secondary100_clipped,
    bool secondary100_shape_comparable, bool pair_restored, bool solo_restored,
    bool pair_geometry_preserved, const struct snapshot *pair100,
    const struct snapshot *pair120, const struct snapshot *pair125,
    const struct metric *primary100, const struct metric *primary120,
    const struct metric *primary125, const struct metric *secondary100,
    const struct metric *secondary120, const struct metric *secondary125,
    const struct metric *secondary_full, const struct snapshot *solo100,
    const struct snapshot *solo_moved)
{
    bool pair120_full_shape = metric_same_shape(secondary120, secondary_full);
    bool pair125_full_shape = metric_same_shape(secondary125, secondary_full);
    fputs("{\"type\":\"relative-top-anchor\",\"case\":", stdout);
    print_json_string(test->id);
    fputs(",\"status\":", stdout);
    print_json_string(status);
    fputs(",\"classification\":", stdout);
    print_json_string(classification);
    printf(",\"anchorSource\":\"%s\",\"scales\":[0.75,1.0],"
           "\"positions\":{\"pair\":[100,120,125],\"solo\":[100,%d,100]},"
           "\"expectedDelta\":{\"primary100to120\":%d,"
           "\"pair120to125\":%d,\"solo\":%d},"
           "\"secondary100\":{\"clipped\":%s,\"shapeComparable\":%s,"
           "\"visiblePixels\":%" PRIu64 ",\"fullReferencePixels\":%" PRIu64
           "},\"fullShape\":{\"pair120\":%s,\"pair125\":%s},"
           "\"pairGeometryPreserved\":%s,"
           "\"restoredByteIdentical\":{\"pair\":%s,\"solo\":%s},"
           "\"observedDelta\":{\"primary100to120\":",
           test->align_y ? "global-sub-align-y-top" : "inline-an8",
           test->align_y ? 120 : 80, primary_delta, nearby_delta, solo_delta,
           secondary100_clipped ? "true" : "false",
           secondary100_shape_comparable ? "true" : "false",
           secondary100->pixels, secondary_full->pixels,
           pair120_full_shape ? "true" : "false",
           pair125_full_shape ? "true" : "false",
           pair_geometry_preserved ? "true" : "false",
           pair_restored ? "true" : "false", solo_restored ? "true" : "false");
    print_delta_metric(primary100, primary120);
    fputs(",\"primary120to125\":", stdout);
    print_delta_metric(primary120, primary125);
    fputs(",\"secondary120to125\":", stdout);
    print_delta_metric(secondary120, secondary125);
    fputs(",\"solo\":", stdout);
    print_delta_metric(&solo100->ink, &solo_moved->ink);
    fputs("},\"primary\":{\"p100\":", stdout);
    print_metric(primary100);
    fputs(",\"p120\":", stdout);
    print_metric(primary120);
    fputs(",\"p125\":", stdout);
    print_metric(primary125);
    fputs("},\"secondary\":{\"p100Visible\":", stdout);
    print_metric(secondary100);
    fputs(",\"p120Full\":", stdout);
    print_metric(secondary120);
    fputs(",\"p125Full\":", stdout);
    print_metric(secondary125);
    fputs("},\"solo\":{\"baseline\":", stdout);
    print_metric(secondary_full);
    fputs(",\"moved\":", stdout);
    print_metric(&solo_moved->ink);
    printf("},\"frameHashes\":{\"pair100\":\"%016" PRIx64
           "\",\"pair120\":\"%016" PRIx64 "\",\"pair125\":\"%016" PRIx64
           "\",\"solo100\":\"%016" PRIx64 "\",\"soloMoved\":\"%016" PRIx64
           "\"}}\n",
           pair100->frame_hash, pair120->frame_hash, pair125->frame_hash,
           solo100->frame_hash, solo_moved->frame_hash);
    fflush(stdout);
}

static bool run_relative_top_anchor_case(struct suite *suite,
                                         const struct movement_case *test)
{
    enum {
        PAIR100,
        PAIR120,
        PAIR125,
        PAIR100_RESTORED,
        SOLO100,
        SOLO_MOVED,
        SOLO100_RESTORED,
        RELATIVE_TOP_CAPTURE_COUNT,
    };
    struct snapshot captures[RELATIVE_TOP_CAPTURE_COUNT] = {0};
    struct metric primary100 = {0};
    struct metric primary120 = {0};
    struct metric primary125 = {0};
    struct metric secondary100 = {0};
    struct metric secondary120 = {0};
    struct metric secondary125 = {0};
    struct metric secondary_full = {0};
    struct position_state pair120 = {120, 120, 100};
    struct position_state pair125 = {125, 125, 100};
    struct position_state solo_moved_state = {
        100,
        test->align_y ? 120 : 80,
        100,
    };
    bool success = false;
    print_case_start(suite, test->id);

    if (!start_player(suite) || !prepare_case(suite, test) ||
        !capture_snapshot(suite, &captures[PAIR100]) ||
        !set_position_state(suite, pair120) ||
        !capture_snapshot(suite, &captures[PAIR120]) ||
        !set_position_state(suite, pair125) ||
        !capture_snapshot(suite, &captures[PAIR125]) ||
        !set_position_state(suite, test->before) ||
        !capture_snapshot(suite, &captures[PAIR100_RESTORED]))
        goto done;

    stop_player(suite);
    if (!start_player(suite) || !prepare_case(suite, test) ||
        !set_property(suite, "sub-visibility", "no") ||
        !capture_snapshot(suite, &captures[SOLO100]) ||
        !set_position_state(suite, solo_moved_state) ||
        !capture_snapshot(suite, &captures[SOLO_MOVED]) ||
        !set_position_state(suite, test->before) ||
        !capture_snapshot(suite, &captures[SOLO100_RESTORED]))
        goto done;

    const char *status = "PASS";
    const char *classification = "relative-subrip-path-preserved";
    bool compatible =
        captures[PAIR100].width == 640 && captures[PAIR100].height == 360;
    for (int index = 1; index < RELATIVE_TOP_CAPTURE_COUNT; index++) {
        if (captures[index].width != captures[PAIR100].width ||
            captures[index].height != captures[PAIR100].height ||
            llabs(captures[index].time_us - captures[PAIR100].time_us) > 500) {
            compatible = false;
            break;
        }
    }

    int pair100_count = 0;
    int pair120_count = 0;
    int pair125_count = 0;
    if (compatible) {
        pair100_count = split_secondary_top_stack(&captures[PAIR100],
                                                  &primary100, &secondary100);
        pair120_count = split_secondary_top_stack(&captures[PAIR120],
                                                  &primary120, &secondary120);
        pair125_count = split_secondary_top_stack(&captures[PAIR125],
                                                  &primary125, &secondary125);
        secondary_full = captures[SOLO100].ink;
    }

    int primary_delta = rounded_relative_delta(100, 120, 360);
    int nearby_delta = rounded_relative_delta(120, 125, 360);
    int solo_delta =
        rounded_relative_delta(100, solo_moved_state.secondary, 360);
    bool secondary100_clipped =
        !secondary100.present || secondary100.pixels < secondary_full.pixels;
    bool secondary100_shape_comparable =
        secondary100.present && !secondary100_clipped;
    bool pair120_full_shape = metric_same_shape(&secondary120, &secondary_full);
    bool pair125_full_shape = metric_same_shape(&secondary125, &secondary_full);
    bool pair_restored =
        restored_exactly(&captures[PAIR100], &captures[PAIR100_RESTORED]);
    bool solo_restored =
        restored_exactly(&captures[SOLO100], &captures[SOLO100_RESTORED]);
    bool pair_geometry_preserved = relative_geometry_preserved(
        &primary120, &secondary120, &primary125, &secondary125);

    if (!compatible) {
        status = "FAIL";
        classification = "time-or-frame-drift";
    } else if ((pair100_count != 1 && pair100_count != 2) ||
               pair120_count != 2 || pair125_count != 2 ||
               !primary100.present || !primary120.present ||
               !primary125.present || !secondary120.present ||
               !secondary125.present || !secondary_full.present ||
               !captures[SOLO_MOVED].ink.present) {
        status = "FAIL";
        classification = "reference-render-missing";
    } else if (!anchor_matches(&primary100, ANCHOR_TOP, 360)) {
        status = "FAIL";
        classification = "primary-top-anchor-not-realized";
    } else if (!translated_by(&primary100, &primary120, primary_delta)) {
        status = "FAIL";
        classification = "wrong-primary-100-to-120-delta";
    } else if (!metric_inside_vertical_frame(&secondary120, 360) ||
               !metric_inside_vertical_frame(&secondary125, 360)) {
        status = "FAIL";
        classification = "full-shape-reference-clipped";
    } else if (!pair120_full_shape || !pair125_full_shape) {
        status = "FAIL";
        classification = "pair-full-shape-mismatch";
    } else if (secondary100_shape_comparable &&
               !metric_same_shape(&secondary100, &secondary_full)) {
        status = "FAIL";
        classification = "non-clipped-pair100-shape-mismatch";
    } else if (!translated_by(&primary120, &primary125, nearby_delta) ||
               !translated_by(&secondary120, &secondary125, nearby_delta) ||
               !translated_by(&captures[PAIR120].ink, &captures[PAIR125].ink,
                              nearby_delta) ||
               !pair_geometry_preserved) {
        status = "FAIL";
        classification = "pair-relative-geometry-changed";
    } else if (secondary100_shape_comparable &&
               !translated_by(&secondary100, &secondary120, primary_delta)) {
        status = "FAIL";
        classification = "wrong-secondary-100-to-120-delta";
    } else if (!translated_by(&captures[SOLO100].ink, &captures[SOLO_MOVED].ink,
                              solo_delta)) {
        status = "FAIL";
        classification =
            observed_double_shift(&captures[SOLO100].ink,
                                  &captures[SOLO_MOVED].ink, solo_delta)
                ? "solo-double-shift"
                : "wrong-solo-relative-delta";
    } else if (!pair_restored || !solo_restored) {
        status = "FAIL";
        classification = "restore-not-byte-identical";
    }

    print_relative_top_anchor_result(
        test, status, classification, primary_delta, nearby_delta, solo_delta,
        secondary100_clipped, secondary100_shape_comparable, pair_restored,
        solo_restored, pair_geometry_preserved, &captures[PAIR100],
        &captures[PAIR120], &captures[PAIR125], &primary100, &primary120,
        &primary125, &secondary100, &secondary120, &secondary125,
        &secondary_full, &captures[SOLO100], &captures[SOLO_MOVED]);
    if (strcmp(status, "PASS") == 0)
        suite->passed++;
    else
        suite->failed++;
    success = true;

done:
    for (int index = RELATIVE_TOP_CAPTURE_COUNT - 1; index >= 0; index--)
        free_snapshot(suite, &captures[index]);
    stop_player(suite);
    return success;
}

static void print_translation_result(const char *status,
                                     const char *classification,
                                     const struct snapshot *before,
                                     const struct snapshot *hidden,
                                     const struct snapshot *restored)
{
    fputs("{\"type\":\"transition\",\"case\":"
          "\"translation-only-primary-visibility\",\"status\":",
          stdout);
    print_json_string(status);
    fputs(",\"classification\":", stdout);
    print_json_string(classification);
    printf(",\"frame\":[%d,%d],\"timeUs\":%" PRId64
           ",\"secondaryHiddenDelta\":",
           before->width, before->height, before->time_us);
    print_delta_metric(&before->secondary, &hidden->secondary);
    printf(",\"restoredExact\":{\"frame\":%s,\"primary\":%s,"
           "\"secondary\":%s,\"ink\":%s},"
           "\"before\":",
           frame_bytes_equal(before, restored) ? "true" : "false",
           metric_same_absolute(&before->primary, &restored->primary) ? "true"
                                                                      : "false",
           metric_same_absolute(&before->secondary, &restored->secondary)
               ? "true"
               : "false",
           metric_same_absolute(&before->ink, &restored->ink) ? "true"
                                                              : "false");
    print_snapshot(before);
    fputs(",\"hidden\":", stdout);
    print_snapshot(hidden);
    fputs(",\"restored\":", stdout);
    print_snapshot(restored);
    fputs("}\n", stdout);
    fflush(stdout);
}

static bool run_translation_transition(struct suite *suite)
{
    const struct movement_case setup = {
        .id = "translation-only-primary-visibility",
        .proposition = "hidden primary is absent and restoration is exact",
        .primary_fixture = FIXTURE_ASS_BOTTOM,
        .primary_text = "Bottom styled sample",
        .secondary_fixture = FIXTURE_SECONDARY_SRT,
        .secondary_text = "Secondary plain sample",
        .layout = "bottom",
        .order = "secondary-top",
        .before = {100, 100, 100},
        .after = {100, 100, 100},
        .primary_anchor = ANCHOR_BOTTOM,
        .secondary_anchor = ANCHOR_BOTTOM,
    };
    struct snapshot before = {0};
    struct snapshot hidden = {0};
    struct snapshot restored = {0};
    bool success = false;
    print_case_start(suite, setup.id);
    if (!start_player(suite) || !prepare_case(suite, &setup) ||
        !capture_snapshot(suite, &before) ||
        !set_property(suite, "sub-visibility", "no") ||
        !capture_snapshot(suite, &hidden) ||
        !set_property(suite, "sub-visibility", "yes") ||
        !capture_snapshot(suite, &restored))
        goto done;

    const char *status = "PASS";
    const char *classification =
        stationary(&before.secondary, &hidden.secondary) ? "stable-solo-anchor"
                                                         : "solo-reanchored";
    bool time_stable = llabs(before.time_us - hidden.time_us) <= 500 &&
                       llabs(before.time_us - restored.time_us) <= 500;
    bool restored_geometry =
        metric_same_bounds(&before.primary, &restored.primary) &&
        metric_same_bounds(&before.secondary, &restored.secondary) &&
        metric_same_bounds(&before.ink, &restored.ink);

    if (!before.primary.present || !before.secondary.present ||
        hidden.primary.present || !hidden.secondary.present ||
        !restored.primary.present || !restored.secondary.present) {
        status = "FAIL";
        classification = "visibility-state-mismatch";
    } else if (!time_stable) {
        status = "FAIL";
        classification = "time-drift";
    } else if (!anchor_matches(&hidden.secondary, ANCHOR_BOTTOM,
                               hidden.height)) {
        status = "FAIL";
        classification = "misanchored";
    } else if (!restored_geometry) {
        status = "FAIL";
        classification = "restore-geometry-mismatch";
    }

    print_translation_result(status, classification, &before, &hidden,
                             &restored);
    if (strcmp(status, "PASS") == 0)
        suite->passed++;
    else
        suite->failed++;
    success = true;

done:
    free_snapshot(suite, &restored);
    free_snapshot(suite, &hidden);
    free_snapshot(suite, &before);
    stop_player(suite);
    return success;
}

static void print_relative_translation_result(
    const char *status, const char *classification, int expected_delta,
    const struct snapshot *paired_base, const struct snapshot *paired_moved,
    const struct snapshot *solo_base, const struct snapshot *solo_moved,
    const struct snapshot *final, bool paired_restored, bool solo_restored,
    bool final_restored)
{
    fputs("{\"type\":\"relative-transition\",\"case\":"
          "\"translation-only-no-double-shift\",\"status\":",
          stdout);
    print_json_string(status);
    fputs(",\"classification\":", stdout);
    print_json_string(classification);
    printf(",\"expectedDelta\":%d,\"pairedDelta\":{\"primary\":",
           expected_delta);
    print_delta_metric(&paired_base->primary, &paired_moved->primary);
    fputs(",\"secondary\":", stdout);
    print_delta_metric(&paired_base->secondary, &paired_moved->secondary);
    fputs("},\"soloSecondaryDelta\":", stdout);
    print_delta_metric(&solo_base->secondary, &solo_moved->secondary);
    printf(",\"restoredByteIdentical\":{\"paired\":%s,\"solo\":%s,"
           "\"final\":%s},\"pairedBase\":",
           paired_restored ? "true" : "false", solo_restored ? "true" : "false",
           final_restored ? "true" : "false");
    print_snapshot(paired_base);
    fputs(",\"pairedMoved\":", stdout);
    print_snapshot(paired_moved);
    fputs(",\"soloBase\":", stdout);
    print_snapshot(solo_base);
    fputs(",\"soloMoved\":", stdout);
    print_snapshot(solo_moved);
    fputs(",\"final\":", stdout);
    print_snapshot(final);
    fputs("}\n", stdout);
    fflush(stdout);
}

static bool run_relative_translation_transition(struct suite *suite)
{
    const struct movement_case setup = {
        .id = "translation-only-no-double-shift",
        .proposition =
            "relative positioning translates paired and solo secondary once",
        .primary_fixture = FIXTURE_ASS_BOTTOM,
        .primary_text = "Bottom styled sample",
        .secondary_fixture = FIXTURE_SECONDARY_SRT,
        .secondary_text = "Secondary plain sample",
        .layout = "bottom",
        .order = "secondary-top",
        .before = {100, 100, 100},
        .after = {80, 80, 100},
        .primary_anchor = ANCHOR_BOTTOM,
        .secondary_anchor = ANCHOR_BOTTOM,
    };
    enum {
        PAIRED_BASE,
        PAIRED_MOVED,
        PAIRED_RESTORED,
        SOLO_BASE,
        SOLO_MOVED,
        SOLO_RESTORED,
        FINAL_PAIRED,
        CAPTURE_COUNT,
    };
    struct snapshot captures[CAPTURE_COUNT] = {0};
    bool success = false;
    print_case_start(suite, setup.id);

    if (!start_player(suite) || !prepare_case(suite, &setup) ||
        !capture_snapshot(suite, &captures[PAIRED_BASE]) ||
        !set_position_state(suite, setup.after) ||
        !capture_snapshot(suite, &captures[PAIRED_MOVED]) ||
        !set_position_state(suite, setup.before) ||
        !capture_snapshot(suite, &captures[PAIRED_RESTORED]) ||
        !set_property(suite, "sub-visibility", "no") ||
        !capture_snapshot(suite, &captures[SOLO_BASE]) ||
        !set_position_state(suite, setup.after) ||
        !capture_snapshot(suite, &captures[SOLO_MOVED]) ||
        !set_position_state(suite, setup.before) ||
        !capture_snapshot(suite, &captures[SOLO_RESTORED]) ||
        !set_property(suite, "sub-visibility", "yes") ||
        !capture_snapshot(suite, &captures[FINAL_PAIRED]))
        goto done;

    int expected_delta =
        rounded_relative_delta(setup.before.primary, setup.after.primary,
                               captures[PAIRED_BASE].height);
    bool paired_restored =
        restored_exactly(&captures[PAIRED_BASE], &captures[PAIRED_RESTORED]);
    bool solo_restored =
        restored_exactly(&captures[SOLO_BASE], &captures[SOLO_RESTORED]);
    bool final_restored =
        restored_exactly(&captures[PAIRED_BASE], &captures[FINAL_PAIRED]);
    const char *status = "PASS";
    const char *classification = "single-relative-shift";

    for (int index = 1; index < CAPTURE_COUNT; index++) {
        if (captures[index].width != captures[PAIRED_BASE].width ||
            captures[index].height != captures[PAIRED_BASE].height ||
            llabs(captures[index].time_us - captures[PAIRED_BASE].time_us) >
                500) {
            status = "FAIL";
            classification = "time-or-frame-drift";
            break;
        }
    }
    if (strcmp(status, "PASS") == 0 &&
        (!captures[PAIRED_BASE].primary.present ||
         !captures[PAIRED_BASE].secondary.present ||
         !captures[PAIRED_MOVED].primary.present ||
         !captures[PAIRED_MOVED].secondary.present ||
         captures[SOLO_BASE].primary.present ||
         !captures[SOLO_BASE].secondary.present ||
         captures[SOLO_MOVED].primary.present ||
         !captures[SOLO_MOVED].secondary.present ||
         !captures[FINAL_PAIRED].primary.present ||
         !captures[FINAL_PAIRED].secondary.present)) {
        status = "FAIL";
        classification = "visibility-state-mismatch";
    } else if (strcmp(status, "PASS") == 0 &&
               (!anchor_matches(&captures[PAIRED_MOVED].primary, ANCHOR_BOTTOM,
                                captures[PAIRED_MOVED].height) ||
                !anchor_matches(&captures[PAIRED_MOVED].secondary,
                                ANCHOR_BOTTOM, captures[PAIRED_MOVED].height) ||
                !anchor_matches(&captures[SOLO_MOVED].secondary, ANCHOR_BOTTOM,
                                captures[SOLO_MOVED].height))) {
        status = "FAIL";
        classification = "misanchored";
    } else if (strcmp(status, "PASS") == 0 &&
               (!translated_by(&captures[PAIRED_BASE].primary,
                               &captures[PAIRED_MOVED].primary,
                               expected_delta) ||
                !translated_by(&captures[PAIRED_BASE].secondary,
                               &captures[PAIRED_MOVED].secondary,
                               expected_delta) ||
                !translated_by(&captures[PAIRED_BASE].ink,
                               &captures[PAIRED_MOVED].ink, expected_delta) ||
                !translated_by(&captures[SOLO_BASE].secondary,
                               &captures[SOLO_MOVED].secondary,
                               expected_delta) ||
                !translated_by(&captures[SOLO_BASE].ink,
                               &captures[SOLO_MOVED].ink, expected_delta))) {
        status = "FAIL";
        if (observed_double_shift(&captures[PAIRED_BASE].primary,
                                  &captures[PAIRED_MOVED].primary,
                                  expected_delta) ||
            observed_double_shift(&captures[PAIRED_BASE].secondary,
                                  &captures[PAIRED_MOVED].secondary,
                                  expected_delta) ||
            observed_double_shift(&captures[SOLO_BASE].secondary,
                                  &captures[SOLO_MOVED].secondary,
                                  expected_delta)) {
            classification = "double-shift";
        } else {
            classification = "wrong-relative-delta";
        }
    } else if (strcmp(status, "PASS") == 0 &&
               (!paired_restored || !solo_restored || !final_restored)) {
        status = "FAIL";
        classification = "restore-not-byte-identical";
    }

    print_relative_translation_result(
        status, classification, expected_delta, &captures[PAIRED_BASE],
        &captures[PAIRED_MOVED], &captures[SOLO_BASE], &captures[SOLO_MOVED],
        &captures[FINAL_PAIRED], paired_restored, solo_restored,
        final_restored);
    if (strcmp(status, "PASS") == 0)
        suite->passed++;
    else
        suite->failed++;
    success = true;

done:
    for (int index = CAPTURE_COUNT - 1; index >= 0; index--)
        free_snapshot(suite, &captures[index]);
    stop_player(suite);
    return success;
}

static const struct movement_case movement_cases[] = {
    {
        .id = "primary-srt-bottom-sub-pos",
        .proposition = "primary sub-pos 100->80 moves a bottom SRT upward",
        .primary_fixture = FIXTURE_PRIMARY_SRT,
        .primary_text = "Primary plain sample",
        .secondary_fixture = FIXTURE_NONE,
        .layout = "bottom",
        .order = "secondary-top",
        .before = {100, 100, 100},
        .after = {80, 100, 100},
        .expectation = EXPECT_PRIMARY_UP,
        .primary_anchor = ANCHOR_BOTTOM,
        .secondary_anchor = ANCHOR_ANY,
    },
    {
        .id = "secondary-srt-none-secondary-pos",
        .proposition = "secondary-sub-pos 100->80 moves a solo SRT upward "
                       "without stacking",
        .primary_fixture = FIXTURE_NONE,
        .secondary_fixture = FIXTURE_SECONDARY_SRT,
        .secondary_text = "Secondary plain sample",
        .layout = "none",
        .order = "secondary-top",
        .before = {100, 100, 100},
        .after = {100, 80, 100},
        .expectation = EXPECT_SECONDARY_UP,
        .primary_anchor = ANCHOR_ANY,
        .secondary_anchor = ANCHOR_BOTTOM,
    },
    {
        .id = "secondary-srt-bottom-secondary-pos",
        .proposition =
            "secondary-sub-pos 100->80 moves a solo bottom-stack SRT upward",
        .ambiguity = "bottom-solo-remaps-secondary-against-primary",
        .primary_fixture = FIXTURE_NONE,
        .secondary_fixture = FIXTURE_SECONDARY_SRT,
        .secondary_text = "Secondary plain sample",
        .layout = "bottom",
        .order = "secondary-top",
        .before = {100, 100, 100},
        .after = {100, 80, 100},
        .expectation = EXPECT_SECONDARY_UP,
        .primary_anchor = ANCHOR_ANY,
        .secondary_anchor = ANCHOR_BOTTOM,
    },
    {
        .id = "secondary-srt-bottom-both-pos",
        .proposition =
            "matching primary and secondary positions move a solo bottom SRT",
        .ambiguity = "control-for-bottom-solo-position-coupling",
        .primary_fixture = FIXTURE_NONE,
        .secondary_fixture = FIXTURE_SECONDARY_SRT,
        .secondary_text = "Secondary plain sample",
        .layout = "bottom",
        .order = "secondary-top",
        .before = {100, 100, 100},
        .after = {80, 80, 100},
        .expectation = EXPECT_SECONDARY_UP,
        .primary_anchor = ANCHOR_ANY,
        .secondary_anchor = ANCHOR_BOTTOM,
    },
    {
        .id = "ass-bottom-sub-pos",
        .proposition =
            "sub-pos 100->80 moves ordinary bottom-aligned ASS upward",
        .primary_fixture = FIXTURE_ASS_BOTTOM,
        .primary_text = "Bottom styled sample",
        .secondary_fixture = FIXTURE_NONE,
        .layout = "none",
        .order = "secondary-top",
        .before = {100, 100, 100},
        .after = {80, 100, 100},
        .expectation = EXPECT_PRIMARY_UP,
        .primary_anchor = ANCHOR_BOTTOM,
        .secondary_anchor = ANCHOR_ANY,
    },
    {
        .id = "ass-top-sub-pos",
        .proposition = "observe sub-pos 100->80 on author top-aligned ASS",
        .ambiguity = "libass-line-position-may-ignore-top-aligned-ass",
        .primary_fixture = FIXTURE_ASS_TOP,
        .primary_text = "Top styled sample",
        .secondary_fixture = FIXTURE_NONE,
        .layout = "none",
        .order = "secondary-top",
        .before = {100, 100, 100},
        .after = {80, 100, 100},
        .expectation = EXPECT_PRIMARY_OBSERVE,
        .primary_anchor = ANCHOR_TOP,
        .secondary_anchor = ANCHOR_ANY,
    },
    {
        .id = "ass-explicit-pos-sub-pos",
        .proposition =
            "sub-pos changes preserve an explicit ASS pos and styled pixels",
        .primary_fixture = FIXTURE_ASS_POSITION,
        .primary_text = "Position styled sample",
        .secondary_fixture = FIXTURE_NONE,
        .layout = "none",
        .order = "secondary-top",
        .before = {100, 100, 100},
        .after = {80, 100, 100},
        .expectation = EXPECT_PRIMARY_STATIC,
        .primary_anchor = ANCHOR_ANY,
        .secondary_anchor = ANCHOR_ANY,
    },
    {
        .id = "ass-explicit-move-sub-pos",
        .proposition =
            "sub-pos changes preserve an exact-time ASS move and styled pixels",
        .primary_fixture = FIXTURE_ASS_MOVE,
        .primary_text = "Moving styled sample",
        .secondary_fixture = FIXTURE_NONE,
        .layout = "none",
        .order = "secondary-top",
        .before = {100, 100, 100},
        .after = {80, 100, 100},
        .expectation = EXPECT_PRIMARY_STATIC,
        .primary_anchor = ANCHOR_ANY,
        .secondary_anchor = ANCHOR_ANY,
    },
    {
        .id = "dual-bottom-primary-top-both-pos",
        .proposition =
            "Rodel matching positions 100->80 move both bottom-stack tracks up",
        .ambiguity = "bottom-stack-docs-designate-sub-stack-margin",
        .primary_fixture = FIXTURE_ASS_BOTTOM,
        .primary_text = "Bottom styled sample",
        .secondary_fixture = FIXTURE_SECONDARY_SRT,
        .secondary_text = "Secondary plain sample",
        .layout = "bottom",
        .order = "primary-top",
        .before = {100, 100, 100},
        .after = {80, 80, 100},
        .expectation = EXPECT_GROUP_UP,
        .primary_anchor = ANCHOR_BOTTOM,
        .secondary_anchor = ANCHOR_BOTTOM,
    },
    {
        .id = "dual-bottom-secondary-top-both-pos",
        .proposition = "Rodel matching positions 100->80 move both reversed "
                       "bottom tracks up",
        .ambiguity = "bottom-stack-docs-designate-sub-stack-margin",
        .primary_fixture = FIXTURE_ASS_BOTTOM,
        .primary_text = "Bottom styled sample",
        .secondary_fixture = FIXTURE_SECONDARY_SRT,
        .secondary_text = "Secondary plain sample",
        .layout = "bottom",
        .order = "secondary-top",
        .before = {100, 100, 100},
        .after = {80, 80, 100},
        .expectation = EXPECT_GROUP_UP,
        .primary_anchor = ANCHOR_BOTTOM,
        .secondary_anchor = ANCHOR_BOTTOM,
    },
    {
        .id = "dual-bottom-secondary-top-margin",
        .proposition =
            "sub-stack-margin 100->80 moves a bottom stack upward as a group",
        .primary_fixture = FIXTURE_ASS_BOTTOM,
        .primary_text = "Bottom styled sample",
        .secondary_fixture = FIXTURE_SECONDARY_SRT,
        .secondary_text = "Secondary plain sample",
        .layout = "bottom",
        .order = "secondary-top",
        .before = {100, 100, 100},
        .after = {100, 100, 80},
        .expectation = EXPECT_GROUP_UP,
        .primary_anchor = ANCHOR_BOTTOM,
        .secondary_anchor = ANCHOR_BOTTOM,
    },
    {
        .id = "dual-top-primary-top-both-pos",
        .proposition = "Rodel matching positions 100->80 move both top-stack "
                       "tracks together",
        .ambiguity = "top-stack-direction-is-mirrored-from-primary-sub-pos",
        .primary_fixture = FIXTURE_ASS_BOTTOM,
        .primary_text = "Bottom styled sample",
        .secondary_fixture = FIXTURE_SECONDARY_SRT,
        .secondary_text = "Secondary plain sample",
        .layout = "top",
        .order = "primary-top",
        .before = {100, 100, 100},
        .after = {80, 80, 100},
        .expectation = EXPECT_GROUP_ANY,
        .primary_anchor = ANCHOR_TOP,
        .secondary_anchor = ANCHOR_TOP,
    },
    {
        .id = "dual-top-secondary-top-both-pos",
        .proposition = "Rodel matching positions 100->80 move reversed top "
                       "tracks together",
        .ambiguity = "top-stack-direction-is-mirrored-from-primary-sub-pos",
        .primary_fixture = FIXTURE_ASS_BOTTOM,
        .primary_text = "Bottom styled sample",
        .secondary_fixture = FIXTURE_SECONDARY_SRT,
        .secondary_text = "Secondary plain sample",
        .layout = "top",
        .order = "secondary-top",
        .before = {100, 100, 100},
        .after = {80, 80, 100},
        .expectation = EXPECT_GROUP_ANY,
        .primary_anchor = ANCHOR_TOP,
        .secondary_anchor = ANCHOR_TOP,
    },
    {
        .id = "dual-top-secondary-top-margin",
        .proposition = "sub-stack-margin 100->80 moves a top stack as a group",
        .ambiguity =
            "docs-designate-margin-but-native-top-uses-primary-sub-pos",
        .primary_fixture = FIXTURE_ASS_BOTTOM,
        .primary_text = "Bottom styled sample",
        .secondary_fixture = FIXTURE_SECONDARY_SRT,
        .secondary_text = "Secondary plain sample",
        .layout = "top",
        .order = "secondary-top",
        .before = {100, 100, 100},
        .after = {100, 100, 80},
        .expectation = EXPECT_GROUP_ANY,
        .primary_anchor = ANCHOR_TOP,
        .secondary_anchor = ANCHOR_TOP,
    },
    {
        .id = "dual-split-primary-top-both-pos",
        .proposition = "matching positions 100->80 move split tracks inward",
        .ambiguity = "split-mirrors-primary-position-for-secondary",
        .primary_fixture = FIXTURE_ASS_BOTTOM,
        .primary_text = "Bottom styled sample",
        .secondary_fixture = FIXTURE_SECONDARY_SRT,
        .secondary_text = "Secondary plain sample",
        .layout = "split",
        .order = "primary-top",
        .before = {100, 100, 100},
        .after = {80, 80, 100},
        .expectation = EXPECT_SPLIT_INWARD,
        .primary_anchor = ANCHOR_BOTTOM,
        .secondary_anchor = ANCHOR_TOP,
    },
    {
        .id = "dual-split-secondary-top-both-pos",
        .proposition = "matching positions 100->80 move reversed-order split "
                       "tracks inward",
        .ambiguity = "split-order-is-not-used-by-native-layout",
        .primary_fixture = FIXTURE_ASS_BOTTOM,
        .primary_text = "Bottom styled sample",
        .secondary_fixture = FIXTURE_SECONDARY_SRT,
        .secondary_text = "Secondary plain sample",
        .layout = "split",
        .order = "secondary-top",
        .before = {100, 100, 100},
        .after = {80, 80, 100},
        .expectation = EXPECT_SPLIT_INWARD,
        .primary_anchor = ANCHOR_BOTTOM,
        .secondary_anchor = ANCHOR_TOP,
    },
    {
        .id = "dual-split-primary-only-pos",
        .proposition =
            "sub-pos moves only the primary track in documented split layout",
        .ambiguity = "native-split-mirrors-primary-position-for-secondary",
        .primary_fixture = FIXTURE_ASS_BOTTOM,
        .primary_text = "Bottom styled sample",
        .secondary_fixture = FIXTURE_SECONDARY_SRT,
        .secondary_text = "Secondary plain sample",
        .layout = "split",
        .order = "secondary-top",
        .before = {100, 100, 100},
        .after = {80, 100, 100},
        .expectation = EXPECT_PRIMARY_UP_SECONDARY_STILL,
        .primary_anchor = ANCHOR_BOTTOM,
        .secondary_anchor = ANCHOR_TOP,
    },
    {
        .id = "dual-split-secondary-only-pos",
        .proposition = "secondary-sub-pos moves only the secondary track in "
                       "documented split layout",
        .ambiguity = "native-split-postpositions-secondary-from-primary",
        .primary_fixture = FIXTURE_ASS_BOTTOM,
        .primary_text = "Bottom styled sample",
        .secondary_fixture = FIXTURE_SECONDARY_SRT,
        .secondary_text = "Secondary plain sample",
        .layout = "split",
        .order = "secondary-top",
        .before = {100, 100, 100},
        .after = {100, 80, 100},
        .expectation = EXPECT_PRIMARY_STILL_SECONDARY_ANY,
        .primary_anchor = ANCHOR_BOTTOM,
        .secondary_anchor = ANCHOR_TOP,
    },
};

static const struct movement_case top_anchor_cases[] = {
    {
        .id = "srt-global-align-top-bottom-stack-secondary-top",
        .proposition =
            "observe raw 100->120 positioning with global SRT top alignment",
        .primary_fixture = FIXTURE_PRIMARY_GLOBAL_TOP_SRT,
        .primary_text = "Primary global top sample",
        .secondary_fixture = FIXTURE_SECONDARY_SRT,
        .secondary_text = "Secondary plain sample",
        .layout = "bottom",
        .order = "secondary-top",
        .align_y = "top",
        .before = {100, 100, 100},
        .after = {120, 120, 100},
        .primary_anchor = ANCHOR_TOP,
        .secondary_anchor = ANCHOR_ANY,
    },
    {
        .id = "srt-inline-an8-bottom-stack-secondary-top",
        .proposition = "observe raw 100->120 positioning when primary SRT "
                       "carries inline an8",
        .primary_fixture = FIXTURE_PRIMARY_INLINE_TOP_SRT,
        .primary_text = "Primary inline top sample",
        .secondary_fixture = FIXTURE_SECONDARY_SRT,
        .secondary_text = "Secondary plain sample",
        .layout = "bottom",
        .order = "secondary-top",
        .before = {100, 100, 100},
        .after = {120, 120, 100},
        .primary_anchor = ANCHOR_TOP,
        .secondary_anchor = ANCHOR_ANY,
    },
};

static const struct movement_case relative_cases[] = {
    {
        .id = "relative-primary-srt-bottom-80",
        .proposition =
            "relative primary SRT translates by round(-20% output height)",
        .primary_fixture = FIXTURE_PRIMARY_SRT,
        .primary_text = "Primary plain sample",
        .secondary_fixture = FIXTURE_NONE,
        .layout = "bottom",
        .order = "secondary-top",
        .before = {100, 100, 100},
        .after = {80, 100, 100},
        .primary_anchor = ANCHOR_BOTTOM,
        .secondary_anchor = ANCHOR_ANY,
    },
    {
        .id = "relative-secondary-srt-none-80",
        .proposition =
            "relative secondary SRT translates once without stacking",
        .primary_fixture = FIXTURE_NONE,
        .secondary_fixture = FIXTURE_SECONDARY_SRT,
        .secondary_text = "Secondary plain sample",
        .layout = "none",
        .order = "secondary-top",
        .before = {100, 100, 100},
        .after = {100, 80, 100},
        .primary_anchor = ANCHOR_ANY,
        .secondary_anchor = ANCHOR_BOTTOM,
    },
    {
        .id = "relative-secondary-srt-bottom-80",
        .proposition =
            "relative solo bottom secondary translates once from its baseline",
        .primary_fixture = FIXTURE_NONE,
        .secondary_fixture = FIXTURE_SECONDARY_SRT,
        .secondary_text = "Secondary plain sample",
        .layout = "bottom",
        .order = "secondary-top",
        .before = {100, 100, 100},
        .after = {100, 80, 100},
        .primary_anchor = ANCHOR_ANY,
        .secondary_anchor = ANCHOR_BOTTOM,
    },
    {
        .id = "relative-secondary-srt-bottom-both-80",
        .proposition =
            "an absent primary axis does not double-shift a solo secondary",
        .primary_fixture = FIXTURE_NONE,
        .secondary_fixture = FIXTURE_SECONDARY_SRT,
        .secondary_text = "Secondary plain sample",
        .layout = "bottom",
        .order = "secondary-top",
        .before = {100, 100, 100},
        .after = {80, 80, 100},
        .primary_anchor = ANCHOR_ANY,
        .secondary_anchor = ANCHOR_BOTTOM,
    },
    {
        .id = "relative-ass-bottom-80",
        .proposition =
            "relative mode rigidly translates bottom-aligned styled ASS",
        .primary_fixture = FIXTURE_ASS_BOTTOM,
        .primary_text = "Bottom styled sample",
        .secondary_fixture = FIXTURE_NONE,
        .layout = "none",
        .order = "secondary-top",
        .before = {100, 100, 100},
        .after = {80, 100, 100},
        .primary_anchor = ANCHOR_BOTTOM,
        .secondary_anchor = ANCHOR_ANY,
    },
    {
        .id = "relative-ass-top-120",
        .proposition =
            "relative mode rigidly translates top-aligned styled ASS downward",
        .primary_fixture = FIXTURE_ASS_TOP,
        .primary_text = "Top styled sample",
        .secondary_fixture = FIXTURE_NONE,
        .layout = "none",
        .order = "secondary-top",
        .before = {100, 100, 100},
        .after = {120, 100, 100},
        .primary_anchor = ANCHOR_TOP,
        .secondary_anchor = ANCHOR_ANY,
    },
    {
        .id = "relative-ass-explicit-pos-120",
        .proposition =
            "relative mode translates explicit ASS pos without shape loss",
        .primary_fixture = FIXTURE_ASS_POSITION,
        .primary_text = "Position styled sample",
        .secondary_fixture = FIXTURE_NONE,
        .layout = "none",
        .order = "secondary-top",
        .before = {100, 100, 100},
        .after = {120, 100, 100},
        .primary_anchor = ANCHOR_ANY,
        .secondary_anchor = ANCHOR_ANY,
    },
    {
        .id = "relative-ass-explicit-move-80",
        .proposition =
            "relative mode translates exact-time ASS move without shape loss",
        .primary_fixture = FIXTURE_ASS_MOVE,
        .primary_text = "Moving styled sample",
        .secondary_fixture = FIXTURE_NONE,
        .layout = "none",
        .order = "secondary-top",
        .before = {100, 100, 100},
        .after = {80, 100, 100},
        .primary_anchor = ANCHOR_ANY,
        .secondary_anchor = ANCHOR_ANY,
    },
    {
        .id = "relative-dual-bottom-primary-top-80",
        .proposition = "identical relative positions move a primary-top bottom "
                       "stack together",
        .primary_fixture = FIXTURE_ASS_BOTTOM,
        .primary_text = "Bottom styled sample",
        .secondary_fixture = FIXTURE_SECONDARY_SRT,
        .secondary_text = "Secondary plain sample",
        .layout = "bottom",
        .order = "primary-top",
        .before = {100, 100, 100},
        .after = {80, 80, 100},
        .primary_anchor = ANCHOR_BOTTOM,
        .secondary_anchor = ANCHOR_BOTTOM,
    },
    {
        .id = "relative-dual-bottom-secondary-top-80",
        .proposition = "identical relative positions move a secondary-top "
                       "bottom stack together",
        .primary_fixture = FIXTURE_ASS_BOTTOM,
        .primary_text = "Bottom styled sample",
        .secondary_fixture = FIXTURE_SECONDARY_SRT,
        .secondary_text = "Secondary plain sample",
        .layout = "bottom",
        .order = "secondary-top",
        .before = {100, 100, 100},
        .after = {80, 80, 100},
        .primary_anchor = ANCHOR_BOTTOM,
        .secondary_anchor = ANCHOR_BOTTOM,
    },
    {
        .id = "relative-dual-bottom-primary-only-105",
        .proposition =
            "relative primary axis moves only its complete stacked track",
        .primary_fixture = FIXTURE_ASS_BOTTOM,
        .primary_text = "Bottom styled sample",
        .secondary_fixture = FIXTURE_SECONDARY_SRT,
        .secondary_text = "Secondary plain sample",
        .layout = "bottom",
        .order = "secondary-top",
        .before = {100, 100, 100},
        .after = {105, 100, 100},
        .primary_anchor = ANCHOR_BOTTOM,
        .secondary_anchor = ANCHOR_BOTTOM,
    },
    {
        .id = "relative-dual-bottom-secondary-only-80",
        .proposition =
            "relative secondary axis moves only its complete stacked track",
        .primary_fixture = FIXTURE_ASS_BOTTOM,
        .primary_text = "Bottom styled sample",
        .secondary_fixture = FIXTURE_SECONDARY_SRT,
        .secondary_text = "Secondary plain sample",
        .layout = "bottom",
        .order = "secondary-top",
        .before = {100, 100, 100},
        .after = {100, 80, 100},
        .primary_anchor = ANCHOR_BOTTOM,
        .secondary_anchor = ANCHOR_BOTTOM,
    },
    {
        .id = "relative-dual-top-primary-top-120",
        .proposition = "identical relative positions move a primary-top top "
                       "stack together",
        .primary_fixture = FIXTURE_ASS_BOTTOM,
        .primary_text = "Bottom styled sample",
        .secondary_fixture = FIXTURE_SECONDARY_SRT,
        .secondary_text = "Secondary plain sample",
        .layout = "top",
        .order = "primary-top",
        .before = {100, 100, 100},
        .after = {120, 120, 100},
        .primary_anchor = ANCHOR_TOP,
        .secondary_anchor = ANCHOR_TOP,
    },
    {
        .id = "relative-dual-top-secondary-top-120",
        .proposition = "identical relative positions move a secondary-top top "
                       "stack together",
        .primary_fixture = FIXTURE_ASS_BOTTOM,
        .primary_text = "Bottom styled sample",
        .secondary_fixture = FIXTURE_SECONDARY_SRT,
        .secondary_text = "Secondary plain sample",
        .layout = "top",
        .order = "secondary-top",
        .before = {100, 100, 100},
        .after = {120, 120, 100},
        .primary_anchor = ANCHOR_TOP,
        .secondary_anchor = ANCHOR_TOP,
    },
    {
        .id = "relative-dual-split-primary-top-105",
        .proposition = "identical relative positions move primary-top split "
                       "tracks together",
        .primary_fixture = FIXTURE_ASS_BOTTOM,
        .primary_text = "Bottom styled sample",
        .secondary_fixture = FIXTURE_SECONDARY_SRT,
        .secondary_text = "Secondary plain sample",
        .layout = "split",
        .order = "primary-top",
        .before = {100, 100, 100},
        .after = {105, 105, 100},
        .primary_anchor = ANCHOR_BOTTOM,
        .secondary_anchor = ANCHOR_TOP,
    },
    {
        .id = "relative-dual-split-secondary-top-105",
        .proposition =
            "identical relative positions move reversed split tracks together",
        .primary_fixture = FIXTURE_ASS_BOTTOM,
        .primary_text = "Bottom styled sample",
        .secondary_fixture = FIXTURE_SECONDARY_SRT,
        .secondary_text = "Secondary plain sample",
        .layout = "split",
        .order = "secondary-top",
        .before = {100, 100, 100},
        .after = {105, 105, 100},
        .primary_anchor = ANCHOR_BOTTOM,
        .secondary_anchor = ANCHOR_TOP,
    },
};

static void print_error(const struct suite *suite)
{
    fputs("{\"type\":\"error\",\"code\":", stdout);
    print_json_string(suite->error_code);
    fputs(",\"message\":", stdout);
    print_json_string(suite->error_message);
    fputs("}\n", stdout);
}

int main(int argc, char **argv)
{
    bool ordinary_selftest = argc == 2 && strcmp(argv[1], "--selftest") == 0;
    bool relative_selftest =
        argc == 2 && strcmp(argv[1], "--selftest-relative") == 0;
    if (!ordinary_selftest && !relative_selftest) {
        fputs("{\"type\":\"usage\",\"status\":\"FAIL\","
              "\"usage\":\"libmpv-test-subtitle-positioning.exe "
              "(--selftest|--selftest-relative)\"}\n",
              stdout);
        return 64;
    }

    struct suite suite = {0};
    suite.deadline_ms = GetTickCount64() + SELFTEST_TIMEOUT_MS;
    suite.relative_mode = relative_selftest;
    suite.current_playlist_entry_id = -1;

    bool runtime_ok = initialize_paths(&suite) && create_fixtures(&suite) &&
                      load_mpv_api(&suite);

    if (runtime_ok) {
        printf("{\"type\":\"probe\",\"name\":\"libmpv-subtitle-positioning\","
               "\"schema\":2,\"mode\":\"%s\",\"api\":\"%lu.%lu\","
               "\"video\":[640,360,30],"
               "\"timeUs\":%" PRId64 ",\"headless\":true,"
               "\"screenshot\":\"software-rgba\"}\n",
               relative_selftest ? "relative" : "auto", suite.api_version >> 16,
               suite.api_version & 0xffff, PROBE_TIME_US);
        fflush(stdout);

        if (relative_selftest) {
            for (size_t index = 0;
                 runtime_ok && index < ARRAY_SIZE(top_anchor_cases); index++) {
                runtime_ok = run_relative_top_anchor_case(
                    &suite, &top_anchor_cases[index]);
            }
            for (size_t index = 0;
                 runtime_ok && index < ARRAY_SIZE(relative_cases); index++) {
                runtime_ok = run_relative_case(&suite, &relative_cases[index]);
            }
            if (runtime_ok)
                runtime_ok = run_relative_translation_transition(&suite);
        } else {
            for (size_t index = 0;
                 runtime_ok && index < ARRAY_SIZE(top_anchor_cases); index++) {
                runtime_ok =
                    run_top_anchor_case(&suite, &top_anchor_cases[index]);
            }
            for (size_t index = 0;
                 runtime_ok && index < ARRAY_SIZE(movement_cases); index++) {
                runtime_ok = run_movement_case(&suite, &movement_cases[index]);
            }
            if (runtime_ok)
                runtime_ok = run_translation_transition(&suite);
        }
    }

    stop_player(&suite);
    unload_mpv_api(&suite);
    bool cleanup_ok = cleanup_fixtures(&suite);
    if (!cleanup_ok && !suite.error_code[0]) {
        set_error(&suite, "fixture-cleanup",
                  "One or more owned fixture files could not be removed.");
    }
    int leaked = count_owned_resources(&suite);
    if (leaked != 0 && !suite.error_code[0])
        set_error(&suite, "owned-resource-leak",
                  "%d owned resources remained after cleanup.", leaked);
    if (suite.error_code[0])
        print_error(&suite);

    const char *status = suite.error_code[0] ? "ERROR"
                         : suite.failed      ? "FAIL"
                                             : "PASS";
    printf("{\"type\":\"summary\",\"status\":\"%s\",\"pass\":%d,"
           "\"fail\":%d,\"ambiguous\":%d,\"cleanup\":\"%s\","
           "\"leaked\":%d}\n",
           status, suite.passed, suite.failed, suite.ambiguous,
           cleanup_ok ? "PASS" : "FAIL", leaked);
    printf("selftest: subtitle-positioning mode=%s pass=%d fail=%d "
           "ambiguous=%d cleanup=%s leaked=%d => %s\n",
           relative_selftest ? "relative" : "auto", suite.passed, suite.failed,
           suite.ambiguous, cleanup_ok ? "ok" : "bad", leaked, status);
    fflush(stdout);

    if (suite.error_code[0] || !runtime_ok || !cleanup_ok || leaked != 0)
        return 2;
    return suite.failed ? 1 : 0;
}
