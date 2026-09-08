/*
 * This file is part of mpv.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef MP_DLSSNR_GPU_H
#define MP_DLSSNR_GPU_H

#include <stdbool.h>
#include <stdint.h>
#include <wchar.h>
#include <windows.h>
#include <d3d11.h>

#include "params.h"
#include "shaders.h"

#define DLSSNR_ERROR_SIZE 512
#define DLSSNR_GPU_NAME_SIZE 256
/* Mirrors the decoder's six-surface baseline and VO retention + three in flight. */
#define DLSSNR_INITIAL_OUTPUT_SLOTS 6
#define DLSSNR_MAX_OUTPUT_SLOTS 23

enum dlssnr_gpu_result {
    DLSSNR_GPU_ERROR,
    DLSSNR_GPU_READY,
    DLSSNR_GPU_CANCELLED,
};

enum dlssnr_status {
    DLSSNR_DISABLED,
    DLSSNR_INITIALIZING,
    DLSSNR_ACTIVE,
    DLSSNR_SOURCE_HDR,
    DLSSNR_UNSUPPORTED_FORMAT,
    DLSSNR_RUNTIME_MISSING,
    DLSSNR_RUNTIME_FAILED,
    DLSSNR_ADAPTER_MISMATCH,
};

struct dlssnr_gpu;

struct dlssnr_gpu_info {
    enum dlssnr_status status;
    int proc_width, proc_height;
    LUID luid;
    char gpu_name[DLSSNR_GPU_NAME_SIZE];
    char error[DLSSNR_ERROR_SIZE];
    double wall_ms;
    bool caller_compatibility;
    bool model_signature_mismatch;
    uint64_t runtime_loads, feature_builds;
    uint64_t backpressure_waits;
    double slot_wait_ms;
    unsigned output_slots, output_capacity;
};

struct dlssnr_gpu_input {
    ID3D11Texture2D *texture;
    unsigned subresource;
    struct dlssnr_shader_config color;
    void *lifetime;
    void (*release_lifetime)(void *);
    HANDLE cancel_event;
    unsigned output_capacity;
};

struct dlssnr_gpu_output {
    ID3D11Texture2D *texture;
    void *lease;
    void (*release)(void *);
};

/* Takes input.lifetime on every result; slot starvation waits until release or cancellation. */
enum dlssnr_gpu_result dlssnr_gpu_process(
    struct dlssnr_gpu **gpu, const struct dlssnr_gpu_input *input,
    const struct dlssnr_options *options, bool reset_history,
    struct dlssnr_gpu_output *output, struct dlssnr_gpu_info *info);
void dlssnr_gpu_destroy(struct dlssnr_gpu **gpu);

bool dlssnr_resolve_model_path(const char *configured, wchar_t *path,
                              size_t count, char error[DLSSNR_ERROR_SIZE]);

#endif
