/*
 * This file is part of mpv.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>

static bool fail_allocations;

static void *test_calloc(size_t count, size_t size)
{
    return fail_allocations ? NULL : calloc(count, size);
}

#define calloc test_calloc
#include "gpu.c"
#undef calloc

static void STDMETHODCALLTYPE no_device(ID3D11Texture2D *texture, ID3D11Device **device)
{
    (void)texture;
    *device = NULL;
}

static void release_lifetime(void *opaque)
{
    unsigned *released = opaque;
    (*released)++;
}

int main(void)
{
    ID3D11Texture2DVtbl functions = {.GetDevice = no_device};
    ID3D11Texture2D texture = {.lpVtbl = &functions};
    unsigned released = 0;
    struct dlssnr_gpu_input input = {
        .texture = &texture, .lifetime = &released,
        .release_lifetime = release_lifetime,
    };
    struct dlssnr_gpu_output output = {0};
    struct dlssnr_gpu_info info = {0};
    struct dlssnr_gpu *gpu = NULL;
    fail_allocations = true;
    assert(!dlssnr_gpu_process(&gpu, &input, &dlssnr_defaults, false, &output, &info));
    assert(!gpu && !output.lease && released == 1);
    assert(info.status == DLSSNR_RUNTIME_FAILED);
    assert(strstr(info.error, "Out of memory"));

    struct dlssnr_gpu failed = {
        .failed = true,
        .info = {.status = DLSSNR_RUNTIME_FAILED},
    };
    snprintf(failed.info.error, sizeof(failed.info.error), "original actionable GPU failure");
    gpu = &failed;
    for (unsigned n = 0; n < 2; n++) {
        assert(!dlssnr_gpu_process(&gpu, &input, &dlssnr_defaults, false, &output, &info));
        assert(gpu == &failed && !output.lease);
        assert(info.status == DLSSNR_RUNTIME_FAILED);
        assert(!strcmp(info.error, "original actionable GPU failure"));
        assert(!strcmp(failed.info.error, "original actionable GPU failure"));
    }
    assert(released == 3);
    puts("GPU context OOM and repeated-failure diagnostic preservation passed");
    return 0;
}
