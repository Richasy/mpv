/*
 * This file is part of mpv.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <assert.h>
#include <stdio.h>
#include <wchar.h>

#include "gpu.h"

int main(void)
{
    wchar_t expected[32768], actual[32768], default_path[32768], original_cwd[32768];
    char error[DLSSNR_ERROR_SIZE];
    DWORD length = GetModuleFileNameW(NULL, expected, 32768);
    assert(length > 0 && length < 32768);
    wchar_t *last = wcsrchr(expected, L'\\');
    assert(last);
    static const wchar_t model_suffix[] = L"\\ngx\\nvngx_dlssnr.dll";
    assert((size_t)(last - expected) + sizeof(model_suffix) /
           sizeof(model_suffix[0]) <= 32768);
    wmemcpy(last, model_suffix, sizeof(model_suffix) / sizeof(model_suffix[0]));
    assert(dlssnr_resolve_model_path(NULL, default_path, 32768, error));
    assert(!wcscmp(default_path, expected));
    assert(dlssnr_resolve_model_path("ngx\\nvngx_dlssnr.dll", actual, 32768, error));
    assert(!wcscmp(actual, expected));
    assert(dlssnr_resolve_model_path("ngx/nvngx_dlssnr.dll", actual, 32768, error));
    assert(!wcscmp(actual, expected));

    length = GetCurrentDirectoryW(32768, original_cwd);
    assert(length > 0 && length < 32768);
    wchar_t module_directory[32768];
    length = GetModuleFileNameW(NULL, module_directory, 32768);
    assert(length > 0 && length < 32768);
    wchar_t *slash = wcsrchr(module_directory, L'\\');
    assert(slash);
    *slash = 0;
    assert(SetCurrentDirectoryW(module_directory));
    assert(dlssnr_resolve_model_path("ngx\\nvngx_dlssnr.dll", actual, 32768, error));
    assert(!wcscmp(actual, default_path));
    assert(SetCurrentDirectoryW(original_cwd));

    assert(dlssnr_resolve_model_path("C:\\private\\nvngx_dlssnr.dll", actual, 32768, error));
    assert(!wcscmp(actual, L"C:\\private\\nvngx_dlssnr.dll"));
    assert(dlssnr_resolve_model_path("C:/private/nvngx_dlssnr.dll", actual, 32768, error));
    assert(!wcscmp(actual, L"C:\\private\\nvngx_dlssnr.dll"));
    assert(!dlssnr_resolve_model_path("C:relative.dll", actual, 32768, error));
    assert(!dlssnr_resolve_model_path("\\relative.dll", actual, 32768, error));
    assert(!dlssnr_resolve_model_path("ngx\\nvngx_dlssnr.dll", actual, 8, error));
    puts("DLSSNR model path tests passed (module-relative, never CWD)");
    return 0;
}
