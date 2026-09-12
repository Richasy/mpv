/*
 * This file is part of mpv.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static bool deny_writes;

static HANDLE WINAPI test_create_file(LPCWSTR name, DWORD access, DWORD share,
    LPSECURITY_ATTRIBUTES attributes, DWORD creation, DWORD flags, HANDLE template)
{
    if (deny_writes && (access & GENERIC_WRITE)) {
        SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    return CreateFileW(name, access, share, attributes, creation, flags, template);
}

#define CreateFileW test_create_file
#include "cache.c"
#undef CreateFileW

int main(void)
{
    wchar_t expected[32768], actual[32768], default_path[32768];
    char error[DLSSNR_ERROR_SIZE] = "";
    wchar_t *base = NULL;
    assert(SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_LocalAppData,
                                         KF_FLAG_DONT_VERIFY, NULL, &base)));
    assert(swprintf(expected, 32768, L"%ls\\mpv\\cache\\dlssnr", base) > 0);
    CoTaskMemFree(base);
    assert(dlssnr_resolve_cache_path(NULL, default_path, 32768, error));
    assert(!wcscmp(default_path, expected));
    assert(dlssnr_resolve_cache_path("", actual, 32768, error));
    assert(!wcscmp(actual, default_path));
    assert(dlssnr_resolve_cache_path("C:/app data/\xe6\xb5\x8b\xe8\xaf\x95/cache",
                                     actual, 32768, error));
    assert(!wcscmp(actual, L"C:\\app data\\\x6d4b\x8bd5\\cache"));
    assert(dlssnr_resolve_cache_path("\\\\server\\share\\cache", actual, 32768, error));
    assert(!wcscmp(actual, L"\\\\server\\share\\cache"));
    assert(!dlssnr_resolve_cache_path("C:relative", actual, 32768, error));
    assert(!dlssnr_resolve_cache_path("relative", actual, 32768, error));
    assert(!dlssnr_resolve_cache_path("\\rooted", actual, 32768, error));
    assert(!dlssnr_resolve_cache_path("C:\\bad\xff", actual, 32768, error));
    assert(!dlssnr_resolve_cache_path(NULL, actual, 8, error));
    assert(!dlssnr_resolve_cache_path("C:\\cache", actual, 4, error));
    assert(error[0]);

    wchar_t temp[32768], root[32768], cwd[32768], nested[32768], cache[32768];
    DWORD length = GetTempPathW(32768, temp);
    assert(length && length < 32768);
    assert(GetTempFileNameW(temp, L"mpv", 0, root));
    assert(DeleteFileW(root));
    assert(CreateDirectoryW(root, NULL));
    length = GetCurrentDirectoryW(32768, cwd);
    assert(length && length < 32768);
    assert(SetCurrentDirectoryW(root));
    assert(dlssnr_resolve_cache_path(NULL, actual, 32768, error));
    assert(!wcscmp(actual, default_path));
    assert(GetFileAttributesW(L"dlssnr-cache") == INVALID_FILE_ATTRIBUTES);
    assert(SetCurrentDirectoryW(cwd));

    assert(swprintf(nested, 32768, L"%ls\\nested", root) > 0);
    assert(swprintf(cache, 32768, L"%ls\\cache", nested) > 0);
    HANDLE lease = dlssnr_acquire_cache_directory(cache, error);
    assert(lease != INVALID_HANDLE_VALUE);
    HANDLE other_lease = dlssnr_acquire_cache_directory(cache, error);
    assert(other_lease != INVALID_HANDLE_VALUE);
    wchar_t lock_path[32768];
    assert(swprintf(lock_path, 32768, L"%ls\\%ls", cache, DLSSNR_CACHE_LOCK_FILENAME) > 0);
    HANDLE exclusive = CreateFileW(lock_path, GENERIC_READ | GENERIC_WRITE, 0,
                                   NULL, OPEN_EXISTING, 0, NULL);
    assert(exclusive == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION);
    assert(CloseHandle(other_lease));
    exclusive = CreateFileW(lock_path, GENERIC_READ | GENERIC_WRITE, 0,
                            NULL, OPEN_EXISTING, 0, NULL);
    assert(exclusive == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION);
    wchar_t retained[32768];
    assert(swprintf(retained, 32768, L"%ls\\retained.bin", cache) > 0);
    HANDLE file = CreateFileW(retained, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
    assert(file != INVALID_HANDLE_VALUE);
    DWORD written;
    assert(WriteFile(file, "keep", 4, &written, NULL) && written == 4);
    assert(CloseHandle(file));
    other_lease = dlssnr_acquire_cache_directory(cache, error);
    assert(other_lease != INVALID_HANDLE_VALUE);
    assert(CloseHandle(other_lease));
    file = CreateFileW(retained, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    assert(file != INVALID_HANDLE_VALUE);
    char content[4];
    DWORD read;
    assert(ReadFile(file, content, 4, &read, NULL) && read == 4);
    assert(!memcmp(content, "keep", 4));
    assert(CloseHandle(file));

    deny_writes = true;
    assert(dlssnr_acquire_cache_directory(cache, error) == INVALID_HANDLE_VALUE);
    assert(strstr(error, "write access") && strstr(error, "Windows error 5"));
    deny_writes = false;
    assert(CloseHandle(lease));
    exclusive = CreateFileW(lock_path, GENERIC_READ | GENERIC_WRITE, 0,
                            NULL, OPEN_EXISTING, 0, NULL);
    assert(exclusive != INVALID_HANDLE_VALUE);
    assert(dlssnr_acquire_cache_directory(cache, error) == INVALID_HANDLE_VALUE);
    assert(strstr(error, "shared lease") && strstr(error, "Windows error 32"));
    assert(CloseHandle(exclusive));
    assert(dlssnr_acquire_cache_directory(retained, error) == INVALID_HANDLE_VALUE);
    assert(strstr(error, "cache") && strstr(error, "Windows error"));
    wchar_t blocked[32768];
    assert(swprintf(blocked, 32768, L"%ls\\child", retained) > 0);
    assert(dlssnr_acquire_cache_directory(blocked, error) == INVALID_HANDLE_VALUE);
    assert(strstr(error, "directory creation"));
    assert(DeleteFileW(retained));
    assert(DeleteFileW(lock_path));
    assert(RemoveDirectoryW(cache));
    assert(RemoveDirectoryW(nested));
    assert(RemoveDirectoryW(root));
    puts("DLSSNR cache paths, write failures, shared/exclusive leases and cleanup passed");
    return 0;
}
