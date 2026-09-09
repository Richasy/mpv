/*
 * This file is part of mpv.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef MP_DLSSNR_RUNTIME_H
#define MP_DLSSNR_RUNTIME_H

#include <windows.h>
#include <d3d12.h>

#include "gpu.h"

struct dlssnr_runtime;

struct dlssnr_runtime *dlssnr_runtime_open(
    ID3D12Device *device, const wchar_t *model_path,
    struct dlssnr_gpu_info *info);
bool dlssnr_runtime_create(struct dlssnr_runtime *runtime,
                          ID3D12GraphicsCommandList *commands,
                          int width, int height, int preset,
                          struct dlssnr_gpu_info *info);
bool dlssnr_runtime_evaluate(struct dlssnr_runtime *runtime,
                            ID3D12GraphicsCommandList *commands,
                            ID3D12Resource *color, ID3D12Resource *output,
                            ID3D12Resource *motion, ID3D12Resource *depth,
                            const struct dlssnr_options *options,
                            bool reset, struct dlssnr_gpu_info *info);
bool dlssnr_runtime_release_feature(struct dlssnr_runtime *runtime,
                                   struct dlssnr_gpu_info *info);
/* A false return retains a faulted runtime rather than unloading live code. */
bool dlssnr_runtime_close(struct dlssnr_runtime **runtime,
                         struct dlssnr_gpu_info *info);
bool dlssnr_runtime_poisoned(const struct dlssnr_runtime *runtime);

#endif
