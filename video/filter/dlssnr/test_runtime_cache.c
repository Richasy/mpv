/*
 * This file is part of mpv.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <assert.h>

#include "runtime.c"

static unsigned releases;

static ULONG STDMETHODCALLTYPE release_device(ID3D12Device *device)
{
    (void)device;
    releases++;
    return 0;
}

int main(void)
{
    wchar_t temp[32768], root[32768], lock_path[32768];
    DWORD length = GetTempPathW(32768, temp);
    assert(length && length < 32768);
    assert(GetTempFileNameW(temp, L"mpv", 0, root));
    assert(DeleteFileW(root));
    assert(CreateDirectoryW(root, NULL));
    struct dlssnr_gpu_info info = {0};
    HANDLE lease = dlssnr_acquire_cache_directory(root, info.error);
    assert(lease != INVALID_HANDLE_VALUE);
    assert(swprintf(lock_path, 32768, L"%ls\\%ls", root, DLSSNR_CACHE_LOCK_FILENAME) > 0);

    ID3D12DeviceVtbl methods = {.Release = release_device};
    ID3D12Device device = {.lpVtbl = &methods};
    struct dlssnr_runtime *runtime = calloc(1, sizeof(*runtime));
    assert(runtime);
    *runtime = (struct dlssnr_runtime){
        .device = &device,
        .cache_lease = lease,
        .poisoned = true,
    };
    struct dlssnr_runtime *retained = runtime;
    assert(!dlssnr_runtime_close(&runtime, &info));
    assert(runtime == retained && releases == 0);
    HANDLE exclusive = CreateFileW(lock_path, GENERIC_READ | GENERIC_WRITE, 0,
                                   NULL, OPEN_EXISTING, 0, NULL);
    assert(exclusive == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION);

    runtime->poisoned = false;
    assert(dlssnr_runtime_close(&runtime, &info));
    assert(!runtime && releases == 1);
    exclusive = CreateFileW(lock_path, GENERIC_READ | GENERIC_WRITE, 0,
                            NULL, OPEN_EXISTING, 0, NULL);
    assert(exclusive != INVALID_HANDLE_VALUE);
    assert(CloseHandle(exclusive));
    assert(DeleteFileW(lock_path));
    assert(RemoveDirectoryW(root));
    puts("DLSSNR runtime retains cache lease until successful native teardown");
    return 0;
}
