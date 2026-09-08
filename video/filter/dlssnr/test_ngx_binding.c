/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 */

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>
#include <dxgi1_2.h>

#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ngx_abi.h"

_Static_assert(sizeof(void *) == 8, "Win64 pointers required");
_Static_assert(sizeof(struct ngx_common_info) == 40, "NGX common info ABI");
_Static_assert(offsetof(struct ngx_common_info, logging) == 24, "NGX logging ABI");
_Static_assert(sizeof(struct ngx_parameter_vtable) == 17 * 8, "NGX vtable ABI");

typedef HRESULT (WINAPI *create_device_fn)(IUnknown *, D3D_FEATURE_LEVEL,
                                           REFIID, void **);
typedef HRESULT (WINAPI *create_factory_fn)(REFIID, void **);

struct test_state {
    HMODULE core, d3d12, dxgi;
    IDXGIFactory1 *factory;
    IDXGIAdapter1 *adapter;
    ID3D12Device *device;
    ID3D12CommandAllocator *allocator;
    ID3D12GraphicsCommandList *commands;
    ID3D12CommandQueue *queue;
    ID3D12Fence *fence;
    HANDLE event;
    struct ngx_parameters *params;
    struct ngx_handle *feature;
    ngx_destroy_parameters_fn destroy_params;
    ngx_release_fn release;
    ngx_shutdown_fn shutdown;
    bool initialized;
    bool submitted;
};

static CRITICAL_SECTION log_lock;
static const char *stage = "starting";

static void __cdecl ngx_log(const char *message, int level, int feature)
{
    EnterCriticalSection(&log_lock);
    fprintf(stderr, "ngx[%d,%d]: ", level, feature);
    for (const unsigned char *p = (const unsigned char *)message; p && *p; p++)
        fputc(*p >= 32 || *p == '\n' || *p == '\t' ? *p : '?', stderr);
    fputc('\n', stderr);
    LeaveCriticalSection(&log_lock);
}

static bool absolute_path(const wchar_t *path)
{
    return path && ((wcslen(path) >= 3 && path[1] == L':' &&
                     (path[2] == L'\\' || path[2] == L'/')) ||
                    (path[0] == L'\\' && path[1] == L'\\'));
}

static FARPROC resolve(HMODULE module, const char *name)
{
    FARPROC proc = GetProcAddress(module, name);
    if (!proc)
        fprintf(stderr, "missing-export=%s\n", name);
    return proc;
}

#define RESOLVE(dst, module, name) do {                           \
    FARPROC proc_ = resolve(module, name);                       \
    _Static_assert(sizeof(dst) == sizeof(proc_), "pointer ABI"); \
    memcpy(&(dst), &proc_, sizeof(dst));                         \
    if (!(dst))                                                 \
        return 1;                                               \
} while (0)

#define HR_OK(expr) do {                                              \
    HRESULT hr_ = (expr);                                             \
    if (FAILED(hr_)) {                                                \
        fprintf(stderr, #expr ": hresult=0x%08lx\n", (unsigned long)hr_); \
        return 1;                                                    \
    }                                                               \
} while (0)

static bool parameter_roundtrip(struct ngx_parameters *p)
{
    const struct ngx_parameter_vtable *v = p->vtable;
    int i = 0;
    unsigned u = 0;
    uint64_t ull = 0;
    float f = 0;
    double d = 0;
    void *ptr = NULL;
    struct ID3D11Resource *r11 = (void *)(uintptr_t)1;
    struct ID3D12Resource *r12 = (void *)(uintptr_t)1;

    v->set_i(p, "mpv.binding.i", -271);
    v->set_u(p, "mpv.binding.u", 329);
    v->set_ull(p, "mpv.binding.ull", UINT64_C(0x123456789abcdef0));
    v->set_f(p, "mpv.binding.f", 0.75f);
    v->set_d(p, "mpv.binding.d", 0.125);
    v->set_pointer(p, "mpv.binding.ptr", p);
    v->set_d3d11(p, "mpv.binding.r11", NULL);
    v->set_d3d12(p, "mpv.binding.r12", NULL);

    bool ok = true;
#define CHECK(name, expr, condition) do {                                    \
    ngx_result r_ = (expr);                                                 \
    bool valid_ = !ngx_failed(r_) && (condition);                            \
    printf("parameter-%s=0x%08" PRIx32 ",%s\n", name, r_,                    \
           valid_ ? "pass" : "FAIL");                                      \
    ok = ok && valid_;                                                     \
} while (0)
    CHECK("i", v->get_i(p, "mpv.binding.i", &i), i == -271);
    CHECK("u", v->get_u(p, "mpv.binding.u", &u), u == 329);
    CHECK("ull", v->get_ull(p, "mpv.binding.ull", &ull),
          ull == UINT64_C(0x123456789abcdef0));
    CHECK("f", v->get_f(p, "mpv.binding.f", &f), f == 0.75f);
    CHECK("d", v->get_d(p, "mpv.binding.d", &d), d == 0.125);
    CHECK("pointer", v->get_pointer(p, "mpv.binding.ptr", &ptr), ptr == p);
    ngx_result r11_result = v->get_d3d11(p, "mpv.binding.r11", &r11);
    bool r11_valid = (!ngx_failed(r11_result) && r11 == NULL) ||
                     r11_result == 0xbad00010u;
    printf("parameter-d3d11=0x%08" PRIx32 ",%s\n", r11_result,
           r11_valid ? "valid-or-unsupported-for-d3d12" : "FAIL");
    ok = ok && r11_valid;
    CHECK("d3d12", v->get_d3d12(p, "mpv.binding.r12", &r12), r12 == NULL);
    v->set_u(p, "Width", 479);
    CHECK("public-width", v->get_u(p, "Width", &u), u == 479);
#undef CHECK
    stage = "parameter-reset";
    v->reset(p);
    return ok;
}

static ngx_result __cdecl scaling_ratio(struct ngx_parameters *p)
{
    if (!p)
        return 0xbad00005u;
    p->vtable->set_f(p, "DLSSNR.ScalingRatio", 1.0f);
    return NGX_SUCCESS;
}

static void create_parameters(struct ngx_parameters *p)
{
    static const char *const sizes[] = {
        "DLSSNR.Width", "DLSSNR.Height",
        "DLSSNR.InputWidth", "DLSSNR.InputHeight",
        "DLSSNR.OutputWidth", "DLSSNR.OutputHeight",
        "DLSSNR.Output.Width", "DLSSNR.Output.Height", "Width", "Height",
    };
    const struct ngx_parameter_vtable *v = p->vtable;
    for (size_t n = 0; n < sizeof(sizes) / sizeof(sizes[0]); n++)
        v->set_u(p, sizes[n], 256);
    v->set_u(p, "DLSSNR.Upscaling", 0);
    v->set_f(p, "DLSSNR.Scale", 1);
    v->set_f(p, "DLSSNR.ScalingRatio", 1);
    v->set_i(p, "DLSSNR.Hint.Render.Preset", 0);
    v->set_i(p, "PerfQualityValue", 1);
    v->set_u(p, "CreationNodeMask", 1);
    v->set_u(p, "VisibilityNodeMask", 1);
    v->set_i(p, "DLSS.Indicator.Invert.X.Axis", 0);
    v->set_i(p, "DLSS.Indicator.Invert.Y.Axis", 0);
    ngx_result (__cdecl *callback)(struct ngx_parameters *) = scaling_ratio;
    void *address;
    _Static_assert(sizeof(address) == sizeof(callback), "callback ABI");
    memcpy(&address, &callback, sizeof(address));
    v->set_pointer(p, "DLSSNRComputeScalingRatioCallback", address);
}

static bool complete_submission(struct test_state *s)
{
    if (!s->submitted)
        return true;
    uint64_t value = ID3D12Fence_GetCompletedValue(s->fence);
    if (value == UINT64_MAX)
        return true;
    if (value >= 1) {
        s->submitted = false;
        return true;
    }
    HRESULT hr = ID3D12Fence_SetEventOnCompletion(s->fence, 1, s->event);
    if (FAILED(hr) || WaitForSingleObject(s->event, 10000) != WAIT_OBJECT_0)
        return false;
    s->submitted = false;
    return true;
}

static int run_test(struct test_state *s, int argc, wchar_t **argv)
{
    if ((argc != 3 && argc != 4) || !absolute_path(argv[1]) ||
        !absolute_path(argv[2]) ||
        (argc == 4 && wcscmp(argv[3], L"--create18"))) {
        fprintf(stderr, "usage: test_ngx_binding ABSOLUTE_CORE_DLL "
                        "ABSOLUTE_WRITABLE_DIRECTORY [--create18]\n");
        return 2;
    }

    s->core = LoadLibraryExW(argv[1], NULL,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    s->d3d12 = LoadLibraryExW(L"d3d12.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    s->dxgi = LoadLibraryExW(L"dxgi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!s->core || !s->d3d12 || !s->dxgi) {
        fprintf(stderr, "load-error=%lu\n", GetLastError());
        return 1;
    }

    ngx_init_project_fn init;
    ngx_allocate_fn allocate;
    ngx_create_fn create;
    create_device_fn create_device;
    create_factory_fn create_factory;
    RESOLVE(init, s->core, "NVSDK_NGX_D3D12_Init_ProjectID");
    RESOLVE(allocate, s->core, "NVSDK_NGX_D3D12_AllocateParameters");
    RESOLVE(create, s->core, "NVSDK_NGX_D3D12_CreateFeature");
    RESOLVE(s->destroy_params, s->core, "NVSDK_NGX_D3D12_DestroyParameters");
    RESOLVE(s->release, s->core, "NVSDK_NGX_D3D12_ReleaseFeature");
    RESOLVE(s->shutdown, s->core, "NVSDK_NGX_D3D12_Shutdown1");
    RESOLVE(create_device, s->d3d12, "D3D12CreateDevice");
    RESOLVE(create_factory, s->dxgi, "CreateDXGIFactory1");

    printf("c_setter_export=%s\n",
           GetProcAddress(s->core, "NVSDK_NGX_Parameter_SetF") ? "yes" : "no");
    HR_OK(create_factory(&IID_IDXGIFactory1, (void **)&s->factory));
    for (unsigned n = 0; ; n++) {
        IDXGIAdapter1 *adapter = NULL;
        HRESULT hr = IDXGIFactory1_EnumAdapters1(s->factory, n, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND)
            break;
        HR_OK(hr);
        DXGI_ADAPTER_DESC1 desc;
        hr = IDXGIAdapter1_GetDesc1(adapter, &desc);
        if (SUCCEEDED(hr) && desc.VendorId == 0x10de &&
            !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            s->adapter = adapter;
            printf("adapter-luid=%08lx:%08lx\n",
                   (unsigned long)desc.AdapterLuid.HighPart,
                   (unsigned long)desc.AdapterLuid.LowPart);
            break;
        }
        IDXGIAdapter1_Release(adapter);
    }
    if (!s->adapter) {
        fprintf(stderr, "no-nvidia-adapter\n");
        return 1;
    }
    HR_OK(create_device((IUnknown *)s->adapter, D3D_FEATURE_LEVEL_12_0,
                        &IID_ID3D12Device, (void **)&s->device));

    struct ngx_common_info info = {
        .logging = {
            .callback = ngx_log,
            .level = 2,
            .disable_other_sinks = true,
        },
    };
    printf("project-id=b4f841f0-9c9b-4754-8b3a-1c79d4adb09a\n");
    printf("engine=mpv-native-ngx-binding-test-1\n");
    printf("requirements-calls=0\n");
    ngx_result result = init("b4f841f0-9c9b-4754-8b3a-1c79d4adb09a",
                             0, "mpv-native-ngx-binding-test-1",
                             argv[2], s->device, NGX_API_VERSION, &info);
    printf("direct-init-project-id=0x%08" PRIx32 "\n", result);
    if (ngx_failed(result))
        return 1;
    s->initialized = true;
    result = allocate(&s->params);
    printf("allocate-parameters=0x%08" PRIx32 "\n", result);
    if (ngx_failed(result) || !s->params)
        return 1;
    stage = "parameter-roundtrip";
    bool valid = parameter_roundtrip(s->params);
    printf("d3d12-parameter-abi-and-reset=%s\n", valid ? "pass" : "FAIL");
    if (!valid)
        return 1;
    if (argc == 3)
        return 0;

    HR_OK(ID3D12Device_CreateCommandAllocator(s->device,
        D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator,
        (void **)&s->allocator));
    HR_OK(ID3D12Device_CreateCommandList(s->device, 0,
        D3D12_COMMAND_LIST_TYPE_DIRECT, s->allocator, NULL,
        &IID_ID3D12GraphicsCommandList, (void **)&s->commands));
    D3D12_COMMAND_QUEUE_DESC desc = {.Type = D3D12_COMMAND_LIST_TYPE_DIRECT};
    HR_OK(ID3D12Device_CreateCommandQueue(s->device, &desc,
        &IID_ID3D12CommandQueue, (void **)&s->queue));
    HR_OK(ID3D12Device_CreateFence(s->device, 0, D3D12_FENCE_FLAG_NONE,
        &IID_ID3D12Fence, (void **)&s->fence));
    s->event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!s->event)
        return 1;
    create_parameters(s->params);
    result = create(s->commands, NGX_FEATURE_NR, s->params, &s->feature);
    printf("direct-core-create-feature18=0x%08" PRIx32 "\n", result);
    printf("feature-handle=%s\n", s->feature ? "non-null" : "null");
    printf("evaluate-calls=0\n");
    if (ngx_failed(result))
        return 3;
    if (!s->feature)
        return 1;
    HR_OK(ID3D12GraphicsCommandList_Close(s->commands));
    ID3D12CommandList *lists[] = {(ID3D12CommandList *)s->commands};
    ID3D12CommandQueue_ExecuteCommandLists(s->queue, 1, lists);
    s->submitted = true;
    HR_OK(ID3D12CommandQueue_Signal(s->queue, s->fence, 1));
    if (!complete_submission(s)) {
        fprintf(stderr, "gpu-completion=timeout-or-error\n");
        return 1;
    }
    printf("feature-creation-gpu-completion=yes\n");
    return 0;
}

static bool cleanup(struct test_state *s)
{
    if (!complete_submission(s)) {
        fprintf(stderr, "in-flight-work-retained-until-test-process-exit\n");
        return false;
    }
    bool valid = true;
    if (s->feature && s->release) {
        ngx_result result = s->release(s->feature);
        printf("release-feature=0x%08" PRIx32 "\n", result);
        valid = valid && !ngx_failed(result);
    }
    if (s->params && s->destroy_params) {
        stage = "destroy-parameters";
        ngx_result result = s->destroy_params(s->params);
        printf("destroy-parameters=0x%08" PRIx32 "\n", result);
        valid = valid && !ngx_failed(result);
    }
    if (s->initialized && s->shutdown) {
        stage = "shutdown";
        unsigned remaining_devices = UINT_MAX;
        ngx_result result = s->shutdown(s->device, &remaining_devices);
        printf("shutdown=0x%08" PRIx32 "\n", result);
        printf("remaining-ngx-devices=%u\n", remaining_devices);
        valid = valid && !ngx_failed(result) && remaining_devices == 0;
    }
    if (!valid) {
        fprintf(stderr, "failed-runtime-retained-until-test-process-exit\n");
        return false;
    }
    if (s->commands)
        ID3D12GraphicsCommandList_Release(s->commands);
    if (s->allocator)
        ID3D12CommandAllocator_Release(s->allocator);
    if (s->queue)
        ID3D12CommandQueue_Release(s->queue);
    if (s->fence)
        ID3D12Fence_Release(s->fence);
    if (s->event)
        CloseHandle(s->event);
    if (s->device)
        ID3D12Device_Release(s->device);
    if (s->adapter)
        IDXGIAdapter1_Release(s->adapter);
    if (s->factory)
        IDXGIFactory1_Release(s->factory);
    if (s->core)
        FreeLibrary(s->core);
    if (s->d3d12)
        FreeLibrary(s->d3d12);
    if (s->dxgi)
        FreeLibrary(s->dxgi);
    return true;
}

int wmain(int argc, wchar_t **argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    setvbuf(stdout, NULL, _IONBF, 0);
    InitializeCriticalSection(&log_lock);
    struct test_state s = {0};
    int result;
    bool cleaned;
#if defined(_MSC_VER)
    __try {
#endif
        result = run_test(&s, argc, argv);
        cleaned = cleanup(&s);
        if (!cleaned)
            result = 1;
#if defined(_MSC_VER)
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        fprintf(stderr, "native-exception=0x%08lx;stage=%s\n",
                GetExceptionCode(), stage);
        return 4;
    }
#endif
    if (cleaned)
        DeleteCriticalSection(&log_lock);
    printf("test-result=%d\n", result);
    return result;
}
