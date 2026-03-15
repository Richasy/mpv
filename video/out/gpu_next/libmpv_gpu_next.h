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
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "video/out/libmpv.h"

#include <libplacebo/renderer.h>
#include <libplacebo/utils/frame_queue.h>

struct pl_tex;
struct ID3D11Texture2D;

// Context interface for gpu-next libmpv backends (e.g. D3D11).
// This is similar to libmpv_gpu_context but uses libplacebo directly
// instead of going through the ra abstraction layer.
struct libmpv_gpu_next_context {
    struct mpv_global *global;
    struct mp_log *log;
    const struct libmpv_gpu_next_context_fns *fns;

    pl_log pllog;
    pl_gpu gpu;
    void *priv;
};

// Backend-specific functions for initializing and using a gpu-next context
// without a window/swapchain. The caller provides the render target directly.
struct libmpv_gpu_next_context_fns {
    // The libmpv API type name, see MPV_RENDER_PARAM_API_TYPE.
    const char *api_name;

    // Initialize the backend. Extract parameters from params, create pl_gpu.
    // Must set ctx->gpu and ctx->pllog on success.
    int (*init)(struct libmpv_gpu_next_context *ctx, mpv_render_param *params);

    // Wrap the render target from params into a pl_tex.
    // The returned pl_tex is valid until the next wrap_fbo or done_frame call.
    // Also outputs the target dimensions and color space.
    int (*wrap_fbo)(struct libmpv_gpu_next_context *ctx, mpv_render_param *params,
                    pl_tex *out, int *w, int *h, struct pl_color_space *out_csp);

    // Called after rendering is complete for a frame.
    // For D3D11 this is a no-op (caller manages Present).
    void (*done_frame)(struct libmpv_gpu_next_context *ctx, bool ds);

    // Free all resources in ctx->priv.
    void (*destroy)(struct libmpv_gpu_next_context *ctx);

    // --- NVIDIA NGX VSR ---

    // Check if NGX VSR is available on this backend. Returns true if available.
    bool (*ngx_vsr_available)(struct libmpv_gpu_next_context *ctx);

    // Create an RGBA8 D3D11 texture suitable for NGX VSR input/output.
    // Flags: BIND_RENDER_TARGET | BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS.
    // Returns the texture via out_tex, or NULL on failure.
    // The caller must wrap this with pl_d3d11_wrap() for use with libplacebo.
    struct ID3D11Texture2D *(*ngx_create_texture)(
        struct libmpv_gpu_next_context *ctx, int w, int h);

    // Run NGX VSR: upscale input_tex to output_tex.
    // input_tex: RGBA8 texture at source resolution (e.g. 480p)
    // output_tex: RGBA8 texture at target resolution (e.g. 1080p), must have UAV
    // quality: 0=bicubic, 1=low, 2=medium, 3=high, 4=ultra
    // Returns true on success.
    bool (*ngx_vsr_process)(struct libmpv_gpu_next_context *ctx,
                            struct ID3D11Texture2D *input_tex,
                            int in_w, int in_h,
                            struct ID3D11Texture2D *output_tex,
                            int out_w, int out_h,
                            int quality);

    // --- NVIDIA NGX TrueHDR ---

    // Check if NGX TrueHDR is available on this backend. Returns true if available.
    bool (*ngx_truehdr_available)(struct libmpv_gpu_next_context *ctx);

    // Create an R10G10B10A2 D3D11 texture suitable for HDR output.
    // Flags: BIND_RENDER_TARGET | BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS.
    // Returns the texture, or NULL on failure.
    struct ID3D11Texture2D *(*ngx_create_hdr_texture)(
        struct libmpv_gpu_next_context *ctx, int w, int h);

    // Run NGX TrueHDR: convert SDR input_tex to HDR output_tex.
    // input_tex: RGBA8 SDR texture
    // output_tex: R10G10B10A2 HDR texture
    // preset: 1=natural, 2=standard, 3=vivid
    // max_luminance: monitor peak nits (0 = use SDK default 1000)
    // Returns true on success.
    bool (*ngx_truehdr_process)(struct libmpv_gpu_next_context *ctx,
                                 struct ID3D11Texture2D *input_tex,
                                 int in_w, int in_h,
                                 struct ID3D11Texture2D *output_tex,
                                 int out_w, int out_h,
                                 int preset, unsigned int max_luminance);
};

extern const struct libmpv_gpu_next_context_fns libmpv_gpu_next_context_d3d11;
