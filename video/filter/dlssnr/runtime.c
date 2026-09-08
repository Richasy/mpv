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
#include <d3d12.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "ngx_abi.h"
#include "bridge.h"
#include "model_identity.h"
#include "runtime.h"

#if !defined(_MSC_VER) && !defined(__clang__)
#error DLSSNR requires a Windows C compiler with structured exception support.
#endif

struct dlssnr_runtime {
    HMODULE core, model, bridge_module, module_pin, caller_module;
    const struct mpv_ngx_bridge_api *bridge;
    HANDLE model_file;
    ID3D12Device *device;
    struct ngx_parameters *parameters;
    struct ngx_handle *feature;
    ngx_destroy_parameters_fn destroy_parameters;
    ngx_shutdown_fn core_shutdown;
    ngx_create_fn create;
    ngx_evaluate_fn evaluate;
    ngx_release_fn release;
    ngx_model_shutdown_fn shutdown;
    bool core_initialized, model_initialized, poisoned;
    int width, height;
};

static const unsigned char module_anchor = 0;
static LONG runtime_owner;
static LONG runtime_fault;
static SRWLOCK log_lock = SRWLOCK_INIT;
static char sdk_message[DLSSNR_ERROR_SIZE];

static void clean_message(char *dest, size_t size, const char *source)
{
    if (!size)
        return;
    size_t n = 0;
    for (; source && source[n] && n + 1 < size; n++) {
        unsigned char c = (unsigned char)source[n];
        dest[n] = c >= 32 && c != 127 ? (char)c : ' ';
    }
    dest[n] = 0;
}

static void __cdecl runtime_log(const char *message, int level, int feature)
{
    (void)level;
    (void)feature;
    AcquireSRWLockExclusive(&log_lock);
    clean_message(sdk_message, sizeof(sdk_message), message);
    ReleaseSRWLockExclusive(&log_lock);
}

static bool sdk_result(struct dlssnr_gpu_info *info, const char *operation,
                       ngx_result result)
{
    if (!ngx_failed(result))
        return true;
    info->status = DLSSNR_RUNTIME_FAILED;
    AcquireSRWLockShared(&log_lock);
    snprintf(info->error, sizeof(info->error),
             "%s returned 0x%08" PRIx32 ": %.360s",
             operation, result, sdk_message);
    ReleaseSRWLockShared(&log_lock);
    return false;
}

static void sdk_exception(struct dlssnr_runtime *r,
                          struct dlssnr_gpu_info *info, DWORD code)
{
    r->poisoned = true;
    InterlockedExchange(&runtime_fault, 1);
    info->status = DLSSNR_RUNTIME_FAILED;
    snprintf(info->error, sizeof(info->error),
             "NGX native exception 0x%08lx; runtime retained; restart required",
             (unsigned long)code);
}

static bool bind_function(HMODULE module, const char *name, void *target,
                          size_t size, struct dlssnr_gpu_info *info)
{
    FARPROC proc = GetProcAddress(module, name);
    if (size != sizeof(proc) || !proc) {
        info->status = DLSSNR_RUNTIME_MISSING;
        snprintf(info->error, sizeof(info->error), "Missing runtime export: %s", name);
        return false;
    }
    memcpy(target, &proc, size);
    return true;
}

#define BIND(module, member, name) \
    bind_function(module, name, &(member), sizeof(member), info)

bool dlssnr_resolve_model_path(const char *configured, wchar_t *path,
                              size_t count, char error[DLSSNR_ERROR_SIZE])
{
    if (!count || count > 32768)
        return false;
    enum dlssnr_model_path_type type = dlssnr_model_path_type(configured);
    if (type == DLSSNR_MODEL_INVALID) {
        snprintf(error, DLSSNR_ERROR_SIZE, "Invalid absolute or module-relative model-path");
        return false;
    }
    if (type == DLSSNR_MODEL_ABSOLUTE) {
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, configured, -1,
                                  path, (int)count)) {
            snprintf(error, DLSSNR_ERROR_SIZE, "Invalid UTF-8 or long model-path");
            return false;
        }
        for (wchar_t *p = path; *p; p++) {
            if (*p == L'/')
                *p = L'\\';
        }
        return true;
    }
    HMODULE module = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (const wchar_t *)&module_anchor, &module)) {
        snprintf(error, DLSSNR_ERROR_SIZE, "Cannot locate the libmpv module");
        return false;
    }
    DWORD length = GetModuleFileNameW(module, path, (DWORD)count);
    if (!length || length >= count) {
        snprintf(error, DLSSNR_ERROR_SIZE, "Cannot resolve libmpv module path");
        return false;
    }
    wchar_t *separator = wcsrchr(path, L'\\');
    if (!separator) {
        snprintf(error, DLSSNR_ERROR_SIZE, "Cannot resolve the libmpv module directory");
        return false;
    }
    if (type == DLSSNR_MODEL_RELATIVE) {
        size_t offset = (size_t)(separator + 1 - path);
        if (offset >= count ||
            !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, configured, -1,
                                 path + offset, (int)(count - offset))) {
            snprintf(error, DLSSNR_ERROR_SIZE,
                     "Invalid UTF-8 or overly long module-relative model-path");
            return false;
        }
        for (wchar_t *p = path + offset; *p; p++) {
            if (*p == L'/')
                *p = L'\\';
        }
        return true;
    }
    static const wchar_t suffix[] = L"\\ngx\\nvngx_dlssnr.dll";
    if ((size_t)(separator - path) +
                       sizeof(suffix) / sizeof(suffix[0]) > count) {
        snprintf(error, DLSSNR_ERROR_SIZE, "The default model path is too long");
        return false;
    }
    wmemcpy(separator, suffix, sizeof(suffix) / sizeof(suffix[0]));
    return true;
}

static bool registry_string(const wchar_t *key, const wchar_t *name,
                            wchar_t *value, size_t count)
{
    typedef LSTATUS (WINAPI *reg_get_fn)(HKEY, LPCWSTR, LPCWSTR, DWORD,
                                       LPDWORD, PVOID, LPDWORD);
    HMODULE module = LoadLibraryExW(L"advapi32.dll", NULL,
                                    LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module)
        return false;
    FARPROC proc = GetProcAddress(module, "RegGetValueW");
    reg_get_fn get = NULL;
    memcpy(&get, &proc, sizeof(get));
    DWORD bytes = (DWORD)(count * sizeof(wchar_t));
    LSTATUS result = get ? get(HKEY_LOCAL_MACHINE, key, name,
        RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
        NULL, value, &bytes) : ERROR_PROC_NOT_FOUND;
    FreeLibrary(module);
    return result == ERROR_SUCCESS && count && value[0] &&
           wmemchr(value, 0, count);
}

static HMODULE load_core(struct dlssnr_gpu_info *info)
{
    wchar_t path[4096], expanded[4096];
    bool have_path = registry_string(
        L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", L"FullPath",
        path, sizeof(path) / sizeof(path[0]));
    if (have_path) {
        size_t length = wcslen(path);
        const wchar_t *name = wcsrchr(path, L'\\');
        if (!name || _wcsicmp(name + 1, L"_nvngx.dll")) {
            static const wchar_t suffix[] = L"\\_nvngx.dll";
            if (length + sizeof(suffix) / sizeof(suffix[0]) < 4096)
                wmemcpy(path + length, suffix, sizeof(suffix) / sizeof(suffix[0]));
            else
                have_path = false;
        }
    }
    if (!have_path) {
        have_path = registry_string(
            L"SYSTEM\\CurrentControlSet\\Services\\nvlddmkm", L"ImagePath",
            path, sizeof(path) / sizeof(path[0]));
        if (have_path && !_wcsnicmp(path, L"\\SystemRoot\\", 12)) {
            DWORD n = GetWindowsDirectoryW(expanded, 4096);
            if (!n || n >= 4096 || n + wcslen(path + 11) >= 4096)
                have_path = false;
            else {
                wmemcpy(expanded + n, path + 11, wcslen(path + 11) + 1);
                wmemcpy(path, expanded, wcslen(expanded) + 1);
            }
        }
        if (have_path) {
            wchar_t *slash = wcsrchr(path, L'\\');
            if (!slash || (size_t)(slash - path) + 12 >= 4096)
                have_path = false;
            else
                wmemcpy(slash + 1, L"_nvngx.dll", 11);
        }
    }
    if (have_path) {
        DWORD n = ExpandEnvironmentStringsW(path, expanded, 4096);
        if (!n || n > 4096)
            have_path = false;
        else
            wmemcpy(path, expanded, n);
    }
    const wchar_t *absolute = path;
    if (have_path && !wcsncmp(absolute, L"\\??\\", 4))
        absolute += 4;
    if (!have_path || !((absolute[0] && absolute[1] == L':' &&
                         absolute[2] == L'\\') ||
                        (absolute[0] == L'\\' && absolute[1] == L'\\'))) {
        info->status = DLSSNR_RUNTIME_MISSING;
        snprintf(info->error, sizeof(info->error),
                 "Cannot locate the installed NVIDIA NGX core");
        return NULL;
    }
    HMODULE module = LoadLibraryExW(absolute, NULL,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) {
        info->status = DLSSNR_RUNTIME_MISSING;
        snprintf(info->error, sizeof(info->error),
                 "Cannot load installed NGX core (Windows error %lu)", GetLastError());
    }
    return module;
}

static bool load_bridge(struct dlssnr_runtime *r, struct dlssnr_gpu_info *info)
{
    wchar_t path[32768];
    DWORD length = GetModuleFileNameW(r->caller_module, path, 32768);
    wchar_t *last = length && length < 32768 ? wcsrchr(path, L'\\') : NULL;
    static const wchar_t filename[] = MPV_NGX_BRIDGE_FILENAME;
    if (!last || (size_t)(last + 1 - path) +
        sizeof(filename) / sizeof(filename[0]) > 32768) {
        info->status = DLSSNR_RUNTIME_MISSING;
        snprintf(info->error, sizeof(info->error), "Cannot resolve mpv-nvngx.dll beside libmpv");
        return false;
    }
    wmemcpy(last + 1, filename, sizeof(filename) / sizeof(filename[0]));
    r->bridge_module = LoadLibraryExW(path, NULL,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!r->bridge_module) {
        info->status = DLSSNR_RUNTIME_MISSING;
        snprintf(info->error, sizeof(info->error),
                 "Original mpv-nvngx.dll bridge missing beside libmpv (Windows error %lu)",
                 GetLastError());
        return false;
    }
    mpv_ngx_bridge_get_api_fn get_api = NULL;
    if (!BIND(r->bridge_module, get_api, MPV_NGX_BRIDGE_ENTRYPOINT))
        return false;
    r->bridge = get_api(MPV_NGX_BRIDGE_ABI_VERSION);
    if (!r->bridge || r->bridge->abi_version != MPV_NGX_BRIDGE_ABI_VERSION ||
        r->bridge->struct_size < sizeof(*r->bridge) || !r->bridge->init ||
        !r->bridge->create || !r->bridge->evaluate || !r->bridge->release ||
        !r->bridge->shutdown) {
        info->status = DLSSNR_RUNTIME_MISSING;
        snprintf(info->error, sizeof(info->error), "Incompatible mpv-nvngx.dll bridge ABI");
        return false;
    }
    info->caller_compatibility = true;
    return true;
}

struct dlssnr_runtime *dlssnr_runtime_open(
    ID3D12Device *device, const wchar_t *path, struct dlssnr_gpu_info *info)
{
    info->caller_compatibility = false;
    info->model_signature_mismatch = false;
    DWORD attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES || attributes & FILE_ATTRIBUTE_DIRECTORY) {
        info->status = DLSSNR_RUNTIME_MISSING;
        snprintf(info->error, sizeof(info->error),
                 "nvngx_dlssnr.dll is missing at the configured model-path");
        return NULL;
    }
    if (InterlockedCompareExchange(&runtime_fault, 0, 0) ||
        InterlockedCompareExchange(&runtime_owner, 1, 0)) {
        info->status = DLSSNR_RUNTIME_FAILED;
        snprintf(info->error, sizeof(info->error),
                 "NGX is faulted or another native DLSSNR instance owns the runtime");
        return NULL;
    }
    struct dlssnr_runtime *r = calloc(1, sizeof(*r));
    if (!r) {
        InterlockedExchange(&runtime_owner, 0);
        return NULL;
    }
    r->device = device;
    ID3D12Device_AddRef(device);
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (const wchar_t *)&module_anchor, &r->caller_module)) {
        info->status = DLSSNR_RUNTIME_MISSING;
        snprintf(info->error, sizeof(info->error), "Cannot identify the libmpv runtime module");
        goto fail;
    }
    if (r->caller_module && r->caller_module != GetModuleHandleW(NULL))
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                          (const wchar_t *)&module_anchor, &r->module_pin);
    if (!load_bridge(r, info))
        goto fail;
    r->core = load_core(info);
    if (!r->core)
        goto fail;
    if (GetModuleHandleW(path)) {
        info->status = DLSSNR_RUNTIME_FAILED;
        snprintf(info->error, sizeof(info->error),
                 "NR model is already loaded by another component; cannot isolate its lifetime");
        goto fail;
    }
    r->model_file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                                OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (r->model_file == INVALID_HANDLE_VALUE) {
        r->model_file = NULL;
        info->status = DLSSNR_RUNTIME_MISSING;
        snprintf(info->error, sizeof(info->error), "Cannot open the configured model file");
        goto fail;
    }
    r->model = LoadLibraryExW(path, NULL,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!r->model) {
        info->status = DLSSNR_RUNTIME_MISSING;
        snprintf(info->error, sizeof(info->error),
                 "Cannot load the configured NR model (Windows error %lu)", GetLastError());
        goto fail;
    }
    dlssnr_model_identify(r->model_file, info);
    ngx_init_project_fn core_init;
    ngx_allocate_fn allocate;
    ngx_model_init_fn model_init;
    if (!BIND(r->core, core_init, "NVSDK_NGX_D3D12_Init_ProjectID") ||
        !BIND(r->core, allocate, "NVSDK_NGX_D3D12_AllocateParameters") ||
        !BIND(r->core, r->destroy_parameters, "NVSDK_NGX_D3D12_DestroyParameters") ||
        !BIND(r->core, r->core_shutdown, "NVSDK_NGX_D3D12_Shutdown1") ||
        !BIND(r->model, model_init, "NVSDK_NGX_D3D12_Init_Ext") ||
        !BIND(r->model, r->create, "NVSDK_NGX_D3D12_CreateFeature") ||
        !BIND(r->model, r->evaluate, "NVSDK_NGX_D3D12_EvaluateFeature") ||
        !BIND(r->model, r->release, "NVSDK_NGX_D3D12_ReleaseFeature") ||
        !BIND(r->model, r->shutdown, "NVSDK_NGX_D3D12_Shutdown1"))
        goto fail;
    size_t length = wcslen(path);
    wchar_t *directory = malloc((length + 1) * sizeof(wchar_t));
    if (!directory)
        goto fail;
    wmemcpy(directory, path, length + 1);
    wchar_t *slash = wcsrchr(directory, L'\\');
    if (!slash) {
        free(directory);
        goto fail;
    }
    *slash = 0;
    wchar_t data_directory[32768];
    DWORD data_length = GetModuleFileNameW(r->caller_module, data_directory, 32768);
    wchar_t *data_slash = data_length && data_length < 32768 ?
                          wcsrchr(data_directory, L'\\') : NULL;
    static const wchar_t cache_suffix[] = L"\\dlssnr-cache";
    if (!data_slash || (size_t)(data_slash - data_directory) +
        sizeof(cache_suffix) / sizeof(cache_suffix[0]) > 32768) {
        free(directory);
        info->status = DLSSNR_RUNTIME_FAILED;
        snprintf(info->error, sizeof(info->error), "Cannot resolve the NR cache directory");
        goto fail;
    }
    wmemcpy(data_slash, cache_suffix, sizeof(cache_suffix) / sizeof(cache_suffix[0]));
    if (!CreateDirectoryW(data_directory, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        free(directory);
        info->status = DLSSNR_RUNTIME_FAILED;
        snprintf(info->error, sizeof(info->error),
                 "The libmpv-adjacent dlssnr-cache directory is not writable");
        goto fail;
    }
    const wchar_t *search[] = {directory};
    struct ngx_common_info common = {
        .search = {.paths = search, .count = 1},
        .logging = {.callback = runtime_log, .level = 1,
                    .disable_other_sinks = true},
    };
    bool initialized = false;
    __try {
        ngx_result result = core_init("b4f841f0-9c9b-4754-8b3a-1c79d4adb09a",
            0, "mpv-native-dlssnr-1", data_directory, device, NGX_API_VERSION, &common);
        if (sdk_result(info, "NGX core Init_ProjectID", result)) {
            r->core_initialized = true;
            result = allocate(&r->parameters);
            if (sdk_result(info, "NGX AllocateParameters", result) && r->parameters) {
                result = r->bridge->init(model_init, 0, data_directory,
                                          device, NGX_API_VERSION, NULL);
                if (sdk_result(info, "NR model Init_Ext", result)) {
                    r->model_initialized = true;
                    initialized = true;
                }
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        sdk_exception(r, info, GetExceptionCode());
    }
    free(directory);
    if (!initialized)
        goto fail;
    return r;
fail:
    dlssnr_runtime_close(&r, info);
    return NULL;
}

static ngx_result __cdecl scaling_ratio(struct ngx_parameters *parameters)
{
    if (!parameters)
        return 0xbad00005u;
    parameters->vtable->set_f(parameters, "DLSSNR.ScalingRatio", 1);
    return NGX_SUCCESS;
}

bool dlssnr_runtime_release_feature(struct dlssnr_runtime *r,
                                   struct dlssnr_gpu_info *info)
{
    if (!r || !r->feature)
        return true;
    if (r->poisoned || InterlockedCompareExchange(&runtime_fault, 0, 0))
        return false;
    __try {
        if (!sdk_result(info, "NR ReleaseFeature",
                        r->bridge->release(r->release, r->feature)))
            return false;
        r->feature = NULL;
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        sdk_exception(r, info, GetExceptionCode());
        return false;
    }
}

bool dlssnr_runtime_create(struct dlssnr_runtime *r,
                          ID3D12GraphicsCommandList *commands,
                          int width, int height, int preset,
                          struct dlssnr_gpu_info *info)
{
    if (r->poisoned || !dlssnr_runtime_release_feature(r, info))
        return false;
    __try {
        struct ngx_parameters *p = r->parameters;
        const struct ngx_parameter_vtable *v = p->vtable;
        v->reset(p);
        static const char *const dimensions[][2] = {
            {"Width", "Height"},
            {"DLSSNR.Width", "DLSSNR.Height"},
            {"DLSSNR.InputWidth", "DLSSNR.InputHeight"},
            {"DLSSNR.OutputWidth", "DLSSNR.OutputHeight"},
            {"DLSSNR.Output.Width", "DLSSNR.Output.Height"},
        };
        for (size_t n = 0; n < sizeof(dimensions) / sizeof(dimensions[0]); n++) {
            v->set_u(p, dimensions[n][0], width);
            v->set_u(p, dimensions[n][1], height);
        }
        v->set_i(p, "PerfQualityValue", 1);
        v->set_u(p, "CreationNodeMask", 1);
        v->set_u(p, "VisibilityNodeMask", 1);
        v->set_i(p, "DLSSNR.Hint.Render.Preset", preset);
        v->set_u(p, "DLSSNR.Upscaling", 0);
        v->set_f(p, "DLSSNR.Scale", 1);
        v->set_f(p, "DLSSNR.ScalingRatio", 1);
        v->set_i(p, "DLSS.Indicator.Invert.X.Axis", 0);
        v->set_i(p, "DLSS.Indicator.Invert.Y.Axis", 0);
        ngx_result (__cdecl *callback)(struct ngx_parameters *) = scaling_ratio;
        void *pointer;
        memcpy(&pointer, &callback, sizeof(pointer));
        v->set_pointer(p, "DLSSNRComputeScalingRatioCallback", pointer);
        r->width = width;
        r->height = height;
        return sdk_result(info, "NR CreateFeature18",
            r->bridge->create(r->create, commands, NGX_FEATURE_NR, p, &r->feature)) &&
            r->feature;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        sdk_exception(r, info, GetExceptionCode());
        return false;
    }
}

bool dlssnr_runtime_evaluate(struct dlssnr_runtime *r,
                            ID3D12GraphicsCommandList *commands,
                            ID3D12Resource *color, ID3D12Resource *output,
                            ID3D12Resource *motion, ID3D12Resource *depth,
                            const struct dlssnr_options *o,
                            bool reset, struct dlssnr_gpu_info *info)
{
    if (!r->feature || r->poisoned ||
        InterlockedCompareExchange(&runtime_fault, 0, 0))
        return false;
    __try {
        struct ngx_parameters *p = r->parameters;
        const struct ngx_parameter_vtable *v = p->vtable;
        static const char *const resource_names[] = {"Color", "Output", "MVec", "Depth"};
        ID3D12Resource *resources[] = {color, output, motion, depth};
        for (size_t n = 0; n < 4; n++) {
            char key[80];
            snprintf(key, sizeof(key), "DLSSNR.%s", resource_names[n]);
            v->set_d3d12(p, key, resources[n]);
            static const char *const parts[] = {
                "SubrectBaseX", "SubrectBaseY", "SubrectWidth", "SubrectHeight",
            };
            const int values[] = {0, 0, r->width, r->height};
            for (size_t part = 0; part < 4; part++) {
                snprintf(key, sizeof(key), "DLSSNR.%s%s",
                         resource_names[n], parts[part]);
                v->set_i(p, key, values[part]);
            }
        }
        v->set_f(p, "DLSSNR.MVecScaleX", 1);
        v->set_f(p, "DLSSNR.MVecScaleY", 1);
        v->set_i(p, "DLSSNR.DepthInverted", 1);
        v->set_i(p, "DLSSNR.Enabled", 1);
        v->set_i(p, "DLSSNR.Reset", reset);
        v->set_i(p, "DLSSNR.Style", o->style);
        v->set_f(p, "DLSSNR.Intensity", o->intensity);
        v->set_f(p, "DLSSNR.LocalToneStrength", o->local_tone);
        v->set_f(p, "DLSSNR.LocalStructureStrength", o->local_structure);
        v->set_f(p, "DLSSNR.SkinStructureStrength", o->skin_structure);
        v->set_i(p, "DLSSNR.UseAutoMask", o->auto_mask);
        v->set_i(p, "DLSSNR.UICorrection", o->ui_correction);
        return sdk_result(info, "NR EvaluateFeature18",
            r->bridge->evaluate(r->evaluate, commands, r->feature, p, NULL));
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        sdk_exception(r, info, GetExceptionCode());
        return false;
    }
}

bool dlssnr_runtime_poisoned(const struct dlssnr_runtime *r)
{
    return r && r->poisoned;
}

bool dlssnr_runtime_close(struct dlssnr_runtime **runtime,
                         struct dlssnr_gpu_info *info)
{
    struct dlssnr_runtime *r = *runtime;
    if (!r)
        return true;
    if (r->poisoned || !dlssnr_runtime_release_feature(r, info))
        return false;
    bool complete = true;
    __try {
        if (r->model_initialized)
            complete = sdk_result(info, "NR Shutdown1",
                                  r->bridge->shutdown(r->shutdown, r->device));
        if (complete && r->parameters)
            complete = sdk_result(info, "NGX DestroyParameters",
                                  r->destroy_parameters(r->parameters));
        if (complete && r->core_initialized) {
            unsigned remaining = 0;
            complete = sdk_result(info, "NGX core Shutdown1",
                                  r->core_shutdown(r->device, &remaining));
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        sdk_exception(r, info, GetExceptionCode());
        complete = false;
    }
    if (!complete)
        return false;
    info->caller_compatibility = false;
    if (r->model)
        FreeLibrary(r->model);
    if (r->model_file)
        CloseHandle(r->model_file);
    if (r->core)
        FreeLibrary(r->core);
    if (r->bridge_module)
        FreeLibrary(r->bridge_module);
    ID3D12Device_Release(r->device);
    HMODULE pin = r->module_pin;
    free(r);
    *runtime = NULL;
    InterlockedExchange(&runtime_owner, 0);
    if (pin)
        FreeLibrary(pin);
    return true;
}
