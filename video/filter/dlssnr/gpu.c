/*
 * This file is part of mpv.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef COBJMACROS
#define COBJMACROS
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <initguid.h>
#include <d3d10_1.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_2.h>
struct ID3D10Effect;
#include <d3dcompiler.h>

#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu.h"
#include "runtime.h"

#define OUTPUT_SLOTS 4
#define SHADER_COUNT 5
#define UAV_BASE 16
#define DESCRIPTOR_COUNT 32

#define DROP(pointer) do {                       \
    if (pointer) {                              \
        IUnknown_Release((IUnknown *)(pointer)); \
        (pointer) = NULL;                       \
    }                                          \
} while (0)

enum texture_id {
    TEX_SOURCE,
    TEX_HORIZONTAL,
    TEX_INPUT,
    TEX_ENHANCED,
    TEX_RESIDUAL,
    TEX_COMPOSITE,
    TEX_MOTION,
    TEX_DEPTH,
    TEX_COUNT,
};

struct gpu_texture {
    ID3D12Resource *resource;
    D3D12_RESOURCE_STATES state;
    DXGI_FORMAT format;
    unsigned descriptor;
};

struct output_pool;

struct output_slot {
    struct output_pool *pool;
    ID3D11Texture2D *texture;
    _Atomic bool leased;
    uint64_t retired_at;
};

struct output_pool {
    _Atomic unsigned references;
    _Atomic bool failed;
    ID3D11Device5 *device;
    ID3D11DeviceContext4 *context;
    ID3D10Multithread *multithread;
    // This consumer-only fence is independent of the inference handoff fence.
    ID3D11Fence *fence;
    uint64_t next_value;
    HMODULE module_pin, d3d11_module;
    struct output_slot slots[OUTPUT_SLOTS];
};

typedef HRESULT (WINAPI *create_device_fn)(IUnknown *, D3D_FEATURE_LEVEL,
                                          REFIID, void **);
typedef HRESULT (WINAPI *serialize_root_fn)(const D3D12_ROOT_SIGNATURE_DESC *,
                                           D3D_ROOT_SIGNATURE_VERSION,
                                           ID3DBlob **, ID3DBlob **);
typedef HRESULT (WINAPI *compile_fn)(LPCVOID, SIZE_T, LPCSTR,
    const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT,
    ID3DBlob **, ID3DBlob **);

struct dlssnr_gpu {
    HMODULE d3d12_module, compiler_module, module_pin;
    ID3D11Device *device11;
    ID3D11Device5 *device11_5;
    ID3D11DeviceContext4 *context11;
    ID3D10Multithread *multithread;
    ID3D12Device *device12;
    ID3D12CommandQueue *queue;
    ID3D12CommandAllocator *allocator;
    ID3D12GraphicsCommandList *commands;
    ID3D12Fence *fence12;
    ID3D11Fence *fence11;
    HANDLE fence_event;
    uint64_t sequence, pending_value, feature_serial;
    bool pending, failed, initialized;
    ID3D12RootSignature *root;
    ID3D12PipelineState *shaders[SHADER_COUNT];
    ID3D12DescriptorHeap *descriptors, *rtv_descriptors;
    unsigned descriptor_stride, rtv_stride;
    ID3D11Texture2D *input_staging, *shared_output;
    struct gpu_texture staging;
    struct gpu_texture textures[TEX_COUNT];
    struct output_pool *pool;
    struct dlssnr_runtime *runtime;
    struct dlssnr_gpu_info info;
    D3D11_TEXTURE2D_DESC input_desc;
    int width, height, proc_width, proc_height, preset;
    wchar_t *model_path;
    void *input_lifetime;
    void (*release_input)(void *);
};

static const unsigned char module_anchor = 0;

static HMODULE pin_module(void)
{
    HMODULE module = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                      GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                      (const wchar_t *)&module_anchor, &module);
    if (!module || module == GetModuleHandleW(NULL))
        return NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                      (const wchar_t *)&module_anchor, &module);
    return module;
}

static bool hr_ok(struct dlssnr_gpu_info *info, const char *operation, HRESULT hr)
{
    if (SUCCEEDED(hr))
        return true;
    info->status = DLSSNR_RUNTIME_FAILED;
    snprintf(info->error, sizeof(info->error), "%s failed (HRESULT 0x%08lx)",
             operation, (unsigned long)hr);
    return false;
}

#define GPU_HR(expression) do {                              \
    if (!hr_ok(&g->info, #expression, (expression)))          \
        return false;                                       \
} while (0)

static bool symbol(HMODULE module, const char *name, void *destination)
{
    FARPROC proc = GetProcAddress(module, name);
    memcpy(destination, &proc, sizeof(proc));
    return proc != NULL;
}

static D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle(ID3D12DescriptorHeap *heap)
{
    D3D12_CPU_DESCRIPTOR_HANDLE result;
    heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(heap, &result);
    return result;
}

static D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle(ID3D12DescriptorHeap *heap)
{
    D3D12_GPU_DESCRIPTOR_HANDLE result;
    heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(heap, &result);
    return result;
}

static void CALLBACK pool_cleanup(PTP_CALLBACK_INSTANCE instance, void *opaque)
{
    struct output_pool *pool = opaque;
    uint64_t completed = ID3D11Fence_GetCompletedValue(pool->fence);
    bool removed = FAILED(ID3D11Device5_GetDeviceRemovedReason(pool->device));
    if (!removed && completed != UINT64_MAX && completed < pool->next_value) {
        HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
        HRESULT hr = event ? ID3D11Fence_SetEventOnCompletion(
            pool->fence, pool->next_value, event) : E_OUTOFMEMORY;
        bool done = SUCCEEDED(hr) && WaitForSingleObject(event, 30000) == WAIT_OBJECT_0;
        if (event)
            CloseHandle(event);
        if (!done && SUCCEEDED(ID3D11Device5_GetDeviceRemovedReason(pool->device))) {
            OutputDebugStringA("mpv dlssnr: retaining output pool with unfinished GPU reads\n");
            return;
        }
    }
    HMODULE pin = pool->module_pin;
    HMODULE d3d11 = pool->d3d11_module;
    for (unsigned n = 0; n < OUTPUT_SLOTS; n++)
        DROP(pool->slots[n].texture);
    DROP(pool->fence);
    DROP(pool->context);
    DROP(pool->multithread);
    DROP(pool->device);
    free(pool);
    if (d3d11)
        FreeLibrary(d3d11);
    // A callback has one unload slot; reserve it for the executing mpv module.
    if (pin)
        FreeLibraryWhenCallbackReturns(instance, pin);
}

static void pool_unref(struct output_pool *pool)
{
    if (!pool)
        return;
    if (atomic_fetch_sub_explicit(&pool->references, 1, memory_order_acq_rel) == 1) {
        if (!TrySubmitThreadpoolCallback(pool_cleanup, pool, NULL))
            OutputDebugStringA("mpv dlssnr: retaining output pool after cleanup scheduling failure\n");
    }
}

static void release_output(void *opaque)
{
    struct output_slot *slot = opaque;
    struct output_pool *pool = slot->pool;
    ID3D10Multithread_Enter(pool->multithread);
    // Assign in actual queue-submission order, never in frame/slot order.
    uint64_t value = ++pool->next_value;
    HRESULT hr = ID3D11DeviceContext4_Signal(pool->context, pool->fence, value);
    ID3D11DeviceContext4_Flush(pool->context);
    ID3D10Multithread_Leave(pool->multithread);
    if (FAILED(hr))
        atomic_store_explicit(&pool->failed, true, memory_order_release);
    slot->retired_at = value;
    atomic_store_explicit(&slot->leased, false, memory_order_release);
    pool_unref(pool);
}

static struct output_pool *pool_create(struct dlssnr_gpu *g)
{
    struct output_pool *pool = calloc(1, sizeof(*pool));
    if (!pool)
        return NULL;
    atomic_init(&pool->references, 1);
    atomic_init(&pool->failed, false);
    pool->module_pin = pin_module();
    GetModuleHandleExW(0, L"d3d11.dll", &pool->d3d11_module);
    pool->device = g->device11_5;
    pool->context = g->context11;
    pool->multithread = g->multithread;
    ID3D11Device5_AddRef(pool->device);
    ID3D11DeviceContext4_AddRef(pool->context);
    ID3D10Multithread_AddRef(pool->multithread);
    HRESULT hr = ID3D11Device5_CreateFence(pool->device, 0, D3D11_FENCE_FLAG_NONE,
        &IID_ID3D11Fence, (void **)&pool->fence);
    D3D11_TEXTURE2D_DESC desc = {
        .Width = (g->width + 1) & ~1, .Height = (g->height + 1) & ~1,
        .MipLevels = 1, .ArraySize = 1,
        .Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
        .SampleDesc.Count = 1,
        .Usage = D3D11_USAGE_DEFAULT,
        .BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
    };
    for (unsigned n = 0; SUCCEEDED(hr) && n < OUTPUT_SLOTS; n++) {
        pool->slots[n].pool = pool;
        atomic_init(&pool->slots[n].leased, false);
        hr = ID3D11Device5_CreateTexture2D(pool->device, &desc, NULL,
                                         &pool->slots[n].texture);
    }
    if (FAILED(hr)) {
        hr_ok(&g->info, "D3D11 output pool allocation", hr);
        for (unsigned n = 0; n < OUTPUT_SLOTS; n++)
            DROP(pool->slots[n].texture);
        DROP(pool->fence);
        DROP(pool->device);
        DROP(pool->context);
        DROP(pool->multithread);
        if (pool->module_pin)
            FreeLibrary(pool->module_pin);
        if (pool->d3d11_module)
            FreeLibrary(pool->d3d11_module);
        free(pool);
        return NULL;
    }
    return pool;
}

static struct output_slot *pool_acquire(struct output_pool *pool)
{
    if (atomic_load_explicit(&pool->failed, memory_order_acquire))
        return NULL;
    uint64_t completed = ID3D11Fence_GetCompletedValue(pool->fence);
    if (completed == UINT64_MAX)
        return NULL;
    for (unsigned n = 0; n < OUTPUT_SLOTS; n++) {
        struct output_slot *slot = &pool->slots[n];
        if (!atomic_load_explicit(&slot->leased, memory_order_acquire) &&
            completed >= slot->retired_at) {
            atomic_store_explicit(&slot->leased, true, memory_order_release);
            atomic_fetch_add_explicit(&pool->references, 1, memory_order_relaxed);
            return slot;
        }
    }
    return NULL;
}

static bool wait_fence(struct dlssnr_gpu *g, uint64_t value, DWORD timeout)
{
    uint64_t completed = ID3D12Fence_GetCompletedValue(g->fence12);
    if (completed == UINT64_MAX) {
        g->pending = false;
        hr_ok(&g->info, "D3D12 device removed",
              ID3D12Device_GetDeviceRemovedReason(g->device12));
        return false;
    }
    if (completed < value) {
        HRESULT hr = ID3D12Fence_SetEventOnCompletion(g->fence12, value, g->fence_event);
        if (!hr_ok(&g->info, "GPU completion event", hr))
            return false;
        if (WaitForSingleObject(g->fence_event, timeout) != WAIT_OBJECT_0) {
            g->info.status = DLSSNR_RUNTIME_FAILED;
            snprintf(g->info.error, sizeof(g->info.error),
                     "GPU completion timed out; in-flight resources retained");
            return false;
        }
        completed = ID3D12Fence_GetCompletedValue(g->fence12);
        if (completed == UINT64_MAX) {
            g->pending = false;
            g->info.status = DLSSNR_RUNTIME_FAILED;
            snprintf(g->info.error, sizeof(g->info.error), "GPU device removed");
            return false;
        }
        if (completed < value) {
            g->info.status = DLSSNR_RUNTIME_FAILED;
            snprintf(g->info.error, sizeof(g->info.error),
                     "GPU completion event did not reach the requested fence value");
            return false;
        }
    }
    g->pending = false;
    return true;
}

static void transition(struct dlssnr_gpu *g, struct gpu_texture *texture,
                       D3D12_RESOURCE_STATES state)
{
    if (texture->state == state)
        return;
    D3D12_RESOURCE_BARRIER barrier = {
        .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Transition = {
            .pResource = texture->resource,
            .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            .StateBefore = texture->state,
            .StateAfter = state,
        },
    };
    ID3D12GraphicsCommandList_ResourceBarrier(g->commands, 1, &barrier);
    texture->state = state;
}

static bool create_texture(struct dlssnr_gpu *g, enum texture_id id, int w, int h,
                           DXGI_FORMAT format, bool shared)
{
    struct gpu_texture *texture = &g->textures[id];
    texture->format = format;
    texture->descriptor = 2 + id;
    D3D12_HEAP_PROPERTIES heap = {
        .Type = D3D12_HEAP_TYPE_DEFAULT,
        .CreationNodeMask = 1, .VisibleNodeMask = 1,
    };
    D3D12_RESOURCE_DESC desc = {
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Width = w, .Height = h, .DepthOrArraySize = 1, .MipLevels = 1,
        .Format = format, .SampleDesc.Count = 1,
        .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        .Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
                 D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                 (shared ? D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS : 0),
    };
    D3D12_CLEAR_VALUE clear = {.Format = format};
    GPU_HR(ID3D12Device_CreateCommittedResource(g->device12, &heap,
        shared ? D3D12_HEAP_FLAG_SHARED : D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COMMON, &clear, &IID_ID3D12Resource,
        (void **)&texture->resource));
    texture->state = D3D12_RESOURCE_STATE_COMMON;
    D3D12_CPU_DESCRIPTOR_HANDLE handle = cpu_handle(g->descriptors);
    handle.ptr += texture->descriptor * g->descriptor_stride;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {
        .Format = format, .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
        .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
        .Texture2D.MipLevels = 1,
    };
    ID3D12Device_CreateShaderResourceView(g->device12, texture->resource, &srv, handle);
    handle = cpu_handle(g->descriptors);
    handle.ptr += (UAV_BASE + id) * g->descriptor_stride;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {
        .Format = format, .ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D,
    };
    ID3D12Device_CreateUnorderedAccessView(g->device12, texture->resource,
                                          NULL, &uav, handle);
    return true;
}

static bool create_pipeline(struct dlssnr_gpu *g)
{
    serialize_root_fn serialize;
    compile_fn compile;
    if (!symbol(g->d3d12_module, "D3D12SerializeRootSignature", &serialize) ||
        !symbol(g->compiler_module, "D3DCompile", &compile)) {
        g->info.status = DLSSNR_RUNTIME_MISSING;
        snprintf(g->info.error, sizeof(g->info.error), "D3D12 shader compiler APIs missing");
        return false;
    }
    D3D12_DESCRIPTOR_RANGE ranges[4] = {0};
    D3D12_ROOT_PARAMETER roots[5] = {0};
    for (unsigned n = 0; n < 4; n++) {
        ranges[n].RangeType = n == 3 ? D3D12_DESCRIPTOR_RANGE_TYPE_UAV :
                                      D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[n].NumDescriptors = 1;
        ranges[n].BaseShaderRegister = n == 3 ? 0 : n;
        roots[n].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        roots[n].DescriptorTable.NumDescriptorRanges = 1;
        roots[n].DescriptorTable.pDescriptorRanges = &ranges[n];
    }
    roots[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    roots[4].Constants.Num32BitValues = sizeof(struct dlssnr_shader_config) / 4;
    D3D12_STATIC_SAMPLER_DESC sampler = {
        .Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        .AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        .AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        .AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        .MaxAnisotropy = 1, .ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS,
        .MaxLOD = D3D12_FLOAT32_MAX,
    };
    D3D12_ROOT_SIGNATURE_DESC desc = {
        .NumParameters = 5, .pParameters = roots,
        .NumStaticSamplers = 1, .pStaticSamplers = &sampler,
    };
    ID3DBlob *blob = NULL, *error = NULL;
    HRESULT hr = serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &error);
    DROP(error);
    if (!hr_ok(&g->info, "Root signature serialization", hr))
        return false;
    hr = ID3D12Device_CreateRootSignature(g->device12, 0,
        ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
        &IID_ID3D12RootSignature, (void **)&g->root);
    DROP(blob);
    if (!hr_ok(&g->info, "Root signature creation", hr))
        return false;
    static const char *const entries[] = {
        "convert_source", "reduce_horizontal", "reduce_vertical",
        "prepare_residual", "compose_residual",
    };
    for (unsigned n = 0; n < SHADER_COUNT; n++) {
        hr = compile(dlssnr_shader_source, strlen(dlssnr_shader_source),
            "mpv-original-dlssnr", NULL, NULL, entries[n], "cs_5_0",
            D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
            0, &blob, &error);
        if (FAILED(hr)) {
            g->info.status = DLSSNR_RUNTIME_FAILED;
            snprintf(g->info.error, sizeof(g->info.error), "Shader %s: %.380s",
                entries[n], error ? (char *)ID3D10Blob_GetBufferPointer(error) : "failed");
            DROP(error);
            DROP(blob);
            return false;
        }
        DROP(error);
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline = {
            .pRootSignature = g->root,
            .CS = {
                .pShaderBytecode = ID3D10Blob_GetBufferPointer(blob),
                .BytecodeLength = ID3D10Blob_GetBufferSize(blob),
            },
        };
        hr = ID3D12Device_CreateComputePipelineState(g->device12, &pipeline,
            &IID_ID3D12PipelineState, (void **)&g->shaders[n]);
        DROP(blob);
        if (!hr_ok(&g->info, "Compute pipeline creation", hr))
            return false;
    }
    D3D12_DESCRIPTOR_HEAP_DESC descriptors = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        .NumDescriptors = DESCRIPTOR_COUNT,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
    };
    GPU_HR(ID3D12Device_CreateDescriptorHeap(g->device12, &descriptors,
        &IID_ID3D12DescriptorHeap, (void **)&g->descriptors));
    descriptors.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    descriptors.NumDescriptors = 2;
    descriptors.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    GPU_HR(ID3D12Device_CreateDescriptorHeap(g->device12, &descriptors,
        &IID_ID3D12DescriptorHeap, (void **)&g->rtv_descriptors));
    g->descriptor_stride = ID3D12Device_GetDescriptorHandleIncrementSize(
        g->device12, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g->rtv_stride = ID3D12Device_GetDescriptorHandleIncrementSize(
        g->device12, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    return true;
}

static void dispatch(struct dlssnr_gpu *g, unsigned shader, unsigned a, unsigned b,
                     enum texture_id destination,
                     const struct dlssnr_shader_config *settings, int w, int h)
{
    ID3D12DescriptorHeap *heaps[] = {g->descriptors};
    ID3D12GraphicsCommandList_SetDescriptorHeaps(g->commands, 1, heaps);
    ID3D12GraphicsCommandList_SetComputeRootSignature(g->commands, g->root);
    ID3D12GraphicsCommandList_SetPipelineState(g->commands, g->shaders[shader]);
    unsigned descriptors[] = {a, b, a, UAV_BASE + destination};
    for (unsigned n = 0; n < 4; n++) {
        D3D12_GPU_DESCRIPTOR_HANDLE handle = gpu_handle(g->descriptors);
        handle.ptr += (uint64_t)descriptors[n] * g->descriptor_stride;
        ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(g->commands, n, handle);
    }
    ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(g->commands, 4,
        sizeof(*settings) / 4, settings, 0);
    ID3D12GraphicsCommandList_Dispatch(g->commands, (w + 15) / 16, (h + 15) / 16, 1);
}

static bool create_device(struct dlssnr_gpu *g, ID3D11Device *source_device)
{
    g->device11 = source_device;
    ID3D11Device_AddRef(g->device11);
    g->module_pin = pin_module();
    GPU_HR(ID3D11Device_QueryInterface(g->device11, &IID_ID3D11Device5,
                                        (void **)&g->device11_5));
    ID3D11DeviceContext *context = NULL;
    ID3D11Device_GetImmediateContext(g->device11, &context);
    HRESULT hr = ID3D11DeviceContext_QueryInterface(context, &IID_ID3D11DeviceContext4,
                                                    (void **)&g->context11);
    DROP(context);
    if (!hr_ok(&g->info, "D3D11 fence-capable context", hr))
        return false;
    GPU_HR(ID3D11Device_QueryInterface(g->device11, &IID_ID3D10Multithread,
                                       (void **)&g->multithread));
    ID3D10Multithread_SetMultithreadProtected(g->multithread, TRUE);

    IDXGIDevice *dxgi = NULL;
    IDXGIAdapter *adapter = NULL;
    GPU_HR(ID3D11Device_QueryInterface(g->device11, &IID_IDXGIDevice, (void **)&dxgi));
    hr = IDXGIDevice_GetAdapter(dxgi, &adapter);
    DROP(dxgi);
    if (!hr_ok(&g->info, "Active decoder DXGI adapter", hr))
        return false;
    DXGI_ADAPTER_DESC adapter_desc;
    hr = IDXGIAdapter_GetDesc(adapter, &adapter_desc);
    if (FAILED(hr) || adapter_desc.VendorId != 0x10de) {
        DROP(adapter);
        g->info.status = DLSSNR_ADAPTER_MISMATCH;
        snprintf(g->info.error, sizeof(g->info.error),
                 "The active D3D11 decoder device is not an NVIDIA adapter");
        return false;
    }
    g->info.luid = adapter_desc.AdapterLuid;
    WideCharToMultiByte(CP_UTF8, 0, adapter_desc.Description, -1,
        g->info.gpu_name, sizeof(g->info.gpu_name), NULL, NULL);
    g->d3d12_module = LoadLibraryExW(L"d3d12.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    g->compiler_module = LoadLibraryExW(L"d3dcompiler_47.dll", NULL,
                                       LOAD_LIBRARY_SEARCH_SYSTEM32);
    create_device_fn create = NULL;
    if (!g->d3d12_module || !g->compiler_module ||
        !symbol(g->d3d12_module, "D3D12CreateDevice", &create)) {
        DROP(adapter);
        g->info.status = DLSSNR_RUNTIME_MISSING;
        snprintf(g->info.error, sizeof(g->info.error), "D3D12 or shader compiler missing");
        return false;
    }
    hr = create((IUnknown *)adapter, D3D_FEATURE_LEVEL_12_0, &IID_ID3D12Device,
                (void **)&g->device12);
    DROP(adapter);
    if (!hr_ok(&g->info, "D3D12 on the active decoder adapter", hr))
        return false;
    LUID luid;
    g->device12->lpVtbl->GetAdapterLuid(g->device12, &luid);
    if (luid.LowPart != g->info.luid.LowPart || luid.HighPart != g->info.luid.HighPart) {
        g->info.status = DLSSNR_ADAPTER_MISMATCH;
        snprintf(g->info.error, sizeof(g->info.error), "D3D11/D3D12 adapter LUID mismatch");
        return false;
    }
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support = {.Format = DXGI_FORMAT_B8G8R8A8_UNORM};
    GPU_HR(ID3D12Device_CheckFeatureSupport(g->device12, D3D12_FEATURE_FORMAT_SUPPORT,
                                           &support, sizeof(support)));
    if (!(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)) {
        g->info.status = DLSSNR_UNSUPPORTED_FORMAT;
        snprintf(g->info.error, sizeof(g->info.error),
                 "The decoder adapter does not support BGRA typed UAV writes");
        return false;
    }
    D3D12_COMMAND_QUEUE_DESC queue = {.Type = D3D12_COMMAND_LIST_TYPE_DIRECT};
    GPU_HR(ID3D12Device_CreateCommandQueue(g->device12, &queue,
        &IID_ID3D12CommandQueue, (void **)&g->queue));
    GPU_HR(ID3D12Device_CreateCommandAllocator(g->device12, D3D12_COMMAND_LIST_TYPE_DIRECT,
        &IID_ID3D12CommandAllocator, (void **)&g->allocator));
    GPU_HR(ID3D12Device_CreateCommandList(g->device12, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        g->allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&g->commands));
    GPU_HR(ID3D12GraphicsCommandList_Close(g->commands));
    GPU_HR(ID3D12Device_CreateFence(g->device12, 0, D3D12_FENCE_FLAG_SHARED,
        &IID_ID3D12Fence, (void **)&g->fence12));
    HANDLE shared = NULL;
    GPU_HR(ID3D12Device_CreateSharedHandle(g->device12, (ID3D12DeviceChild *)g->fence12,
        NULL, GENERIC_ALL, NULL, &shared));
    hr = ID3D11Device5_OpenSharedFence(g->device11_5, shared,
        &IID_ID3D11Fence, (void **)&g->fence11);
    CloseHandle(shared);
    if (!hr_ok(&g->info, "D3D11/D3D12 shared fence", hr))
        return false;
    g->fence_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!g->fence_event)
        return false;
    return create_pipeline(g);
}

static bool create_input_staging(struct dlssnr_gpu *g)
{
    D3D11_TEXTURE2D_DESC desc = {
        .Width = g->input_desc.Width, .Height = g->input_desc.Height,
        .MipLevels = 1, .ArraySize = 1, .Format = g->input_desc.Format,
        .SampleDesc.Count = 1, .Usage = D3D11_USAGE_DEFAULT,
        .BindFlags = D3D11_BIND_SHADER_RESOURCE,
        .MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE,
    };
    GPU_HR(ID3D11Device_CreateTexture2D(g->device11, &desc, NULL, &g->input_staging));
    IDXGIResource1 *resource = NULL;
    GPU_HR(ID3D11Texture2D_QueryInterface(g->input_staging, &IID_IDXGIResource1,
                                          (void **)&resource));
    HANDLE handle = NULL;
    HRESULT hr = IDXGIResource1_CreateSharedHandle(resource, NULL,
        DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, NULL, &handle);
    DROP(resource);
    if (!hr_ok(&g->info, "D3D11 shared decoder staging handle", hr))
        return false;
    hr = ID3D12Device_OpenSharedHandle(g->device12, handle, &IID_ID3D12Resource,
                                      (void **)&g->staging.resource);
    CloseHandle(handle);
    if (!hr_ok(&g->info, "Import decoder staging on the same adapter", hr))
        return false;
    g->staging.state = D3D12_RESOURCE_STATE_COMMON;
    bool planar = desc.Format == DXGI_FORMAT_NV12 || desc.Format == DXGI_FORMAT_P010;
    for (unsigned n = 0; n < 2; n++) {
        DXGI_FORMAT format = desc.Format;
        if (planar) {
            format = desc.Format == DXGI_FORMAT_NV12 ?
                (n ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R8_UNORM) :
                (n ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R16_UNORM);
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC view = {
            .Format = format, .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
            .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
            .Texture2D = {.MipLevels = 1, .PlaneSlice = planar ? n : 0},
        };
        D3D12_CPU_DESCRIPTOR_HANDLE destination = cpu_handle(g->descriptors);
        destination.ptr += n * g->descriptor_stride;
        ID3D12Device_CreateShaderResourceView(g->device12, g->staging.resource,
                                             &view, destination);
    }
    return true;
}

static void free_size_resources(struct dlssnr_gpu *g)
{
    for (unsigned n = 0; n < TEX_COUNT; n++)
        DROP(g->textures[n].resource);
    DROP(g->staging.resource);
    DROP(g->input_staging);
    DROP(g->shared_output);
    pool_unref(g->pool);
    g->pool = NULL;
    g->initialized = false;
}

static bool resize_resources(struct dlssnr_gpu *g, bool full_size)
{
    if (full_size) {
        free_size_resources(g);
        if (!create_input_staging(g) ||
            !create_texture(g, TEX_SOURCE, g->width, g->height,
                            DXGI_FORMAT_R16G16B16A16_FLOAT, false) ||
            !create_texture(g, TEX_COMPOSITE, (g->width + 1) & ~1, (g->height + 1) & ~1,
                            DXGI_FORMAT_R16G16B16A16_FLOAT, true))
            return false;
    } else {
        static const enum texture_id internal[] = {
            TEX_HORIZONTAL, TEX_INPUT, TEX_ENHANCED, TEX_RESIDUAL, TEX_MOTION, TEX_DEPTH,
        };
        for (size_t n = 0; n < sizeof(internal) / sizeof(internal[0]); n++)
            DROP(g->textures[internal[n]].resource);
        g->initialized = false;
    }
    if (!create_texture(g, TEX_HORIZONTAL, g->proc_width, g->height,
                        DXGI_FORMAT_R16G16B16A16_FLOAT, false) ||
        !create_texture(g, TEX_INPUT, g->proc_width, g->proc_height,
                        DXGI_FORMAT_B8G8R8A8_UNORM, false) ||
        !create_texture(g, TEX_ENHANCED, g->proc_width, g->proc_height,
                        DXGI_FORMAT_B8G8R8A8_UNORM, false) ||
        !create_texture(g, TEX_RESIDUAL, g->proc_width, g->proc_height,
                        DXGI_FORMAT_R16G16B16A16_FLOAT, false) ||
        !create_texture(g, TEX_MOTION, g->proc_width, g->proc_height,
                        DXGI_FORMAT_R16G16_FLOAT, false) ||
        !create_texture(g, TEX_DEPTH, g->proc_width, g->proc_height,
                        DXGI_FORMAT_R32_FLOAT, false))
        return false;
    if (!full_size)
        return true;
    HANDLE handle = NULL;
    GPU_HR(ID3D12Device_CreateSharedHandle(g->device12,
        (ID3D12DeviceChild *)g->textures[TEX_COMPOSITE].resource,
        NULL, GENERIC_ALL, NULL, &handle));
    HRESULT hr = ID3D11Device5_OpenSharedResource1(g->device11_5, handle,
        &IID_ID3D11Texture2D, (void **)&g->shared_output);
    CloseHandle(handle);
    if (!hr_ok(&g->info, "D3D11 RGB result import", hr))
        return false;
    g->pool = pool_create(g);
    return g->pool != NULL;
}

static bool begin_commands(struct dlssnr_gpu *g)
{
    GPU_HR(ID3D12CommandAllocator_Reset(g->allocator));
    GPU_HR(ID3D12GraphicsCommandList_Reset(g->commands, g->allocator, NULL));
    return true;
}

static bool submit_commands(struct dlssnr_gpu *g)
{
    GPU_HR(ID3D12GraphicsCommandList_Close(g->commands));
    ID3D12CommandList *lists[] = {(ID3D12CommandList *)g->commands};
    ID3D12CommandQueue_ExecuteCommandLists(g->queue, 1, lists);
    g->pending_value = ++g->sequence;
    g->pending = true;
    GPU_HR(ID3D12CommandQueue_Signal(g->queue, g->fence12, g->pending_value));
    return true;
}

static bool initialize_feature(struct dlssnr_gpu *g, int preset)
{
    if (!begin_commands(g))
        return false;
    for (unsigned n = 0; n < 2; n++) {
        struct gpu_texture *texture = &g->textures[n ? TEX_DEPTH : TEX_MOTION];
        D3D12_CPU_DESCRIPTOR_HANDLE handle = cpu_handle(g->rtv_descriptors);
        handle.ptr += n * g->rtv_stride;
        D3D12_RENDER_TARGET_VIEW_DESC rtv = {
            .Format = texture->format, .ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D,
        };
        ID3D12Device_CreateRenderTargetView(g->device12, texture->resource, &rtv, handle);
        transition(g, texture, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float zero[4] = {0};
        ID3D12GraphicsCommandList_ClearRenderTargetView(g->commands, handle, zero, 0, NULL);
        transition(g, texture, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    if (!dlssnr_runtime_create(g->runtime, g->commands, g->proc_width,
                               g->proc_height, preset, &g->info) ||
        !submit_commands(g) || !wait_fence(g, g->pending_value, 30000))
        return false;
    g->preset = preset;
    g->initialized = true;
    g->feature_serial++;
    g->info.feature_builds++;
    return true;
}

static bool configure(struct dlssnr_gpu *g, const struct dlssnr_gpu_input *input,
                      const struct dlssnr_options *options)
{
    int width = input->color.width, height = input->color.height;
    int proc_width, proc_height;
    if (!dlssnr_processing_extent(options, width, height, &proc_width, &proc_height))
        return false;
    D3D11_TEXTURE2D_DESC desc;
    ID3D11Texture2D_GetDesc(input->texture, &desc);
    if (width < 1 || height < 1 || width > 16384 || height > 16384 ||
        desc.Width < (unsigned)width || desc.Height < (unsigned)height ||
        desc.Width > 16384 || desc.Height > 16384 || desc.MipLevels != 1 ||
        desc.SampleDesc.Count != 1 || input->subresource >= desc.ArraySize ||
        (desc.Format != DXGI_FORMAT_NV12 && desc.Format != DXGI_FORMAT_P010 &&
         desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
         desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT)) {
        g->info.status = DLSSNR_UNSUPPORTED_FORMAT;
        snprintf(g->info.error, sizeof(g->info.error),
                 "Unsupported D3D11 source format, extent, or subresource");
        return false;
    }
    wchar_t path[32768];
    if (!dlssnr_resolve_model_path(options->model_path, path, 32768, g->info.error))
        return false;
    bool model_changed = !g->model_path || wcscmp(g->model_path, path);
    if (model_changed) {
        if (!dlssnr_runtime_close(&g->runtime, &g->info))
            return false;
        g->runtime = dlssnr_runtime_open(g->device12, path, &g->info);
        if (!g->runtime)
            return false;
        g->info.runtime_loads++;
        size_t count = wcslen(path) + 1;
        wchar_t *copy = malloc(count * sizeof(wchar_t));
        if (!copy)
            return false;
        wmemcpy(copy, path, count);
        free(g->model_path);
        g->model_path = copy;
    }
    bool full_size = !g->pool || g->width != width || g->height != height ||
        g->input_desc.Width != desc.Width || g->input_desc.Height != desc.Height ||
        g->input_desc.Format != desc.Format;
    bool resized = full_size || !g->initialized ||
        g->proc_width != proc_width || g->proc_height != proc_height;
    bool rebuild = resized || model_changed || g->preset != options->preset;
    if (rebuild && !dlssnr_runtime_release_feature(g->runtime, &g->info))
        return false;
    g->width = width;
    g->height = height;
    g->proc_width = proc_width;
    g->proc_height = proc_height;
    g->input_desc = desc;
    g->info.proc_width = proc_width;
    g->info.proc_height = proc_height;
    if (resized && !resize_resources(g, full_size))
        return false;
    if (rebuild && !initialize_feature(g, options->preset))
        return false;
    return true;
}

static void release_input(struct dlssnr_gpu *g)
{
    if (g->input_lifetime && g->release_input)
        g->release_input(g->input_lifetime);
    g->input_lifetime = NULL;
    g->release_input = NULL;
}

static bool free_gpu(struct dlssnr_gpu *g)
{
    if (g->pending && !wait_fence(g, g->pending_value, 30000) &&
        SUCCEEDED(ID3D12Device_GetDeviceRemovedReason(g->device12)))
        return false;
    if (!dlssnr_runtime_close(&g->runtime, &g->info))
        return false;
    release_input(g);
    free_size_resources(g);
    DROP(g->root);
    for (unsigned n = 0; n < SHADER_COUNT; n++)
        DROP(g->shaders[n]);
    DROP(g->descriptors);
    DROP(g->rtv_descriptors);
    DROP(g->commands);
    DROP(g->allocator);
    DROP(g->queue);
    DROP(g->fence11);
    DROP(g->fence12);
    DROP(g->device12);
    DROP(g->context11);
    DROP(g->multithread);
    DROP(g->device11_5);
    DROP(g->device11);
    if (g->fence_event)
        CloseHandle(g->fence_event);
    if (g->compiler_module)
        FreeLibrary(g->compiler_module);
    if (g->d3d12_module)
        FreeLibrary(g->d3d12_module);
    free(g->model_path);
    return true;
}

static void CALLBACK gpu_cleanup(PTP_CALLBACK_INSTANCE instance, void *opaque)
{
    struct dlssnr_gpu *g = opaque;
    if (!free_gpu(g)) {
        OutputDebugStringA("mpv dlssnr: retaining a faulted or in-flight GPU context\n");
        return;
    }
    HMODULE pin = g->module_pin;
    free(g);
    if (pin)
        FreeLibraryWhenCallbackReturns(instance, pin);
}

void dlssnr_gpu_destroy(struct dlssnr_gpu **gpu)
{
    struct dlssnr_gpu *g = *gpu;
    *gpu = NULL;
    if (!g)
        return;
    if (g->pending || dlssnr_runtime_poisoned(g->runtime)) {
        if (!TrySubmitThreadpoolCallback(gpu_cleanup, g, NULL))
            OutputDebugStringA("mpv dlssnr: retaining GPU context after cleanup scheduling failure\n");
        return;
    }
    if (free_gpu(g)) {
        HMODULE pin = g->module_pin;
        free(g);
        if (pin)
            FreeLibrary(pin);
    }
}

bool dlssnr_gpu_process(struct dlssnr_gpu **gpu,
                       const struct dlssnr_gpu_input *input,
                       const struct dlssnr_options *options, bool reset_history,
                       struct dlssnr_gpu_output *output,
                       struct dlssnr_gpu_info *info)
{
    *output = (struct dlssnr_gpu_output){0};
    ID3D11Device *device = NULL;
    ID3D11Texture2D_GetDevice(input->texture, &device);
    if (*gpu && ((*gpu)->device11 != device || ((*gpu)->failed && reset_history)))
        dlssnr_gpu_destroy(gpu);
    if (!*gpu) {
        *gpu = calloc(1, sizeof(**gpu));
        if (*gpu) {
            (*gpu)->info.status = DLSSNR_INITIALIZING;
            if (!create_device(*gpu, device)) {
                if ((*gpu)->info.status == DLSSNR_INITIALIZING ||
                    (*gpu)->info.status == DLSSNR_ACTIVE)
                    (*gpu)->info.status = DLSSNR_RUNTIME_FAILED;
                if (!(*gpu)->info.error[0])
                    snprintf((*gpu)->info.error, sizeof((*gpu)->info.error),
                             "Native GPU initialization failed");
                (*gpu)->failed = true;
            }
        }
    }
    DROP(device);
    struct dlssnr_gpu *g = *gpu;
    if (!g || g->failed) {
        if (input->lifetime && input->release_lifetime)
            input->release_lifetime(input->lifetime);
        if (g) {
            *info = g->info;
        } else {
            info->status = DLSSNR_RUNTIME_FAILED;
            snprintf(info->error, sizeof(info->error), "Out of memory creating GPU context");
        }
        return false;
    }
    g->info.status = DLSSNR_INITIALIZING;
    g->info.error[0] = 0;
    g->input_lifetime = input->lifetime;
    g->release_input = input->release_lifetime;
    struct output_slot *slot = NULL;
    uint64_t previous_feature = g->feature_serial;
    if (!configure(g, input, options))
        goto fail;
    slot = pool_acquire(g->pool);
    if (!slot) {
        g->info.status = DLSSNR_RUNTIME_FAILED;
        snprintf(g->info.error, sizeof(g->info.error),
                 "All bounded output slots are retained downstream or in GPU use");
        release_input(g);
        *info = g->info;
        return false;
    }
    LARGE_INTEGER start, finish, frequency;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    ID3D10Multithread_Enter(g->multithread);
    ID3D11DeviceContext4_CopySubresourceRegion(g->context11,
        (ID3D11Resource *)g->input_staging, 0, 0, 0, 0,
        (ID3D11Resource *)input->texture, input->subresource, NULL);
    g->pending_value = ++g->sequence;
    g->pending = true;
    HRESULT hr = ID3D11DeviceContext4_Signal(g->context11, g->fence11, g->pending_value);
    ID3D11DeviceContext4_Flush(g->context11);
    ID3D10Multithread_Leave(g->multithread);
    if (!hr_ok(&g->info, "D3D11 input-copy fence", hr))
        goto fail;
    hr = ID3D12CommandQueue_Wait(g->queue, g->fence12, g->pending_value);
    if (!hr_ok(&g->info, "D3D12 wait for decoder copy", hr) || !begin_commands(g))
        goto fail;
    struct dlssnr_shader_config settings = input->color;
    settings.width = g->width;
    settings.height = g->height;
    settings.proc_width = g->proc_width;
    settings.proc_height = g->proc_height;
    settings.texture_width = g->input_desc.Width;
    settings.texture_height = g->input_desc.Height;
    settings.input_yuv = g->input_desc.Format == DXGI_FORMAT_NV12 ||
                         g->input_desc.Format == DXGI_FORMAT_P010;
    settings.multiplier = options->residual_multiplier;
    settings.saturation = options->residual_saturation;
    settings.lightness = options->residual_lightness;
    settings.shadow = options->shadow_structure;
    settings.glow = options->reflection_glow;

    struct gpu_texture *t = g->textures;
    transition(g, &g->staging, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(g, &t[TEX_SOURCE], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    dispatch(g, 0, 0, 1, TEX_SOURCE, &settings, g->width, g->height);
    transition(g, &t[TEX_SOURCE], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    unsigned downsample_source = t[TEX_SOURCE].descriptor;
    if (g->proc_width != g->width) {
        transition(g, &t[TEX_HORIZONTAL], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        dispatch(g, 1, downsample_source, downsample_source,
                 TEX_HORIZONTAL, &settings, g->proc_width, g->height);
        transition(g, &t[TEX_HORIZONTAL], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        downsample_source = t[TEX_HORIZONTAL].descriptor;
    }
    transition(g, &t[TEX_INPUT], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    dispatch(g, 2, downsample_source, downsample_source, TEX_INPUT,
             &settings, g->proc_width, g->proc_height);
    transition(g, &t[TEX_INPUT], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(g, &t[TEX_ENHANCED], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!dlssnr_runtime_evaluate(g->runtime, g->commands,
        t[TEX_INPUT].resource, t[TEX_ENHANCED].resource,
        t[TEX_MOTION].resource, t[TEX_DEPTH].resource,
        options, reset_history || previous_feature != g->feature_serial, &g->info))
        goto fail;
    transition(g, &t[TEX_ENHANCED], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(g, &t[TEX_RESIDUAL], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    dispatch(g, 3, t[TEX_INPUT].descriptor, t[TEX_ENHANCED].descriptor,
             TEX_RESIDUAL, &settings, g->proc_width, g->proc_height);
    transition(g, &t[TEX_RESIDUAL], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(g, &t[TEX_COMPOSITE], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    dispatch(g, 4, t[TEX_SOURCE].descriptor, t[TEX_RESIDUAL].descriptor,
             TEX_COMPOSITE, &settings, g->width, g->height);
    transition(g, &g->staging, D3D12_RESOURCE_STATE_COMMON);
    for (unsigned n = 0; n < TEX_MOTION; n++)
        transition(g, &t[n], D3D12_RESOURCE_STATE_COMMON);
    if (!submit_commands(g))
        goto fail;
    ID3D10Multithread_Enter(g->multithread);
    hr = ID3D11DeviceContext4_Wait(g->context11, g->fence11, g->pending_value);
    if (SUCCEEDED(hr)) {
        ID3D11DeviceContext4_CopyResource(g->context11,
            (ID3D11Resource *)slot->texture, (ID3D11Resource *)g->shared_output);
        g->pending_value = ++g->sequence;
        hr = ID3D11DeviceContext4_Signal(g->context11, g->fence11, g->pending_value);
        ID3D11DeviceContext4_Flush(g->context11);
    }
    ID3D10Multithread_Leave(g->multithread);
    if (!hr_ok(&g->info, "D3D11 result-copy fence", hr) ||
        !wait_fence(g, g->pending_value, 30000))
        goto fail;
    QueryPerformanceCounter(&finish);
    g->info.wall_ms = (finish.QuadPart - start.QuadPart) * 1000.0 / frequency.QuadPart;
    g->info.status = DLSSNR_ACTIVE;
    g->info.error[0] = 0;
    *output = (struct dlssnr_gpu_output){
        .texture = slot->texture, .lease = slot, .release = release_output,
    };
    release_input(g);
    *info = g->info;
    return true;
fail:
    g->failed = true;
    if (g->info.status == DLSSNR_ACTIVE || g->info.status == DLSSNR_INITIALIZING)
        g->info.status = DLSSNR_RUNTIME_FAILED;
    if (!g->info.error[0])
        snprintf(g->info.error, sizeof(g->info.error), "Native GPU processing failed");
    if (slot)
        release_output(slot);
    if (g->pending)
        wait_fence(g, g->pending_value, 30000);
    if (!g->pending)
        release_input(g);
    *info = g->info;
    return false;
}
