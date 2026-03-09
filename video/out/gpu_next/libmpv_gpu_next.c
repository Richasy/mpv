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

#include <libplacebo/options.h>
#include <libplacebo/renderer.h>
#include <libplacebo/utils/frame_queue.h>
#include <libplacebo/utils/libav.h>

#include "common/common.h"
#include "options/m_config.h"
#include "options/options.h"
#include "video/fmt-conversion.h"
#include "video/mp_image.h"
#include "video/out/libmpv.h"
#include "video/out/gpu/hwdec.h"
#include "video/out/gpu/video.h"
#include "video/out/placebo/utils.h"

#include "libmpv_gpu_next.h"

static const struct libmpv_gpu_next_context_fns *context_backends[] = {
#if HAVE_D3D11 && defined(PL_HAVE_D3D11)
    &libmpv_gpu_next_context_d3d11,
#endif
    NULL
};

struct frame_priv {
    struct render_backend *ctx;
};

struct priv {
    struct libmpv_gpu_next_context *context;

    pl_renderer rr;
    pl_queue queue;
    pl_gpu gpu;

    struct mp_rect src, dst;
    struct mp_osd_res osd_res;

    uint64_t last_id;
    double last_pts;
    bool want_reset;

    pl_options pars;
    struct m_config_cache *opts_cache;
    struct mp_csp_equalizer_state *video_eq;
};

static int plane_data_from_imgfmt(struct pl_plane_data out_data[4],
                                  struct pl_bit_encoding *out_bits,
                                  enum mp_imgfmt imgfmt, bool use_uint)
{
    struct mp_imgfmt_desc desc = mp_imgfmt_get_desc(imgfmt);
    if (!desc.num_planes || !(desc.flags & MP_IMGFLAG_HAS_COMPS))
        return 0;
    if (desc.flags & MP_IMGFLAG_HWACCEL)
        return 0;
    if (!(desc.flags & MP_IMGFLAG_NE))
        return 0;
    if (desc.flags & MP_IMGFLAG_PAL)
        return 0;
    if ((desc.flags & MP_IMGFLAG_TYPE_FLOAT) && (desc.flags & MP_IMGFLAG_YUV))
        return 0;

    bool has_bits = false;
    bool any_padded = false;

    for (int p = 0; p < desc.num_planes; p++) {
        struct pl_plane_data *data = &out_data[p];
        struct mp_imgfmt_comp_desc sorted[MP_NUM_COMPONENTS];
        int num_comps = 0;
        if (desc.bpp[p] % 8)
            return 0;

        for (int c = 0; c < mp_imgfmt_desc_get_num_comps(&desc); c++) {
            if (desc.comps[c].plane != p)
                continue;
            data->component_map[num_comps] = c;
            sorted[num_comps] = desc.comps[c];
            num_comps++;
            for (int i = num_comps - 1; i > 0; i--) {
                if (sorted[i].offset >= sorted[i - 1].offset)
                    break;
                MPSWAP(struct mp_imgfmt_comp_desc, sorted[i], sorted[i - 1]);
                MPSWAP(int, data->component_map[i], data->component_map[i - 1]);
            }
        }

        uint64_t total_bits = 0;
        memset(data->component_size, 0, sizeof(data->component_size));
        for (int c = 0; c < num_comps; c++) {
            data->component_size[c] = sorted[c].size;
            data->component_pad[c] = sorted[c].offset - total_bits;
            total_bits += data->component_pad[c] + data->component_size[c];
            any_padded |= sorted[c].pad;
            if (!out_bits || data->component_map[c] == PL_CHANNEL_A)
                continue;
            struct pl_bit_encoding bits = {
                .sample_depth = data->component_size[c],
                .color_depth = sorted[c].size - abs(sorted[c].pad),
                .bit_shift = MPMAX(sorted[c].pad, 0),
            };
            if (!has_bits) {
                *out_bits = bits;
                has_bits = true;
            } else if (!pl_bit_encoding_equal(out_bits, &bits)) {
                *out_bits = (struct pl_bit_encoding) {0};
                out_bits = NULL;
            }
        }

        data->pixel_stride = desc.bpp[p] / 8;
        data->type = (desc.flags & MP_IMGFLAG_TYPE_FLOAT)
                            ? PL_FMT_FLOAT
                            : (use_uint ? PL_FMT_UINT : PL_FMT_UNORM);
    }

    if (any_padded && !out_bits)
        return 0;

    return desc.num_planes;
}

static bool format_supported(struct priv *p, int format, bool use_uint)
{
    struct pl_bit_encoding bits;
    struct pl_plane_data data[4] = {0};
    int planes = plane_data_from_imgfmt(data, &bits, format, use_uint);
    if (!planes)
        return false;
    for (int i = 0; i < planes; i++) {
        if (!pl_plane_find_fmt(p->gpu, NULL, &data[i]))
            return false;
    }
    return true;
}

static bool map_frame(pl_gpu gpu, pl_tex *tex, const struct pl_source_frame *src,
                      struct pl_frame *frame)
{
    struct mp_image *mpi = src->frame_data;
    struct mp_image_params par = mpi->params;
    struct frame_priv *fp = mpi->priv;
    struct priv *p = fp->ctx->priv;

    mp_image_params_guess_csp(&par);

    *frame = (struct pl_frame) {
        .color = par.color,
        .repr = par.repr,
        .profile = {
            .data = mpi->icc_profile ? mpi->icc_profile->data : NULL,
            .len = mpi->icc_profile ? mpi->icc_profile->size : 0,
        },
        .rotation = par.rotate / 90,
        .user_data = mpi,
    };

    struct pl_plane_data data[4] = {0};
    bool use_uint = false;
    if (!format_supported(p, mpi->imgfmt, false))
        use_uint = true;

    frame->num_planes = plane_data_from_imgfmt(data, &frame->repr.bits,
                                                mpi->imgfmt, use_uint);
    for (int n = 0; n < frame->num_planes; n++) {
        struct pl_plane *plane = &frame->planes[n];
        data[n].width = mp_image_plane_w(mpi, n);
        data[n].height = mp_image_plane_h(mpi, n);
        if (mpi->stride[n] < 0) {
            data[n].pixels = mpi->planes[n] + (data[n].height - 1) * mpi->stride[n];
            data[n].row_stride = -mpi->stride[n];
            plane->flipped = true;
        } else {
            data[n].pixels = mpi->planes[n];
            data[n].row_stride = mpi->stride[n];
        }

        if (gpu->limits.callbacks) {
            data[n].callback = talloc_free;
            data[n].priv = mp_image_new_ref(mpi);
        }

        if (!pl_upload_plane(gpu, plane, &tex[n], &data[n])) {
            MP_ERR(fp->ctx, "Failed uploading frame!\n");
            talloc_free(data[n].priv);
            talloc_free(mpi);
            return false;
        }
    }

    pl_frame_set_chroma_location(frame, par.chroma_location);

    if (mpi->film_grain)
        pl_film_grain_from_av(&frame->film_grain, (AVFilmGrainParams *) mpi->film_grain->data);

    pl_icc_profile_compute_signature(&frame->profile);
    return true;
}

static void unmap_frame(pl_gpu gpu, struct pl_frame *frame,
                        const struct pl_source_frame *src)
{
    struct mp_image *mpi = src->frame_data;
    talloc_free(mpi);
}

static void discard_frame(const struct pl_source_frame *src)
{
    struct mp_image *mpi = src->frame_data;
    talloc_free(mpi);
}

static void apply_crop(struct pl_frame *frame, struct mp_rect crop,
                       int width, int height)
{
    frame->crop = (struct pl_rect2df) {
        .x0 = crop.x0,
        .y0 = crop.y0,
        .x1 = crop.x1,
        .y1 = crop.y1,
    };
    pl_rect2df_rotate(&frame->crop, -frame->rotation);
    if (frame->crop.x1 < frame->crop.x0) {
        frame->crop.x0 = width - frame->crop.x0;
        frame->crop.x1 = width - frame->crop.x1;
    }
    if (frame->crop.y1 < frame->crop.y0) {
        frame->crop.y0 = height - frame->crop.y0;
        frame->crop.y1 = height - frame->crop.y1;
    }
}

// --- render_backend_fns implementation ---

static int init(struct render_backend *ctx, mpv_render_param *params)
{
    ctx->priv = talloc_zero(NULL, struct priv);
    struct priv *p = ctx->priv;

    char *api = get_mpv_render_param(params, MPV_RENDER_PARAM_API_TYPE, NULL);
    if (!api)
        return MPV_ERROR_INVALID_PARAMETER;

    for (int n = 0; context_backends[n]; n++) {
        const struct libmpv_gpu_next_context_fns *backend = context_backends[n];
        if (strcmp(backend->api_name, api) == 0) {
            p->context = talloc_zero(NULL, struct libmpv_gpu_next_context);
            *p->context = (struct libmpv_gpu_next_context){
                .global = ctx->global,
                .log = ctx->log,
                .fns = backend,
            };
            break;
        }
    }

    if (!p->context)
        return MPV_ERROR_NOT_IMPLEMENTED;

    int err = p->context->fns->init(p->context, params);
    if (err < 0)
        return err;

    p->gpu = p->context->gpu;
    p->rr = pl_renderer_create(p->context->pllog, p->gpu);
    p->queue = pl_queue_create(p->gpu);
    p->pars = pl_options_alloc(p->context->pllog);
    p->video_eq = mp_csp_equalizer_create(p, ctx->global);
    p->opts_cache = m_config_cache_alloc(p, ctx->global, &gl_video_conf);

    ctx->hwdec_devs = hwdec_devices_create();
    ctx->driver_caps = VO_CAP_ROTATE90 | VO_CAP_VFLIP;
    return 0;
}

static bool check_format(struct render_backend *ctx, int imgfmt)
{
    struct priv *p = ctx->priv;
    return format_supported(p, imgfmt, false) ||
           format_supported(p, imgfmt, true);
}

static int set_parameter(struct render_backend *ctx, mpv_render_param param)
{
    (void)ctx;
    switch (param.type) {
    case MPV_RENDER_PARAM_ICC_PROFILE:
        return 0;
    default:
        return MPV_ERROR_NOT_IMPLEMENTED;
    }
}

static void reconfig(struct render_backend *ctx, struct mp_image_params *params)
{
    // No-op: libmpv mode has no window to reconfigure.
    (void)ctx;
    (void)params;
}

static void reset(struct render_backend *ctx)
{
    struct priv *p = ctx->priv;
    p->want_reset = true;
}

static void update_external(struct render_backend *ctx, struct vo *vo)
{
    struct priv *p = ctx->priv;
    if (vo) {
        struct mp_rect src, dst;
        struct mp_osd_res osd;
        vo_get_src_dst_rects(vo, &src, &dst, &osd);
        p->src = src;
        p->dst = dst;
        p->osd_res = osd;
    }
}

static void resize(struct render_backend *ctx, struct mp_rect *src,
                   struct mp_rect *dst, struct mp_osd_res *osd)
{
    struct priv *p = ctx->priv;
    p->src = *src;
    p->dst = *dst;
    p->osd_res = *osd;
}

static int get_target_size(struct render_backend *ctx, mpv_render_param *params,
                           int *out_w, int *out_h)
{
    struct priv *p = ctx->priv;
    pl_tex tex;
    int w, h;
    struct pl_color_space csp;
    int err = p->context->fns->wrap_fbo(p->context, params, &tex, &w, &h, &csp);
    if (err < 0)
        return err;
    *out_w = w;
    *out_h = h;
    return 0;
}

static int render(struct render_backend *ctx, mpv_render_param *params,
                  struct vo_frame *frame)
{
    struct priv *p = ctx->priv;
    pl_gpu gpu = p->gpu;

    // Wrap the caller's render target
    pl_tex fbo;
    int fbo_w, fbo_h;
    struct pl_color_space fbo_csp;
    int err = p->context->fns->wrap_fbo(p->context, params, &fbo, &fbo_w, &fbo_h, &fbo_csp);
    if (err < 0)
        return err;

    // Update options
    m_config_cache_update(p->opts_cache);
    const struct gl_video_opts *opts = p->opts_cache->opts;

    // Build render params
    pl_options pars = p->pars;
    struct pl_render_params rparams = pars->params;
    rparams.skip_caching_single_frame = !frame->still;
    rparams.frame_mixer = NULL; // No interpolation in libmpv mode

    bool can_interpolate = opts->interpolation && frame->display_synced &&
                           !frame->still && frame->num_frames > 1;
    double pts_offset = can_interpolate ? frame->ideal_frame_vsync : 0;

    // Handle queue reset
    struct pl_source_frame vpts;
    if (frame->current && !p->want_reset) {
        if (pl_queue_peek(p->queue, 0, &vpts) &&
            frame->current->pts + MPMAX(0, pts_offset) < vpts.pts)
        {
            p->want_reset = true;
        }
    }

    // Push all incoming frames into the frame queue
    for (int n = 0; n < frame->num_frames; n++) {
        int id = frame->frame_id + n;

        if (p->want_reset) {
            pl_renderer_flush_cache(p->rr);
            pl_queue_reset(p->queue);
            p->last_pts = 0.0;
            p->last_id = 0;
            p->want_reset = false;
        }

        if (id <= p->last_id)
            continue;

        struct mp_image *mpi = mp_image_new_ref(frame->frames[n]);
        struct frame_priv *fp = talloc_zero(mpi, struct frame_priv);
        mpi->priv = fp;
        fp->ctx = ctx;

        pl_queue_push(p->queue, &(struct pl_source_frame) {
            .pts = mpi->pts,
            .duration = can_interpolate ? frame->approx_duration : 0,
            .frame_data = mpi,
            .map = map_frame,
            .unmap = unmap_frame,
            .discard = discard_frame,
        });

        p->last_id = id;
    }

    // Build target frame
    struct pl_frame target = {
        .repr = pl_color_repr_rgb,
        .num_planes = 1,
        .planes[0] = {
            .texture = fbo,
            .components = fbo->params.format->num_components,
            .component_mapping = {0, 1, 2, 3},
        },
        .color = fbo_csp,
    };

    // Apply target colorspace overrides from options
    if (opts->target_prim)
        target.color.primaries = opts->target_prim;
    if (opts->target_trc)
        target.color.transfer = opts->target_trc;
    if (opts->target_peak)
        target.color.hdr.max_luma = opts->target_peak;

    // Apply crop
    apply_crop(&target, p->dst, fbo_w, fbo_h);

    // Build frame mix from queue
    struct pl_frame_mix mix = {0};
    bool valid = false;

    if (frame->current) {
        struct pl_queue_params qparams = *pl_queue_params(
            .pts = frame->current->pts + pts_offset,
            .radius = pl_frame_mix_radius(&rparams),
            .vsync_duration = can_interpolate ? frame->ideal_frame_vsync_duration : 0,
        );
#if PL_API_VER >= 340
        qparams.drift_compensation = 0;
#endif

        struct pl_source_frame first;
        if (pl_queue_peek(p->queue, 0, &first) && qparams.pts < first.pts)
            qparams.pts = first.pts;
        p->last_pts = qparams.pts;

        switch (pl_queue_update(p->queue, &mix, &qparams)) {
        case PL_QUEUE_ERR:
            MP_ERR(ctx, "Failed updating frames!\n");
            goto done;
        case PL_QUEUE_EOF:
            abort();
        case PL_QUEUE_MORE:
        case PL_QUEUE_OK:
            break;
        }

        // Update source crop on all frames
        for (int i = 0; i < mix.num_frames; i++) {
            struct pl_frame *image = (struct pl_frame *) mix.frames[i];
            struct mp_image *mpi = image->user_data;
            apply_crop(image, p->src, mpi->params.w, mpi->params.h);
        }
    }

    // Render
    if (!pl_render_image_mix(p->rr, &mix, &target, &rparams)) {
        MP_ERR(ctx, "Failed rendering frame!\n");
        goto done;
    }

    valid = true;

done:
    if (!valid)
        pl_tex_clear(gpu, fbo, (float[4]){ 0.5, 0.0, 1.0, 1.0 });

    pl_gpu_flush(gpu);
    p->context->fns->done_frame(p->context, frame->display_synced);
    return 0;
}

static struct mp_image *get_image(struct render_backend *ctx, int imgfmt,
                                  int w, int h, int stride_align, int flags)
{
    // No DR support in libmpv gpu-next mode for simplicity.
    (void)ctx;
    (void)imgfmt;
    (void)w;
    (void)h;
    (void)stride_align;
    (void)flags;
    return NULL;
}

static void screenshot(struct render_backend *ctx, struct vo_frame *frame,
                       struct voctrl_screenshot *args)
{
    // Not implemented for libmpv gpu-next mode.
    (void)ctx;
    (void)frame;
    args->res = NULL;
}

static void perfdata(struct render_backend *ctx,
                     struct voctrl_performance_data *out)
{
    // Not implemented for libmpv gpu-next mode.
    (void)ctx;
    memset(out, 0, sizeof(*out));
}

static void destroy(struct render_backend *ctx)
{
    struct priv *p = ctx->priv;
    if (!p)
        return;

    pl_queue_destroy(&p->queue);
    pl_renderer_destroy(&p->rr);
    pl_options_free(&p->pars);

    hwdec_devices_destroy(ctx->hwdec_devs);

    if (p->context) {
        p->context->fns->destroy(p->context);
        talloc_free(p->context->priv);
        talloc_free(p->context);
    }
}

const struct render_backend_fns render_backend_gpu_next = {
    .init = init,
    .check_format = check_format,
    .set_parameter = set_parameter,
    .reconfig = reconfig,
    .reset = reset,
    .update_external = update_external,
    .resize = resize,
    .get_target_size = get_target_size,
    .render = render,
    .get_image = get_image,
    .screenshot = screenshot,
    .perfdata = perfdata,
    .destroy = destroy,
};
