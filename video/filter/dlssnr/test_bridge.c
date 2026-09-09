/*
 * This file is part of mpv.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "bridge.h"

static HMODULE expected_module;
static struct ID3D12Device *const device = (void *)(uintptr_t)0x1000;
static struct ID3D12GraphicsCommandList *const commands = (void *)(uintptr_t)0x2000;
static const struct ngx_parameters *const parameters = (void *)(uintptr_t)0x3000;
static struct ngx_handle *const handle = (void *)(uintptr_t)0x4000;

static void check_caller(void *address)
{
    HMODULE module = NULL;
    assert(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              (const wchar_t *)address, &module));
    assert(module == expected_module);
    wchar_t path[32768];
    DWORD length = GetModuleFileNameW(module, path, 32768);
    assert(length > 0 && length < 32768);
    wchar_t *filename = wcsrchr(path, L'\\');
    assert(filename && !wcscmp(filename + 1, MPV_NGX_BRIDGE_FILENAME));
}

static __declspec(noinline) ngx_result __cdecl check_init(
    uint64_t app, const wchar_t *path, struct ID3D12Device *d, unsigned version,
    const struct ngx_parameters *p)
{
    check_caller(_ReturnAddress());
    assert(app == UINT64_C(0x123456789abcdef0) && !wcscmp(path, L"bridge-test"));
    assert(d == device && version == NGX_API_VERSION && p == parameters);
    return NGX_SUCCESS;
}

static __declspec(noinline) ngx_result __cdecl check_create(
    struct ID3D12GraphicsCommandList *c, int feature,
    const struct ngx_parameters *p, struct ngx_handle **out)
{
    check_caller(_ReturnAddress());
    assert(c == commands && feature == NGX_FEATURE_NR && p == parameters);
    *out = handle;
    return NGX_SUCCESS;
}

static __declspec(noinline) ngx_result __cdecl check_evaluate(
    struct ID3D12GraphicsCommandList *c, const struct ngx_handle *h,
    const struct ngx_parameters *p, void *progress)
{
    check_caller(_ReturnAddress());
    assert(c == commands && h == handle && p == parameters && progress == NULL);
    return NGX_SUCCESS;
}

static __declspec(noinline) ngx_result __cdecl check_release(struct ngx_handle *h)
{
    check_caller(_ReturnAddress());
    assert(h == handle);
    return NGX_SUCCESS;
}

static __declspec(noinline) ngx_result __cdecl check_shutdown(struct ID3D12Device *d)
{
    check_caller(_ReturnAddress());
    assert(d == device);
    return NGX_SUCCESS;
}

static __declspec(noinline) ngx_result __cdecl raise_exception(struct ID3D12Device *d)
{
    check_caller(_ReturnAddress());
    assert(d == device);
    RaiseException(0xe0425056u, 0, 0, NULL);
    return NGX_SUCCESS;
}

int main(void)
{
    wchar_t path[32768];
    DWORD length = GetModuleFileNameW(NULL, path, 32768);
    assert(length > 0 && length < 32768);
    wchar_t *last = wcsrchr(path, L'\\');
    static const wchar_t filename[] = MPV_NGX_BRIDGE_FILENAME;
    assert(last && (size_t)(last + 1 - path) +
           sizeof(filename) / sizeof(filename[0]) <= 32768);
    wmemcpy(last + 1, filename, sizeof(filename) / sizeof(filename[0]));
    expected_module = LoadLibraryExW(path, NULL,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    assert(expected_module);
    FARPROC symbol = GetProcAddress(expected_module, MPV_NGX_BRIDGE_ENTRYPOINT);
    mpv_ngx_bridge_get_api_fn get_api = NULL;
    memcpy(&get_api, &symbol, sizeof(get_api));
    assert(get_api);
    assert(!get_api(0) && !get_api(MPV_NGX_BRIDGE_ABI_VERSION + 1));
    const struct mpv_ngx_bridge_api *api = get_api(MPV_NGX_BRIDGE_ABI_VERSION);
    assert(api && api->abi_version == MPV_NGX_BRIDGE_ABI_VERSION);
    assert(api->struct_size == sizeof(*api));
    assert(api->init(check_init, UINT64_C(0x123456789abcdef0), L"bridge-test",
                     device, NGX_API_VERSION, parameters) == NGX_SUCCESS);
    struct ngx_handle *out = NULL;
    assert(api->create(check_create, commands, NGX_FEATURE_NR, parameters, &out) == NGX_SUCCESS);
    assert(out == handle);
    assert(api->evaluate(check_evaluate, commands, handle, parameters, NULL) == NGX_SUCCESS);
    assert(api->release(check_release, handle) == NGX_SUCCESS);
    assert(api->shutdown(check_shutdown, device) == NGX_SUCCESS);
    assert(ngx_failed(api->shutdown(NULL, device)));
    bool caught = false;
    __try {
        api->shutdown(raise_exception, device);
    } __except(GetExceptionCode() == 0xe0425056u ?
               EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        caught = true;
    }
    assert(caught);
    assert(FreeLibrary(expected_module));
    puts("Bridge ABI, five genuine non-tail-call identities and SEH traversal passed");
    return 0;
}
