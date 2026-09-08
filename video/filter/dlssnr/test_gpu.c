/*
 * This file is part of mpv.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef COBJMACROS
#define COBJMACROS
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_2.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "gpu.h"

#define WIDTH 512
#define HEIGHT 288

typedef HRESULT (WINAPI *factory_fn)(REFIID, void **);

static double half_value(uint16_t bits)
{
    unsigned exponent = (bits >> 10) & 31;
    unsigned mantissa = bits & 1023;
    double value = exponent == 0 ? ldexp((double)mantissa, -24) :
        exponent == 31 ? NAN : ldexp(1.0 + mantissa / 1024.0, (int)exponent - 15);
    return bits & 0x8000 ? -value : value;
}

static bool inspect_output(ID3D11Device *device, ID3D11DeviceContext *context,
                           ID3D11Texture2D *texture, const uint8_t *source,
                           double *mean, double *difference)
{
    D3D11_TEXTURE2D_DESC desc;
    ID3D11Texture2D_GetDesc(texture, &desc);
    if (desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT)
        return false;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ID3D11Texture2D *readback = NULL;
    if (FAILED(ID3D11Device_CreateTexture2D(device, &desc, NULL, &readback)))
        return false;
    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)readback,
                                     (ID3D11Resource *)texture);
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = ID3D11DeviceContext_Map(context, (ID3D11Resource *)readback, 0,
                                        D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        ID3D11Texture2D_Release(readback);
        return false;
    }
    double sum = 0, delta = 0;
    bool valid = true;
    for (unsigned y = 0; y < HEIGHT; y++) {
        const uint16_t *row = (const void *)((const uint8_t *)mapped.pData +
                                            y * mapped.RowPitch);
        for (unsigned x = 0; x < WIDTH; x++) {
            for (unsigned c = 0; c < 3; c++) {
                double value = half_value(row[x * 4 + c]);
                double original = source[(y * WIDTH + x) * 4 + (2 - c)] / 255.0;
                valid = valid && isfinite(value) && fabs(value) < 10;
                sum += value;
                delta += fabs(value - original);
            }
        }
    }
    *mean = sum / (WIDTH * HEIGHT * 3);
    *difference = delta / (WIDTH * HEIGHT * 3);
    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)readback, 0);
    ID3D11Texture2D_Release(readback);
    return valid && *mean > 0.02 && *mean < 1.5;
}

static bool test_yuv(ID3D11Device *device, ID3D11DeviceContext *context,
                     struct dlssnr_gpu **gpu, const char *model, int bits)
{
    unsigned pitch = WIDTH * (bits == 10 ? 2 : 1);
    size_t bytes = (size_t)pitch * HEIGHT * 3 / 2;
    uint8_t *unused = calloc(1, bytes);
    uint8_t *pixels = malloc(bytes);
    if (!unused || !pixels)
        return false;
    if (bits == 10) {
        uint16_t *words = (void *)pixels;
        for (unsigned y = 0; y < HEIGHT; y++)
            for (unsigned x = 0; x < WIDTH; x++)
                words[y * WIDTH + x] = (uint16_t)((64 + x) << 6);
        for (size_t n = WIDTH * HEIGHT; n < bytes / 2; n++)
            words[n] = 512 << 6;
    } else {
        for (unsigned y = 0; y < HEIGHT; y++)
            for (unsigned x = 0; x < WIDTH; x++)
                pixels[y * WIDTH + x] = (uint8_t)(16 + x % 220);
        memset(pixels + WIDTH * HEIGHT, 128, WIDTH * HEIGHT / 2);
    }
    D3D11_TEXTURE2D_DESC desc = {
        .Width = WIDTH, .Height = HEIGHT, .MipLevels = 1, .ArraySize = 2,
        .Format = bits == 10 ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12,
        .SampleDesc.Count = 1, .Usage = D3D11_USAGE_DEFAULT,
    };
    D3D11_SUBRESOURCE_DATA data[2] = {
        {.pSysMem = unused, .SysMemPitch = pitch},
        {.pSysMem = pixels, .SysMemPitch = pitch},
    };
    ID3D11Texture2D *source = NULL;
    HRESULT hr = ID3D11Device_CreateTexture2D(device, &desc, data, &source);
    free(unused);
    free(pixels);
    if (FAILED(hr)) {
        fprintf(stderr, "YUV%d array creation failed: 0x%08lx\n", bits, (unsigned long)hr);
        return false;
    }
    struct dlssnr_options options = dlssnr_defaults;
    options.model_path = (char *)model;
    options.input_resolution = 25;
    options.intensity = 0;
    options.local_tone = 0;
    options.local_structure = 0;
    options.skin_structure = 0;
    options.auto_mask = false;
    options.ui_correction = false;
    struct dlssnr_gpu_input input = {
        .texture = source, .subresource = 1,
        .color = {.width = WIDTH, .height = HEIGHT,
                  .luma = {0.2126f, 0.7152f, 0.0722f}},
    };
    dlssnr_yuv_matrix(bits, false, 0.2126, 0.0722, input.color.matrix);
    struct dlssnr_gpu_info info = {0};
    struct dlssnr_gpu_output output = {0};
    bool ok = dlssnr_gpu_process(gpu, &input, &options, true, &output, &info);
    ID3D11Texture2D_Release(source);
    if (!ok) {
        fprintf(stderr, "YUV%d evaluation failed: %s\n", bits, info.error);
        return false;
    }
    ID3D11Texture2D_GetDesc(output.texture, &desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ID3D11Texture2D *readback = NULL;
    hr = ID3D11Device_CreateTexture2D(device, &desc, NULL, &readback);
    double largest = 0;
    if (SUCCEEDED(hr)) {
        ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)readback,
                                         (ID3D11Resource *)output.texture);
        D3D11_MAPPED_SUBRESOURCE mapped;
        hr = ID3D11DeviceContext_Map(context, (ID3D11Resource *)readback, 0,
                                    D3D11_MAP_READ, 0, &mapped);
        if (SUCCEEDED(hr)) {
            for (unsigned y = 0; y < HEIGHT; y++) {
                const uint16_t *row = (const void *)((const uint8_t *)mapped.pData +
                                                    y * mapped.RowPitch);
                for (unsigned x = 0; x < WIDTH; x++) {
                    double expected = bits == 10 ? x / 876.0 : (x % 220) / 219.0;
                    for (unsigned c = 0; c < 3; c++) {
                        double error = fabs(half_value(row[x * 4 + c]) - expected);
                        if (!isfinite(error))
                            ok = false;
                        if (error > largest)
                            largest = error;
                    }
                }
            }
            ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)readback, 0);
        }
    }
    if (readback)
        ID3D11Texture2D_Release(readback);
    output.release(output.lease);
    ok = ok && SUCCEEDED(hr) && largest < 0.0006;
    printf("YUV%d array-slice1 quarter-resolution intensity0 "
           "max_rgb_error=%.9f preserved_source_precision=%s\n",
           bits, largest, ok ? "yes" : "NO");
    return ok;
}

struct release_gate {
    ID3D12Fence *fence;
    HANDLE requested;
};

static DWORD WINAPI open_release_gate(void *opaque)
{
    struct release_gate *gate = opaque;
    WaitForSingleObject(gate->requested, 5000);
    return FAILED(ID3D12Fence_Signal(gate->fence, 1));
}

static bool test_release_order(ID3D11Device *device, ID3D11DeviceContext *context,
                               struct dlssnr_gpu **gpu,
                               const struct dlssnr_gpu_input *input,
                               const struct dlssnr_options *options,
                               struct dlssnr_gpu_output held[4],
                               const double means[4], const uint8_t *pixels)
{
    typedef HRESULT (WINAPI *create12_fn)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
    HMODULE library = LoadLibraryExW(L"d3d12.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    ID3D11Device5 *device5 = NULL;
    ID3D11DeviceContext4 *context4 = NULL;
    IDXGIDevice *dxgi = NULL;
    IDXGIAdapter *adapter = NULL;
    ID3D12Device *device12 = NULL;
    ID3D11Fence *gate11 = NULL;
    ID3D12Fence *gate12 = NULL;
    ID3D11Texture2D *copies[2] = {0};
    HANDLE shared = NULL, thread = NULL;
    struct release_gate gate = {0};
    bool valid = false;
    if (!library)
        goto done;
    FARPROC proc = GetProcAddress(library, "D3D12CreateDevice");
    create12_fn create = NULL;
    memcpy(&create, &proc, sizeof(create));
    if (!create)
        goto done;
#define GATE_HR(expression) do {                                     \
    HRESULT result_ = (expression);                                 \
    if (FAILED(result_)) {                                          \
        fprintf(stderr, "consumer gate: 0x%08lx\n",                  \
                (unsigned long)result_);                            \
        goto done;                                                 \
    }                                                              \
} while (0)
    GATE_HR(ID3D11Device_QueryInterface(device, &IID_ID3D11Device5, (void **)&device5));
    GATE_HR(ID3D11DeviceContext_QueryInterface(context, &IID_ID3D11DeviceContext4,
                                               (void **)&context4));
    GATE_HR(ID3D11Device_QueryInterface(device, &IID_IDXGIDevice, (void **)&dxgi));
    GATE_HR(IDXGIDevice_GetAdapter(dxgi, &adapter));
    GATE_HR(create((IUnknown *)adapter, D3D_FEATURE_LEVEL_12_0,
                   &IID_ID3D12Device, (void **)&device12));
    GATE_HR(ID3D11Device5_CreateFence(device5, 0, D3D11_FENCE_FLAG_SHARED,
                                     &IID_ID3D11Fence, (void **)&gate11));
    GATE_HR(ID3D11Fence_CreateSharedHandle(gate11, NULL, GENERIC_ALL, NULL, &shared));
    GATE_HR(ID3D12Device_OpenSharedHandle(device12, shared,
                                          &IID_ID3D12Fence, (void **)&gate12));
    D3D11_TEXTURE2D_DESC desc;
    ID3D11Texture2D_GetDesc(held[0].texture, &desc);
    desc.BindFlags = 0;
    for (unsigned n = 0; n < 2; n++)
        GATE_HR(ID3D11Device_CreateTexture2D(device, &desc, NULL, &copies[n]));
    gate.fence = gate12;
    gate.requested = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!gate.requested)
        goto done;
    thread = CreateThread(NULL, 0, open_release_gate, &gate, 0, NULL);
    if (!thread)
        goto done;

    GATE_HR(ID3D11DeviceContext4_Wait(context4, gate11, 1));
    const unsigned order[] = {3, 1};
    for (unsigned n = 0; n < 2; n++) {
        unsigned slot = order[n];
        ID3D11DeviceContext4_CopyResource(context4, (ID3D11Resource *)copies[n],
                                         (ID3D11Resource *)held[slot].texture);
        held[slot].release(held[slot].lease);
        held[slot].lease = NULL;
    }
    ID3D11DeviceContext4_Flush(context4);
    struct dlssnr_gpu_info info = {0};
    struct dlssnr_gpu_output extra = {0};
    bool acquired = dlssnr_gpu_process(gpu, input, options, false, &extra, &info);
    SetEvent(gate.requested);
    WaitForSingleObject(thread, INFINITE);
    if (acquired)
        extra.release(extra.lease);
    printf("released-refs-with-blocked-GPU-consumers-refuse-reuse=%s\n",
           acquired ? "NO" : "yes");
    if (acquired)
        goto done;
    for (unsigned n = 0; n < 2; n++) {
        double mean = 0, delta = 0;
        bool unchanged = inspect_output(device, context, copies[n], pixels,
                                         &mean, &delta) &&
                         fabs(mean - means[order[n]]) < 1e-12;
        printf("out-of-order-consumer-read-slot-%u-unchanged=%s\n",
               order[n], unchanged ? "yes" : "NO");
        if (!unchanged)
            goto done;
    }
    for (unsigned attempt = 0; attempt < 100; attempt++) {
        if (dlssnr_gpu_process(gpu, input, options, false, &extra, &info)) {
            valid = extra.texture == held[3].texture || extra.texture == held[1].texture;
            extra.release(extra.lease);
            break;
        }
        Sleep(1);
    }
    printf("reuse-after-consumer-GPU-completion=%s\n", valid ? "yes" : "NO");
#undef GATE_HR
done:
    if (thread) {
        SetEvent(gate.requested);
        WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread);
    }
    if (gate.requested)
        CloseHandle(gate.requested);
    if (shared)
        CloseHandle(shared);
    for (unsigned n = 0; n < 2; n++) {
        if (copies[n])
            ID3D11Texture2D_Release(copies[n]);
    }
    if (gate12)
        ID3D12Fence_Release(gate12);
    if (gate11)
        ID3D11Fence_Release(gate11);
    if (device12)
        ID3D12Device_Release(device12);
    if (adapter)
        IDXGIAdapter_Release(adapter);
    if (dxgi)
        IDXGIDevice_Release(dxgi);
    if (context4)
        ID3D11DeviceContext4_Release(context4);
    if (device5)
        ID3D11Device5_Release(device5);
    if (library)
        FreeLibrary(library);
    return valid;
}

static bool test_ownership(ID3D11Device *device, ID3D11DeviceContext *context,
                           struct dlssnr_gpu **gpu, ID3D11Texture2D *source,
                           uint8_t *pixels, const char *model)
{
    struct dlssnr_options options = dlssnr_defaults;
    options.model_path = (char *)model;
    options.input_resolution = 50;
    options.auto_mask = false;
    options.ui_correction = false;
    struct dlssnr_gpu_input input = {
        .texture = source,
        .color = {.width = WIDTH, .height = HEIGHT,
                  .luma = {0.2126f, 0.7152f, 0.0722f}},
    };
    struct dlssnr_gpu_output held[4] = {0};
    struct dlssnr_gpu_info info = {0};
    double means[4] = {0};
    bool valid = true;
    for (unsigned n = 0; n < 4; n++) {
        for (size_t pixel = 0; pixel < WIDTH * HEIGHT; pixel++)
            pixels[pixel * 4 + 1] = (uint8_t)(60 + n * 35);
        ID3D11DeviceContext_UpdateSubresource(context, (ID3D11Resource *)source,
                                               0, NULL, pixels, WIDTH * 4, 0);
        if (!dlssnr_gpu_process(gpu, &input, &options, n == 0, &held[n], &info)) {
            fprintf(stderr, "ownership frame %u: %s\n", n, info.error);
            valid = false;
            break;
        }
        double delta = 0;
        valid = inspect_output(device, context, held[n].texture, pixels, &means[n], &delta);
        if (!valid)
            break;
    }
    if (valid) {
        struct dlssnr_gpu_output extra = {0};
        bool acquired = dlssnr_gpu_process(gpu, &input, &options, false, &extra, &info);
        if (acquired) {
            extra.release(extra.lease);
            valid = false;
        }
        printf("bounded-pool-refuses-fifth-live-output=%s\n", acquired ? "NO" : "yes");
    }
    if (valid)
        valid = test_release_order(device, context, gpu, &input, &options,
                                    held, means, pixels);
    if (valid) {
        input.color.width = WIDTH - 16;
        input.color.height = HEIGHT - 16;
        struct dlssnr_gpu_output resized = {0};
        valid = dlssnr_gpu_process(gpu, &input, &options, true, &resized, &info);
        if (valid) {
            D3D11_TEXTURE2D_DESC desc;
            ID3D11Texture2D_GetDesc(resized.texture, &desc);
            valid = desc.Width == WIDTH - 16 && desc.Height == HEIGHT - 16;
            resized.release(resized.lease);
        }
        printf("size-and-history-reset-with-old-output-references=%s\n",
               valid ? "yes" : "NO");
    }
    dlssnr_gpu_destroy(gpu);
    for (unsigned n = 0; n < 4; n++) {
        if (!held[n].lease)
            continue;
        double mean = 0, delta = 0;
        bool unchanged = inspect_output(device, context, held[n].texture, pixels,
                                         &mean, &delta) && fabs(mean - means[n]) < 1e-12;
        printf("retained-output-%u-survives-later-frames-and-filter-destruction=%s\n",
               n, unchanged ? "yes" : "NO");
        valid = valid && unchanged;
        held[n].release(held[n].lease);
    }
    Sleep(100);
    return valid;
}

static bool test_nv12_1080p(ID3D11Device *device, ID3D11DeviceContext *context,
                            const char *model)
{
    enum { width = 1920, height = 1080, padded_height = 1088 };
    size_t bytes = (size_t)width * padded_height * 3 / 2;
    uint8_t *pixels = malloc(bytes);
    uint8_t *unused = calloc(1, bytes);
    if (!pixels || !unused) {
        free(pixels);
        free(unused);
        return false;
    }
    for (unsigned y = 0; y < padded_height; y++) {
        unsigned visible_y = y < height ? y : height - 1;
        for (unsigned x = 0; x < width; x++)
            pixels[y * width + x] =
                (uint8_t)(36 + (x * 127 / width) +
                          ((x * 17 + visible_y * 13) % 43));
    }
    memset(pixels + width * padded_height, 128, width * padded_height / 2);
    D3D11_TEXTURE2D_DESC desc = {
        .Width = width, .Height = padded_height, .MipLevels = 1, .ArraySize = 2,
        .Format = DXGI_FORMAT_NV12, .SampleDesc.Count = 1,
        .Usage = D3D11_USAGE_DEFAULT,
    };
    D3D11_SUBRESOURCE_DATA initial[2] = {
        {.pSysMem = unused, .SysMemPitch = width},
        {.pSysMem = pixels, .SysMemPitch = width},
    };
    ID3D11Texture2D *source = NULL;
    HRESULT hr = ID3D11Device_CreateTexture2D(device, &desc, initial, &source);
    free(unused);
    if (FAILED(hr)) {
        free(pixels);
        return false;
    }
    struct dlssnr_options options = dlssnr_defaults;
    options.model_path = (char *)model;
    struct dlssnr_gpu *gpu = NULL;
    struct dlssnr_gpu_info info = {0};
    struct dlssnr_gpu_input input = {
        .texture = source, .subresource = 1,
        .color = {
            .width = width, .height = height, .chroma_x = 0.5f,
            .luma = {0.2126f, 0.7152f, 0.0722f},
        },
    };
    dlssnr_yuv_matrix(8, false, 0.2126, 0.0722, input.color.matrix);
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    bool valid = true;
    for (unsigned n = 0; n < 3 && valid; n++) {
        struct dlssnr_gpu_output output = {0};
        LARGE_INTEGER start, end;
        QueryPerformanceCounter(&start);
        bool evaluated = dlssnr_gpu_process(&gpu, &input, &options, n == 0,
                                            &output, &info);
        QueryPerformanceCounter(&end);
        double context_ms = (end.QuadPart - start.QuadPart) * 1000.0 /
                            frequency.QuadPart;
        printf("nv12-1080p-frame=%u real-evaluation=%s context-wall-ms=%.3f "
               "pipeline-wall-ms=%.3f proc=%dx%d error=%s\n",
               n, evaluated ? "yes" : "NO", context_ms, info.wall_ms,
               info.proc_width, info.proc_height, info.error);
        if (!evaluated) {
            valid = false;
            break;
        }
        ID3D11Texture2D_GetDesc(output.texture, &desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        ID3D11Texture2D *readback = NULL;
        hr = ID3D11Device_CreateTexture2D(device, &desc, NULL, &readback);
        double sum = 0, delta = 0;
        if (SUCCEEDED(hr)) {
            ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)readback,
                                             (ID3D11Resource *)output.texture);
            D3D11_MAPPED_SUBRESOURCE mapped;
            hr = ID3D11DeviceContext_Map(context, (ID3D11Resource *)readback, 0,
                                        D3D11_MAP_READ, 0, &mapped);
            if (SUCCEEDED(hr)) {
                for (unsigned y = 0; y < height; y++) {
                    const uint16_t *row = (const void *)(
                        (const uint8_t *)mapped.pData + y * mapped.RowPitch);
                    for (unsigned x = 0; x < width; x++) {
                        double baseline = (pixels[y * width + x] - 16) / 219.0;
                        for (unsigned c = 0; c < 3; c++) {
                            double value = half_value(row[x * 4 + c]);
                            valid = valid && isfinite(value) && fabs(value) < 10;
                            sum += value;
                            delta += fabs(value - baseline);
                        }
                    }
                }
                ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)readback, 0);
            }
        }
        if (readback)
            ID3D11Texture2D_Release(readback);
        output.release(output.lease);
        double mean = sum / (width * height * 3);
        valid = valid && SUCCEEDED(hr) && mean > 0.02 && mean < 1.5 &&
                info.status == DLSSNR_ACTIVE && info.runtime_loads == 1 &&
                info.feature_builds == 1;
        printf("nv12-1080p-finite-nonblank=%s mean=%.9f "
               "mean-absolute-change=%.9f\n",
               valid ? "yes" : "NO", mean, delta / (width * height * 3));
    }
    dlssnr_gpu_destroy(&gpu);
    ID3D11Texture2D_Release(source);
    free(pixels);
    Sleep(100);
    return valid;
}

int wmain(int argc, wchar_t **argv)
{
    bool nv12_acceptance = argc == 3 && !wcscmp(argv[2], L"--nv12-1080p");
    if (argc != 2 && !nv12_acceptance) {
        fprintf(stderr, "usage: test_gpu ABSOLUTE_PRIVATE_MODEL_PATH [--nv12-1080p]\n");
        return 2;
    }
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    setvbuf(stdout, NULL, _IONBF, 0);
    HMODULE dxgi = LoadLibraryExW(L"dxgi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE d3d11 = LoadLibraryExW(L"d3d11.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dxgi || !d3d11)
        return 1;
    FARPROC proc = GetProcAddress(dxgi, "CreateDXGIFactory1");
    factory_fn factory_create = NULL;
    memcpy(&factory_create, &proc, sizeof(factory_create));
    PFN_D3D11_CREATE_DEVICE device_create = NULL;
    proc = GetProcAddress(d3d11, "D3D11CreateDevice");
    memcpy(&device_create, &proc, sizeof(device_create));
    if (!factory_create || !device_create)
        return 1;
    IDXGIFactory1 *factory = NULL;
    IDXGIAdapter1 *adapter = NULL;
    if (FAILED(factory_create(&IID_IDXGIFactory1, (void **)&factory)))
        return 1;
    for (unsigned n = 0; ; n++) {
        HRESULT hr = IDXGIFactory1_EnumAdapters1(factory, n, &adapter);
        if (FAILED(hr))
            break;
        DXGI_ADAPTER_DESC1 desc;
        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(adapter, &desc)) &&
            desc.VendorId == 0x10de && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            break;
        IDXGIAdapter1_Release(adapter);
        adapter = NULL;
    }
    if (!adapter)
        return 1;
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_1;
    HRESULT hr = device_create((IDXGIAdapter *)adapter, D3D_DRIVER_TYPE_UNKNOWN,
        NULL, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, &level, 1, D3D11_SDK_VERSION,
        &device, NULL, &context);
    IDXGIAdapter1_Release(adapter);
    IDXGIFactory1_Release(factory);
    if (FAILED(hr)) {
        fprintf(stderr, "D3D11CreateDevice=0x%08lx\n", (unsigned long)hr);
        return 1;
    }
    uint8_t *pixels = malloc(WIDTH * HEIGHT * 4);
    int bytes = WideCharToMultiByte(CP_UTF8, 0, argv[1], -1, NULL, 0, NULL, NULL);
    char *model = malloc(bytes > 0 ? (size_t)bytes : 1);
    if (!pixels || !model || !bytes)
        return 1;
    WideCharToMultiByte(CP_UTF8, 0, argv[1], -1, model, bytes, NULL, NULL);
    if (nv12_acceptance) {
        int result = test_nv12_1080p(device, context, model) ? 0 : 9;
        free(pixels);
        free(model);
        ID3D11DeviceContext_Release(context);
        ID3D11Device_Release(device);
        FreeLibrary(d3d11);
        FreeLibrary(dxgi);
        printf("nv12-1080p-test-result=%d; synthetic fixture, diagnostic-only CPU transfers\n",
               result);
        return result;
    }

    for (unsigned y = 0; y < HEIGHT; y++) {
        for (unsigned x = 0; x < WIDTH; x++) {
            uint8_t *p = pixels + (y * WIDTH + x) * 4;
            p[0] = 40 + (x * 149 / WIDTH);
            p[1] = 35 + (y * 153 / HEIGHT);
            p[2] = 45 + ((x / 16 + y / 16) & 1) * 110 + (x * 13 + y * 7) % 35;
            p[3] = 255;
        }
    }
    D3D11_TEXTURE2D_DESC desc = {
        .Width = WIDTH, .Height = HEIGHT, .MipLevels = 1, .ArraySize = 1,
        .Format = DXGI_FORMAT_B8G8R8A8_UNORM, .SampleDesc.Count = 1,
        .Usage = D3D11_USAGE_DEFAULT,
    };
    D3D11_SUBRESOURCE_DATA initial = {.pSysMem = pixels, .SysMemPitch = WIDTH * 4};
    ID3D11Texture2D *source = NULL;
    hr = ID3D11Device_CreateTexture2D(device, &desc, &initial, &source);
    if (FAILED(hr))
        return 1;
    struct dlssnr_options options = dlssnr_defaults;
    options.model_path = model;
    options.input_resolution = 50;
    options.auto_mask = false;
    options.ui_correction = false;
    struct dlssnr_gpu *gpu = NULL;
    struct dlssnr_gpu_info info = {0};
    struct dlssnr_gpu_input input = {
        .texture = source,
        .color = {.width = WIDTH, .height = HEIGHT,
                  .luma = {0.2126f, 0.7152f, 0.0722f}},
    };
    int result = 0;
    for (unsigned frame = 0; frame < 3; frame++) {
        if (frame == 1) {
            options.style = 2;
            options.intensity = 0.6f;
        }
        if (frame == 2) {
            options.input_resolution = 100;
            options.preset = 1;
        }
        struct dlssnr_gpu_output output = {0};
        bool ok = dlssnr_gpu_process(&gpu, &input, &options, frame == 0,
                                     &output, &info);
        printf("frame=%u success=%s status=%d proc=%dx%d wall_ms=%.3f "
               "caller_compatibility=%s gpu=%s error=%s\n",
               frame, ok ? "yes" : "no", info.status, info.proc_width,
               info.proc_height, info.wall_ms,
               info.caller_compatibility ? "yes" : "no", info.gpu_name, info.error);
        printf("runtime-loads=%llu feature-builds=%llu\n",
               (unsigned long long)info.runtime_loads,
               (unsigned long long)info.feature_builds);
        if (!ok) {
            result = 3;
            break;
        }
        if (info.runtime_loads != 1 || info.feature_builds != (frame == 2 ? 2u : 1u)) {
            output.release(output.lease);
            fprintf(stderr, "Unnecessary model/feature rebuild detected\n");
            result = 8;
            break;
        }
        double mean = 0, difference = 0;
        bool valid = inspect_output(device, context, output.texture, pixels,
                                     &mean, &difference);
        output.release(output.lease);
        printf("output_finite_and_nonblank=%s mean=%.9f mean_absolute_change=%.9f\n",
               valid ? "yes" : "NO", mean, difference);
        if (!valid || difference < 0.00001) {
            result = 4;
            break;
        }
    }
    if (!result && !test_yuv(device, context, &gpu, model, 8))
        result = 5;
    if (!result && !test_yuv(device, context, &gpu, model, 10))
        result = 6;
    if (!result && !test_ownership(device, context, &gpu, source, pixels, model))
        result = 7;
    dlssnr_gpu_destroy(&gpu);
    ID3D11Texture2D_Release(source);
    ID3D11DeviceContext_Release(context);
    ID3D11Device_Release(device);
    free(model);
    free(pixels);
    FreeLibrary(d3d11);
    FreeLibrary(dxgi);
    printf("test-result=%d; CPU pixel readback was diagnostic-only\n", result);
    return result;
}

#ifdef DLSSNR_TEST_DLL
__declspec(dllexport) int __cdecl dlssnr_gpu_test(const wchar_t *model)
{
    static const unsigned char marker = 0;
    HMODULE module = NULL;
    bool in_dll = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                     GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                     (const wchar_t *)&marker, &module) &&
                  module != GetModuleHandleW(NULL);
    printf("GPU-backend-under-test-resides-in-DLL=%s\n", in_dll ? "yes" : "NO");
    if (!in_dll)
        return 1;
    wchar_t name[] = L"dlssnr-dll-test";
    wchar_t *arguments[] = {name, (wchar_t *)model};
    return wmain(2, arguments);
}
#endif
