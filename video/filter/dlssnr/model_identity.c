/*
 * This file is part of mpv.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>

#include <stdlib.h>
#include <string.h>

#include "model_identity.h"

static bool load_function(HMODULE module, const char *name, void *target)
{
    FARPROC function = GetProcAddress(module, name);
    memcpy(target, &function, sizeof(function));
    return function != NULL;
}

static bool model_digest(HANDLE file, unsigned char digest[32])
{
    typedef NTSTATUS (WINAPI *open_fn)(BCRYPT_ALG_HANDLE *, LPCWSTR, LPCWSTR, ULONG);
    typedef NTSTATUS (WINAPI *close_fn)(BCRYPT_ALG_HANDLE, ULONG);
    typedef NTSTATUS (WINAPI *create_fn)(BCRYPT_ALG_HANDLE, BCRYPT_HASH_HANDLE *,
                                        PUCHAR, ULONG, PUCHAR, ULONG, ULONG);
    typedef NTSTATUS (WINAPI *data_fn)(BCRYPT_HASH_HANDLE, PUCHAR, ULONG, ULONG);
    typedef NTSTATUS (WINAPI *finish_fn)(BCRYPT_HASH_HANDLE, PUCHAR, ULONG, ULONG);
    typedef NTSTATUS (WINAPI *destroy_fn)(BCRYPT_HASH_HANDLE);
    HMODULE library = LoadLibraryExW(L"bcrypt.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!library)
        return false;
    open_fn open = NULL;
    close_fn close = NULL;
    create_fn create = NULL;
    data_fn data = NULL;
    finish_fn finish = NULL;
    destroy_fn destroy = NULL;
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    unsigned char *buffer = NULL;
    bool valid = false;
    if (!load_function(library, "BCryptOpenAlgorithmProvider", &open) ||
        !load_function(library, "BCryptCloseAlgorithmProvider", &close) ||
        !load_function(library, "BCryptCreateHash", &create) ||
        !load_function(library, "BCryptHashData", &data) ||
        !load_function(library, "BCryptFinishHash", &finish) ||
        !load_function(library, "BCryptDestroyHash", &destroy) ||
        open(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0 ||
        create(algorithm, &hash, NULL, 0, NULL, 0, 0) < 0)
        goto done;
    buffer = malloc(65536);
    if (!buffer)
        goto done;
    LARGE_INTEGER zero = {0};
    if (!SetFilePointerEx(file, zero, NULL, FILE_BEGIN))
        goto done;
    for (;;) {
        DWORD size = 0;
        if (!ReadFile(file, buffer, 65536, &size, NULL))
            goto done;
        if (!size)
            break;
        if (data(hash, buffer, size, 0) < 0)
            goto done;
    }
    valid = finish(hash, digest, 32, 0) >= 0;
done:
    free(buffer);
    if (hash)
        destroy(hash);
    if (algorithm)
        close(algorithm, 0);
    FreeLibrary(library);
    return valid;
}

void dlssnr_model_identify(HANDLE file, struct dlssnr_gpu_info *info)
{
    static const unsigned char reference_digest[32] = {
        0x82, 0x70, 0xb3, 0x50, 0xcd, 0x82, 0xde, 0x5c,
        0xe8, 0x98, 0x06, 0x87, 0x2c, 0xdd, 0x6b, 0x6a,
        0x92, 0x49, 0xb8, 0x08, 0x36, 0xb9, 0x1b, 0xbe,
        0xb3, 0x57, 0x34, 0x70, 0x74, 0x4c, 0xc2, 0x06,
    };
    unsigned char digest[32];
    info->model_signature_mismatch = model_digest(file, digest) &&
        !memcmp(digest, reference_digest, sizeof(digest));
}
