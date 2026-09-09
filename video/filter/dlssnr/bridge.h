/*
 * This file is part of mpv.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef MP_DLSSNR_BRIDGE_H
#define MP_DLSSNR_BRIDGE_H

#include "ngx_abi.h"

#define MPV_NGX_BRIDGE_ABI_VERSION 1u
#define MPV_NGX_BRIDGE_FILENAME L"mpv-nvngx.dll"
#define MPV_NGX_BRIDGE_ENTRYPOINT "mpv_ngx_bridge_get_api"

struct mpv_ngx_bridge_api {
    uint32_t abi_version;
    uint32_t struct_size;
    ngx_result (__cdecl *init)(ngx_model_init_fn, uint64_t, const wchar_t *,
                              struct ID3D12Device *, unsigned,
                              const struct ngx_parameters *);
    ngx_result (__cdecl *create)(ngx_create_fn, struct ID3D12GraphicsCommandList *,
                                int, const struct ngx_parameters *,
                                struct ngx_handle **);
    ngx_result (__cdecl *evaluate)(ngx_evaluate_fn,
                                  struct ID3D12GraphicsCommandList *,
                                  const struct ngx_handle *,
                                  const struct ngx_parameters *, void *);
    ngx_result (__cdecl *release)(ngx_release_fn, struct ngx_handle *);
    ngx_result (__cdecl *shutdown)(ngx_model_shutdown_fn, struct ID3D12Device *);
};

typedef const struct mpv_ngx_bridge_api *(__cdecl *mpv_ngx_bridge_get_api_fn)(uint32_t);

#ifdef MPV_NGX_BRIDGE_BUILD
__declspec(dllexport)
#endif
const struct mpv_ngx_bridge_api *__cdecl mpv_ngx_bridge_get_api(uint32_t version);

#endif
