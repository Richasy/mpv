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
};

extern const struct libmpv_gpu_next_context_fns libmpv_gpu_next_context_d3d11;
