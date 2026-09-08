/*
 * This file is part of mpv.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define MPV_NGX_BRIDGE_BUILD
#include "bridge.h"

#if defined(__clang__)
#define KEEP_CALL_FRAME __attribute__((noinline, disable_tail_calls))
#elif defined(_MSC_VER)
#define KEEP_CALL_FRAME __declspec(noinline)
#else
#define KEEP_CALL_FRAME __attribute__((noinline))
#endif

/*
 * The model inspects its genuine caller module. The volatile post-call access
 * also prevents sibling-call elimination when compiler attributes differ.
 */
static KEEP_CALL_FRAME ngx_result __cdecl forward_init(
    ngx_model_init_fn target, uint64_t application_id, const wchar_t *data_path,
    struct ID3D12Device *device, unsigned version,
    const struct ngx_parameters *parameters)
{
    if (!target)
        return 0xbad00005u;
    volatile ngx_result result = target(application_id, data_path, device,
                                        version, parameters);
    return result;
}

static KEEP_CALL_FRAME ngx_result __cdecl forward_create(
    ngx_create_fn target, struct ID3D12GraphicsCommandList *commands, int feature,
    const struct ngx_parameters *parameters, struct ngx_handle **handle)
{
    if (!target)
        return 0xbad00005u;
    volatile ngx_result result = target(commands, feature, parameters, handle);
    return result;
}

static KEEP_CALL_FRAME ngx_result __cdecl forward_evaluate(
    ngx_evaluate_fn target, struct ID3D12GraphicsCommandList *commands,
    const struct ngx_handle *handle, const struct ngx_parameters *parameters,
    void *progress)
{
    if (!target)
        return 0xbad00005u;
    volatile ngx_result result = target(commands, handle, parameters, progress);
    return result;
}

static KEEP_CALL_FRAME ngx_result __cdecl forward_release(
    ngx_release_fn target, struct ngx_handle *handle)
{
    if (!target)
        return 0xbad00005u;
    volatile ngx_result result = target(handle);
    return result;
}

static KEEP_CALL_FRAME ngx_result __cdecl forward_shutdown(
    ngx_model_shutdown_fn target, struct ID3D12Device *device)
{
    if (!target)
        return 0xbad00005u;
    volatile ngx_result result = target(device);
    return result;
}

const struct mpv_ngx_bridge_api *__cdecl mpv_ngx_bridge_get_api(uint32_t version)
{
    static const struct mpv_ngx_bridge_api api = {
        .abi_version = MPV_NGX_BRIDGE_ABI_VERSION,
        .struct_size = sizeof(struct mpv_ngx_bridge_api),
        .init = forward_init,
        .create = forward_create,
        .evaluate = forward_evaluate,
        .release = forward_release,
        .shutdown = forward_shutdown,
    };
    return version == MPV_NGX_BRIDGE_ABI_VERSION ? &api : NULL;
}
