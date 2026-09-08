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

#ifndef MP_DLSSNR_NGX_ABI_H
#define MP_DLSSNR_NGX_ABI_H

#include <stdbool.h>
#include <stdint.h>
#include <wchar.h>

#if !defined(_WIN64)
#error This interface describes the Microsoft x64 NGX ABI only.
#endif

struct ID3D11Resource;
struct ID3D12Resource;
struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ngx_parameters;
struct ngx_handle;

typedef uint32_t ngx_result;

#define NGX_API_VERSION 0x15u
#define NGX_FEATURE_NR 18
#define NGX_SUCCESS 1u

static inline bool ngx_failed(ngx_result result)
{
    return (result & 0xfff00000u) == 0xbad00000u;
}

/*
 * The public NGX parameter interface has eight Set overloads, eight Get
 * overloads, then Reset, with no virtual destructor. Microsoft x64 reverses
 * each overload group in the vtable. Explicit C declarations avoid requiring
 * a C++ compiler ABI or NVIDIA's static SDK library in the mpv build.
 */
struct ngx_parameter_vtable {
    void (__cdecl *set_pointer)(struct ngx_parameters *, const char *, void *);
    void (__cdecl *set_d3d12)(struct ngx_parameters *, const char *,
                            struct ID3D12Resource *);
    void (__cdecl *set_d3d11)(struct ngx_parameters *, const char *,
                            struct ID3D11Resource *);
    void (__cdecl *set_i)(struct ngx_parameters *, const char *, int);
    void (__cdecl *set_u)(struct ngx_parameters *, const char *, unsigned);
    void (__cdecl *set_d)(struct ngx_parameters *, const char *, double);
    void (__cdecl *set_f)(struct ngx_parameters *, const char *, float);
    void (__cdecl *set_ull)(struct ngx_parameters *, const char *, uint64_t);
    ngx_result (__cdecl *get_pointer)(const struct ngx_parameters *, const char *,
                                     void **);
    ngx_result (__cdecl *get_d3d12)(const struct ngx_parameters *, const char *,
                                   struct ID3D12Resource **);
    ngx_result (__cdecl *get_d3d11)(const struct ngx_parameters *, const char *,
                                   struct ID3D11Resource **);
    ngx_result (__cdecl *get_i)(const struct ngx_parameters *, const char *, int *);
    ngx_result (__cdecl *get_u)(const struct ngx_parameters *, const char *,
                               unsigned *);
    ngx_result (__cdecl *get_d)(const struct ngx_parameters *, const char *,
                               double *);
    ngx_result (__cdecl *get_f)(const struct ngx_parameters *, const char *,
                               float *);
    ngx_result (__cdecl *get_ull)(const struct ngx_parameters *, const char *,
                                 uint64_t *);
    void (__cdecl *reset)(struct ngx_parameters *);
};

struct ngx_parameters {
    const struct ngx_parameter_vtable *vtable;
};

typedef void (__cdecl *ngx_log_callback)(const char *, int, int);

struct ngx_common_info {
    struct {
        const wchar_t *const *paths;
        unsigned count;
    } search;
    void *internal;
    struct {
        ngx_log_callback callback;
        int level;
        bool disable_other_sinks;
    } logging;
};

/*
 * This is the driver export named Init_ProjectID, not the SDK's static
 * Init_with_ProjectID wrapper. The driver takes version BEFORE common_info.
 * In particular, this is not the model/snippet's Init_Ext signature.
 */
typedef ngx_result (__cdecl *ngx_init_project_fn)(
    const char *project_id, int engine_type, const char *engine_version,
    const wchar_t *data_directory, struct ID3D12Device *device,
    unsigned api_version, const struct ngx_common_info *common_info);
typedef ngx_result (__cdecl *ngx_allocate_fn)(struct ngx_parameters **);
typedef ngx_result (__cdecl *ngx_destroy_parameters_fn)(struct ngx_parameters *);
typedef ngx_result (__cdecl *ngx_create_fn)(
    struct ID3D12GraphicsCommandList *, int, const struct ngx_parameters *,
    struct ngx_handle **);
typedef ngx_result (__cdecl *ngx_release_fn)(struct ngx_handle *);
/* The driver writes a remaining-device count that the SDK wrapper hides. */
typedef ngx_result (__cdecl *ngx_shutdown_fn)(struct ID3D12Device *, unsigned *);
typedef ngx_result (__cdecl *ngx_model_init_fn)(
    uint64_t, const wchar_t *, struct ID3D12Device *, unsigned,
    const struct ngx_parameters *);
typedef ngx_result (__cdecl *ngx_model_shutdown_fn)(struct ID3D12Device *);
typedef ngx_result (__cdecl *ngx_evaluate_fn)(
    struct ID3D12GraphicsCommandList *, const struct ngx_handle *,
    const struct ngx_parameters *, void *);

#endif
