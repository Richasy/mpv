// SPDX-License-Identifier: LGPL-2.1-or-later

// Run in a Windows executable without a longPathAware manifest.
// Usage: libmpv-long-paths.exe <absolute-path-to-libmpv-2.dll>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include <mpv/client.h>

static mpv_handle *(*create_client)(void);
static int (*set_option)(mpv_handle *, const char *, const char *);
static int (*initialize)(mpv_handle *);
static int (*command)(mpv_handle *, const char **);
static mpv_event *(*wait_event)(mpv_handle *, double);
static void (*destroy_client)(mpv_handle *);

static int load_file(const char *path, bool expect_loaded)
{
    mpv_handle *client = create_client();
    if (!client)
        return 1;
    int failed = 0;
    const char *options[][2] = {
        {"config", "no"}, {"load-scripts", "no"}, {"terminal", "no"},
        {"vo", "null"}, {"ao", "null"}, {"pause", "yes"},
    };
    for (size_t n = 0; n < sizeof(options) / sizeof(options[0]); n++)
        failed |= set_option(client, options[n][0], options[n][1]) < 0;
    failed |= initialize(client) < 0;
    const char *args[] = {"loadfile", path, NULL};
    if (!failed)
        failed |= command(client, args) < 0;

    bool loaded = false;
    bool ended = false;
    ULONGLONG deadline = GetTickCount64() + 5000;
    while (!failed && !loaded && !ended && GetTickCount64() < deadline) {
        mpv_event *event = wait_event(client, 0.05);
        loaded = event->event_id == MPV_EVENT_FILE_LOADED;
        ended = event->event_id == MPV_EVENT_END_FILE;
    }
    destroy_client(client);
    return failed || (!loaded && !ended) || loaded != expect_loaded;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc != 2)
        return 2;
    HMODULE library = LoadLibraryExW(argv[1], NULL,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!library)
        return 2;

#define RESOLVE(target, name) do { \
    FARPROC address = GetProcAddress(library, name); \
    if (!address) return 2; \
    memcpy(&target, &address, sizeof(target)); \
} while (0)
    RESOLVE(create_client, "mpv_create");
    RESOLVE(set_option, "mpv_set_option_string");
    RESOLVE(initialize, "mpv_initialize");
    RESOLVE(command, "mpv_command");
    RESOLVE(wait_event, "mpv_wait_event");
    RESOLVE(destroy_client, "mpv_terminate_destroy");
#undef RESOLVE

    wchar_t temporary[MAX_PATH];
    wchar_t unique[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, temporary) ||
        !GetTempFileNameW(temporary, L"mpv", 0, unique) ||
        !DeleteFileW(unique))
        return 2;

    wchar_t directories[8][1024];
    int count = 0;
    swprintf(directories[0], 1024, L"\\\\?\\%ls", unique);
    if (!CreateDirectoryW(directories[0], NULL))
        return 2;
    count++;
    while (wcslen(directories[count - 1]) < 320 && count < 8) {
        swprintf(directories[count], 1024,
            L"%ls\\[long path] unicode-\xA789-abcdefghijklmnopqrstuvwxyz",
            directories[count - 1]);
        if (!CreateDirectoryW(directories[count], NULL))
            break;
        count++;
    }

    wchar_t file[1024];
    swprintf(file, 1024, L"%ls\\sample.wav", directories[count - 1]);
    HANDLE output = CreateFileW(file, GENERIC_WRITE, 0, NULL,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    int result = 1;
    if (output != INVALID_HANDLE_VALUE) {
        // One second of mono 8-bit PCM, with a byte-exact RIFF header.
        unsigned char wav[44 + 8000] = {
            'R', 'I', 'F', 'F', 0x64, 0x1f, 0, 0,
            'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0,
            1, 0, 1, 0, 0x40, 0x1f, 0, 0, 0x40, 0x1f, 0, 0,
            1, 0, 8, 0, 'd', 'a', 't', 'a', 0x40, 0x1f, 0, 0,
        };
        memset(wav + 44, 128, 8000);
        DWORD written = 0;
        BOOL wrote = WriteFile(output, wav, sizeof(wav), &written, NULL);
        CloseHandle(output);
        if (wrote && written == sizeof(wav) && wcslen(file) > MAX_PATH) {
            char utf8[4096];
            if (WideCharToMultiByte(CP_UTF8, 0, file, -1,
                                   utf8, sizeof(utf8), NULL, NULL)) {
                result = load_file(utf8, true);
                printf("extended path: %s\n", result ? "FAIL" : "PASS");
            }
            if (!result) {
                if (!WideCharToMultiByte(CP_UTF8, 0, file + 4, -1,
                                        utf8, sizeof(utf8), NULL, NULL)) {
                    result = 1;
                } else {
                    result = load_file(utf8, true);
                    printf("ordinary long path: %s\n", result ? "FAIL" : "PASS");
                    for (char *p = utf8; *p; p++)
                        if (*p == '\\')
                            *p = '/';
                    result |= load_file(utf8, true);
                    printf("forward-slash long path: %s\n", result ? "FAIL" : "PASS");
                    size_t length = strlen(utf8);
                    memcpy(utf8 + length - 3, "bad", 3);
                    result |= load_file(utf8, false);
                }
            }
        }
        result |= !DeleteFileW(file);
    }
    while (count > 0)
        result |= !RemoveDirectoryW(directories[--count]);
    FreeLibrary(library);
    if (!result)
        puts("LONG_PATH_TEST_OK");
    return result;
}
