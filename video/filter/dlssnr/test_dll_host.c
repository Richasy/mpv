/*
 * This file is part of mpv.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

typedef int (__cdecl *run_test_fn)(const wchar_t *);

int wmain(int argc, wchar_t **argv)
{
    if (argc != 2)
        return 2;
    wchar_t path[32768];
    DWORD length = GetModuleFileNameW(NULL, path, 32768);
    if (!length || length >= 32768)
        return 1;
    wchar_t *last = wcsrchr(path, L'\\');
    static const wchar_t name[] = L"dlssnr-test-host.dll";
    if (!last || (size_t)(last + 1 - path) + sizeof(name) / sizeof(name[0]) > 32768)
        return 1;
    wmemcpy(last + 1, name, sizeof(name) / sizeof(name[0]));
    HMODULE module = LoadLibraryExW(path, NULL,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module)
        return 1;
    FARPROC symbol = GetProcAddress(module, "dlssnr_gpu_test");
    run_test_fn run = NULL;
    memcpy(&run, &symbol, sizeof(run));
    int result = run ? run(argv[1]) : 1;
    if (!FreeLibrary(module))
        return 1;
    for (unsigned n = 0; n < 200 && GetModuleHandleW(path); n++)
        Sleep(10);
    bool unloaded = GetModuleHandleW(path) == NULL;
    printf("DLL-host-retirement-and-final-module-unload=%s\n", unloaded ? "yes" : "NO");
    return result ? result : unloaded ? 0 : 1;
}
