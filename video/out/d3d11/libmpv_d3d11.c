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

#include <libplacebo/d3d11.h>

#include "common/msg.h"
#include "mpv/render_d3d11.h"
#include "video/out/gpu_next/libmpv_gpu_next.h"
#include "video/out/libmpv.h"
#include "video/out/placebo/utils.h"

#include <d3d11.h>

struct priv {
    pl_d3d11 d3d11;
    pl_tex wrapped_tex;
};

static int init(struct libmpv_gpu_next_context *ctx, mpv_render_param *params)
{
    ctx->priv = talloc_zero(NULL, struct priv);
    struct priv *p = ctx->priv;

    mpv_d3d11_init_params *d3d_params =
        get_mpv_render_param(params, MPV_RENDER_PARAM_D3D11_INIT_PARAMS, NULL);
    if (!d3d_params || !d3d_params->device) {
        MP_FATAL(ctx, "Missing D3D11 device in init params.\n");
        return MPV_ERROR_INVALID_PARAMETER;
    }

    ctx->pllog = mppl_log_create(ctx, ctx->log);
    if (!ctx->pllog) {
        MP_FATAL(ctx, "Failed to create libplacebo log.\n");
        return MPV_ERROR_GENERIC;
    }

    p->d3d11 = pl_d3d11_create(ctx->pllog, pl_d3d11_params(
        .device = (ID3D11Device *)d3d_params->device,
    ));
    if (!p->d3d11) {
        MP_FATAL(ctx, "Failed to create libplacebo D3D11 context.\n");
        return MPV_ERROR_UNSUPPORTED;
    }

    ctx->gpu = p->d3d11->gpu;
    return 0;
}

static int wrap_fbo(struct libmpv_gpu_next_context *ctx, mpv_render_param *params,
                    pl_tex *out, int *w, int *h)
{
    struct priv *p = ctx->priv;

    mpv_d3d11_fbo *fbo =
        get_mpv_render_param(params, MPV_RENDER_PARAM_D3D11_FBO, NULL);
    if (!fbo || !fbo->texture) {
        MP_FATAL(ctx, "Missing D3D11 FBO in render params.\n");
        return MPV_ERROR_INVALID_PARAMETER;
    }

    // Release previous wrapped texture if any
    if (p->wrapped_tex)
        pl_tex_destroy(ctx->gpu, &p->wrapped_tex);

    p->wrapped_tex = pl_d3d11_wrap(ctx->gpu, pl_d3d11_wrap_params(
        .tex = (ID3D11Resource *)fbo->texture,
        .w = fbo->w,
        .h = fbo->h,
    ));
    if (!p->wrapped_tex) {
        MP_FATAL(ctx, "Failed to wrap D3D11 texture with libplacebo.\n");
        return MPV_ERROR_GENERIC;
    }

    *out = p->wrapped_tex;
    *w = fbo->w;
    *h = fbo->h;
    return 0;
}

static void done_frame(struct libmpv_gpu_next_context *ctx, bool ds)
{
    // No-op: the caller manages Present/composition timing.
    (void)ctx;
    (void)ds;
}

static void destroy(struct libmpv_gpu_next_context *ctx)
{
    struct priv *p = ctx->priv;
    if (!p)
        return;

    if (p->wrapped_tex)
        pl_tex_destroy(ctx->gpu, &p->wrapped_tex);

    if (p->d3d11)
        pl_d3d11_destroy(&p->d3d11);

    if (ctx->pllog)
        pl_log_destroy(&ctx->pllog);
}

const struct libmpv_gpu_next_context_fns libmpv_gpu_next_context_d3d11 = {
    .api_name = MPV_RENDER_API_TYPE_D3D11,
    .init = init,
    .wrap_fbo = wrap_fbo,
    .done_frame = done_frame,
    .destroy = destroy,
};
