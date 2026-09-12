/*
 * This file is part of mpv.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#include <knownfolders.h>

#include <stdio.h>
#include <wchar.h>

#include "gpu.h"
#include "params.h"

static bool cache_error(char error[DLSSNR_ERROR_SIZE], const char *operation,
                        DWORD code)
{
    snprintf(error, DLSSNR_ERROR_SIZE,
             "DLSSNR cache %s failed (Windows error %lu); "
             "set cache-path to a writable absolute directory",
             operation, (unsigned long)code);
    return false;
}

bool dlssnr_resolve_cache_path(const char *configured, wchar_t *path,
                              size_t count, char error[DLSSNR_ERROR_SIZE])
{
    if (!path || !count || count > 32768)
        return cache_error(error, "path resolution", ERROR_INVALID_PARAMETER);
    path[0] = 0;
    enum dlssnr_model_path_type type = dlssnr_model_path_type(configured);
    if (type == DLSSNR_MODEL_ABSOLUTE) {
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, configured, -1,
                                 path, (int)count))
            return cache_error(error, "UTF-8 path conversion", GetLastError());
        for (wchar_t *p = path; *p; p++) {
            if (*p == L'/')
                *p = L'\\';
        }
        return true;
    }
    if (type != DLSSNR_MODEL_DEFAULT)
        return cache_error(error, "absolute path validation", ERROR_BAD_PATHNAME);

    /* Unlike mpv config paths, NGX storage is required even with --no-config. */
    wchar_t *base = NULL;
    HRESULT result = SHGetKnownFolderPath(&FOLDERID_LocalAppData,
                                          KF_FLAG_DONT_VERIFY, NULL, &base);
    if (FAILED(result)) {
        snprintf(error, DLSSNR_ERROR_SIZE,
                 "DLSSNR local cache lookup failed (HRESULT 0x%08lx); "
                 "set an explicit writable cache-path", (unsigned long)result);
        return false;
    }
    static const wchar_t suffix[] = L"\\mpv\\cache\\dlssnr";
    size_t length = wcslen(base);
    bool fits = length + sizeof(suffix) / sizeof(suffix[0]) <= count;
    if (fits) {
        wmemcpy(path, base, length);
        wmemcpy(path + length, suffix, sizeof(suffix) / sizeof(suffix[0]));
    }
    CoTaskMemFree(base);
    return fits || cache_error(error, "path resolution", ERROR_INSUFFICIENT_BUFFER);
}

static bool prepare_cache_directory(const wchar_t *path,
                                    char error[DLSSNR_ERROR_SIZE])
{
    int result = SHCreateDirectoryExW(NULL, path, NULL);
    if (result != ERROR_SUCCESS && result != ERROR_ALREADY_EXISTS &&
        result != ERROR_FILE_EXISTS)
        return cache_error(error, "directory creation", result);
    DWORD attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return cache_error(error, "directory inspection", GetLastError());
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY))
        return cache_error(error, "directory inspection", ERROR_DIRECTORY);
    return true;
}

static bool probe_write_access(const wchar_t *path,
                               char error[DLSSNR_ERROR_SIZE])
{
    static LONG sequence;
    wchar_t probe[32768];
    int length = swprintf(probe, sizeof(probe) / sizeof(probe[0]),
        L"%ls\\mpv-dlssnr-write-%lu-%lu-%ld.tmp", path,
        (unsigned long)GetCurrentProcessId(), (unsigned long)GetCurrentThreadId(),
        (long)InterlockedIncrement(&sequence));
    if (length < 0 || (size_t)length >= sizeof(probe) / sizeof(probe[0]))
        return cache_error(error, "write probe path", ERROR_INSUFFICIENT_BUFFER);
    HANDLE file = CreateFileW(probe, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                              FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return cache_error(error, "write access", GetLastError());
    if (!CloseHandle(file))
        return cache_error(error, "write probe cleanup", GetLastError());
    return true;
}

HANDLE dlssnr_acquire_cache_directory(const wchar_t *path,
                                     char error[DLSSNR_ERROR_SIZE])
{
    if (!prepare_cache_directory(path, error))
        return INVALID_HANDLE_VALUE;
    wchar_t lock_path[32768];
    int length = swprintf(lock_path, sizeof(lock_path) / sizeof(lock_path[0]),
                          L"%ls\\%ls", path, DLSSNR_CACHE_LOCK_FILENAME);
    if (length < 0 || (size_t)length >= sizeof(lock_path) / sizeof(lock_path[0])) {
        cache_error(error, "lease path", ERROR_INSUFFICIENT_BUFFER);
        return INVALID_HANDLE_VALUE;
    }
    HANDLE lease = CreateFileW(lock_path, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (lease == INVALID_HANDLE_VALUE) {
        cache_error(error, "shared lease", GetLastError());
        return INVALID_HANDLE_VALUE;
    }
    if (!probe_write_access(path, error)) {
        if (!CloseHandle(lease))
            cache_error(error, "lease cleanup", GetLastError());
        return INVALID_HANDLE_VALUE;
    }
    return lease;
}
