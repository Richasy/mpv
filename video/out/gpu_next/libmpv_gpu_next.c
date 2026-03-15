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
#include "video/img_format.h"
#include "video/mp_image.h"
#include "video/out/libmpv.h"
#include "video/out/gpu/hwdec.h"
#include "video/out/gpu/video.h"
#include "video/out/placebo/utils.h"
#include "sub/osd.h"
#include "sub/draw_bmp.h"

#include "libmpv_gpu_next.h"
#include "gl_next_opts.h"

#if HAVE_D3D11 && defined(PL_HAVE_D3D11) && HAVE_NGX_VSR
#include <libplacebo/d3d11.h>
#include <d3d11.h>
#endif

static const struct libmpv_gpu_next_context_fns *context_backends[] = {
#if HAVE_D3D11 && defined(PL_HAVE_D3D11)
    &libmpv_gpu_next_context_d3d11,
#endif
    NULL
};

// --- OSD overlay structures (from vo_gpu_next.c) ---

struct osd_entry {
    pl_tex tex;
    struct pl_overlay_part *parts;
    int num_parts;
};

struct overlay_state {
    struct osd_entry entries[MAX_OSD_PARTS];
    struct pl_overlay overlays[MAX_OSD_PARTS];
};

// ---

struct frame_info {
    int count;
    struct pl_dispatch_info info[VO_PASS_PERF_MAX];
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

    // OSD rendering state
    struct overlay_state osd_overlay;
    pl_fmt osd_fmt[SUBBITMAP_COUNT];
    pl_tex *sub_tex;
    int num_sub_tex;
    struct osd_state *osd;  // pointer to vo->osd, valid only during render

    uint64_t last_id;
    double last_pts;
    bool want_reset;

    pl_options pars;
    struct m_config_cache *opts_cache;
    struct m_config_cache *next_opts_cache;
    struct gl_next_opts *next_opts;
    struct mp_csp_equalizer_state *video_eq;

#if HAVE_D3D11 && defined(PL_HAVE_D3D11) && HAVE_NGX_VSR
    // NGX VSR cached intermediate textures
    struct ID3D11Texture2D *vsr_intermediate_d3d;   // source-resolution RGBA8
    pl_tex vsr_intermediate_pl;                      // wrapped pl_tex
    int vsr_intermediate_w, vsr_intermediate_h;
    struct ID3D11Texture2D *vsr_output_d3d;         // fbo-resolution RGBA8 (UAV)
    pl_tex vsr_output_pl;
    int vsr_output_w, vsr_output_h;

    // NGX TrueHDR cached textures
    struct ID3D11Texture2D *truehdr_input_d3d;      // RGBA8 SDR (target resolution)
    pl_tex truehdr_input_pl;
    struct ID3D11Texture2D *truehdr_output_d3d;     // R10G10B10A2 HDR
    pl_tex truehdr_output_pl;
    int truehdr_w, truehdr_h;
#endif

    // AMD FSR 1.0 cached textures
#if HAVE_D3D11 && defined(PL_HAVE_D3D11)
    struct ID3D11Texture2D *fsr_intermediate_d3d;   // source-resolution RGBA8
    pl_tex fsr_intermediate_pl;
    int fsr_intermediate_w, fsr_intermediate_h;
    struct ID3D11Texture2D *fsr_output_d3d;         // fbo-resolution RGBA8
    pl_tex fsr_output_pl;
    int fsr_output_w, fsr_output_h;
#endif

    // Performance data of last frame
    struct frame_info perf_fresh;
    struct frame_info perf_redraw;

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

// --- OSD overlay rendering (simplified from vo_gpu_next.c) ---

static void update_overlays(struct render_backend *ctx,
                            struct osd_state *osd_src,
                            struct mp_osd_res res,
                            double pts,
                            struct overlay_state *state,
                            struct pl_frame *frame)
{
    struct priv *p = ctx->priv;

    struct sub_bitmap_list *subs = osd_render(osd_src, res, pts, 0,
                                              mp_draw_sub_formats);

    frame->overlays = state->overlays;
    frame->num_overlays = 0;

    for (int n = 0; n < subs->num_items; n++) {
        const struct sub_bitmaps *item = subs->items[n];
        if (!item->num_parts || !item->packed)
            continue;
        struct osd_entry *entry = &state->entries[item->render_index];
        pl_fmt tex_fmt = p->osd_fmt[item->format];
        if (!tex_fmt)
            continue;
        if (!entry->tex)
            MP_TARRAY_POP(p->sub_tex, p->num_sub_tex, &entry->tex);
        bool ok = pl_tex_recreate(p->gpu, &entry->tex, &(struct pl_tex_params) {
            .format = tex_fmt,
            .w = MPMAX(item->packed_w, entry->tex ? entry->tex->params.w : 0),
            .h = MPMAX(item->packed_h, entry->tex ? entry->tex->params.h : 0),
            .host_writable = true,
            .sampleable = true,
        });
        if (!ok) {
            MP_ERR(ctx, "Failed recreating OSD texture!\n");
            break;
        }
        ok = pl_tex_upload(p->gpu, &(struct pl_tex_transfer_params) {
            .tex        = entry->tex,
            .rc         = { .x1 = item->packed_w, .y1 = item->packed_h, },
            .row_pitch  = item->packed->stride[0],
            .ptr        = item->packed->planes[0],
        });
        if (!ok) {
            MP_ERR(ctx, "Failed uploading OSD texture!\n");
            break;
        }

        entry->num_parts = 0;
        for (int i = 0; i < item->num_parts; i++) {
            const struct sub_bitmap *b = &item->parts[i];
            if (b->dw == 0 || b->dh == 0)
                continue;
            uint32_t c = b->libass.color;
            struct pl_overlay_part part = {
                .src = { b->src_x, b->src_y, b->src_x + b->w, b->src_y + b->h },
                .dst = { b->x, b->y, b->x + b->dw, b->y + b->dh },
                .color = {
                    (c >> 24) / 255.0f,
                    ((c >> 16) & 0xFF) / 255.0f,
                    ((c >> 8) & 0xFF) / 255.0f,
                    (255 - (c & 0xFF)) / 255.0f,
                }
            };
            MP_TARRAY_APPEND(p, entry->parts, entry->num_parts, part);
        }

        struct pl_overlay *ol = &state->overlays[frame->num_overlays++];
        *ol = (struct pl_overlay) {
            .tex = entry->tex,
            .parts = entry->parts,
            .num_parts = entry->num_parts,
            .color = {
                .primaries = PL_COLOR_PRIM_BT_709,
                .transfer = PL_COLOR_TRC_SRGB,
            },
            .coords = PL_OVERLAY_COORDS_DST_FRAME,
        };

        switch (item->format) {
        case SUBBITMAP_BGRA:
            ol->mode = PL_OVERLAY_NORMAL;
            ol->repr.alpha = PL_ALPHA_PREMULTIPLIED;
            break;
        case SUBBITMAP_LIBASS:
            ol->mode = PL_OVERLAY_MONOCHROME;
            ol->repr.alpha = PL_ALPHA_INDEPENDENT;
            break;
        }
    }

    talloc_free(subs);
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
    p->next_opts_cache = m_config_cache_alloc(p, ctx->global, &gl_next_conf);
    p->next_opts = p->next_opts_cache->opts;

    // Initialize OSD texture formats
    p->osd_fmt[SUBBITMAP_LIBASS] = pl_find_named_fmt(p->gpu, "r8");
    p->osd_fmt[SUBBITMAP_BGRA] = pl_find_named_fmt(p->gpu, "bgra8");

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
        // Only update the OSD pointer here. Do NOT recalculate src/dst rects
        // via vo_get_src_dst_rects(), because in libmpv mode the VO does not
        // know the actual viewport size (it's passed by the external render
        // call). The correct src/dst are set by resize(), which is called
        // from mpv_render_context_render() with the real FBO dimensions.
        p->osd = vo->osd;
    } else {
        p->osd = NULL;
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

static void info_callback(void *priv, const struct pl_render_info *info)
{
    struct render_backend *ctx = priv;
    struct priv *p = ctx->priv;
    if (info->index >= VO_PASS_PERF_MAX)
        return; // silently ignore clipped passes

    struct frame_info *frame;
    switch (info->stage) {
    case PL_RENDER_STAGE_FRAME: frame = &p->perf_fresh; break;
    case PL_RENDER_STAGE_BLEND: frame = &p->perf_redraw; break;
    default: abort();
    }

    frame->count = info->index + 1;
    pl_dispatch_info_move(&frame->info[info->index], info->pass);
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
    m_config_cache_update(p->next_opts_cache);
    const struct gl_video_opts *opts = p->opts_cache->opts;

    // Build render params
    pl_options pars = p->pars;
    struct pl_render_params rparams = pars->params;
    rparams.skip_caching_single_frame = !frame->still;
    rparams.frame_mixer = NULL; // No interpolation in libmpv mode
    rparams.info_callback = info_callback;
    rparams.info_priv = ctx;

    // Apply border background mode from gl_next_opts
#if PL_API_VER >= 346
    {
        static const int map_background_types[] = {
            [BACKGROUND_NONE]  = PL_CLEAR_SKIP,
            [BACKGROUND_COLOR] = PL_CLEAR_COLOR,
            [BACKGROUND_TILES] = PL_CLEAR_TILES,
#if PL_API_VER >= 355
            [BACKGROUND_BLUR]  = PL_CLEAR_BLUR,
#endif
        };
        rparams.border = map_background_types[p->next_opts->border_background];
#if PL_API_VER >= 355
        rparams.blur_radius = p->next_opts->background_blur_radius;
#endif
    }
#endif

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

    // Render OSD/subtitle overlays onto target frame
    if (p->osd) {
        double pts = frame->current ? frame->current->pts : 0;
        update_overlays(ctx, p->osd, p->osd_res, pts,
                        &p->osd_overlay, &target);
    }

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
    //
    // NGX pipeline: VSR (optional) → TrueHDR (optional) → FBO
    // Both features can work independently or chained together.
    // When both are active: video → VSR upscale → TrueHDR SDR→HDR → FBO
#if HAVE_D3D11 && defined(PL_HAVE_D3D11) && HAVE_NGX_VSR
    int nvidia_vsr_quality = p->next_opts->nvidia_vsr;
    int nvidia_truehdr_preset = p->next_opts->nvidia_truehdr;

    bool use_ngx_vsr = nvidia_vsr_quality > 0 &&
                        p->context->fns->ngx_vsr_available &&
                        p->context->fns->ngx_vsr_available(p->context) &&
                        frame->current && mix.num_frames > 0;

    bool use_ngx_truehdr = nvidia_truehdr_preset > 0 &&
                            p->context->fns->ngx_truehdr_available &&
                            p->context->fns->ngx_truehdr_available(p->context) &&
                            frame->current && mix.num_frames > 0;

    // --- VSR stage: upscale source → destination resolution ---
    bool vsr_done = false;
    int vsr_dst_w = 0, vsr_dst_h = 0;

    if (use_ngx_vsr) {
        int ngx_quality = nvidia_vsr_quality - 1;

        struct pl_frame *first_frame = (struct pl_frame *) mix.frames[0];
        struct mp_image *src_mpi = first_frame->user_data;
        int src_w = src_mpi->params.w;
        int src_h = src_mpi->params.h;

        int dst_w = p->dst.x1 - p->dst.x0;
        int dst_h = p->dst.y1 - p->dst.y0;
        if (dst_w <= 0 || dst_h <= 0) {
            dst_w = fbo_w;
            dst_h = fbo_h;
        }

        if (src_w >= dst_w && src_h >= dst_h) {
            MP_DBG(ctx, "NGX VSR: Source %dx%d >= dst %dx%d, skipping VSR.\n",
                   src_w, src_h, dst_w, dst_h);
            use_ngx_vsr = false;
        }

        if (use_ngx_vsr) {
            MP_DBG(ctx, "NGX VSR: Two-stage render %dx%d -> %dx%d (quality=%d)\n",
                   src_w, src_h, dst_w, dst_h, ngx_quality);

            // Ensure intermediate texture at source resolution
            if (!p->vsr_intermediate_d3d ||
                p->vsr_intermediate_w != src_w || p->vsr_intermediate_h != src_h)
            {
                if (p->vsr_intermediate_pl)
                    pl_tex_destroy(gpu, &p->vsr_intermediate_pl);
                if (p->vsr_intermediate_d3d) {
                    ID3D11Texture2D_Release(p->vsr_intermediate_d3d);
                    p->vsr_intermediate_d3d = NULL;
                }

                p->vsr_intermediate_d3d =
                    p->context->fns->ngx_create_texture(p->context, src_w, src_h);
                if (p->vsr_intermediate_d3d) {
                    p->vsr_intermediate_pl = pl_d3d11_wrap(gpu, pl_d3d11_wrap_params(
                        .tex = (ID3D11Resource *)p->vsr_intermediate_d3d,
                        .w = src_w,
                        .h = src_h,
                    ));
                    p->vsr_intermediate_w = src_w;
                    p->vsr_intermediate_h = src_h;
                }

                if (!p->vsr_intermediate_pl) {
                    MP_WARN(ctx, "NGX VSR: Failed to create intermediate texture.\n");
                    use_ngx_vsr = false;
                }
            }

            // Ensure output texture at destination resolution
            if (use_ngx_vsr &&
                (!p->vsr_output_d3d ||
                 p->vsr_output_w != dst_w || p->vsr_output_h != dst_h))
            {
                if (p->vsr_output_pl)
                    pl_tex_destroy(gpu, &p->vsr_output_pl);
                if (p->vsr_output_d3d) {
                    ID3D11Texture2D_Release(p->vsr_output_d3d);
                    p->vsr_output_d3d = NULL;
                }

                p->vsr_output_d3d =
                    p->context->fns->ngx_create_texture(p->context, dst_w, dst_h);
                if (p->vsr_output_d3d) {
                    p->vsr_output_pl = pl_d3d11_wrap(gpu, pl_d3d11_wrap_params(
                        .tex = (ID3D11Resource *)p->vsr_output_d3d,
                        .w = dst_w,
                        .h = dst_h,
                    ));
                    p->vsr_output_w = dst_w;
                    p->vsr_output_h = dst_h;
                }

                if (!p->vsr_output_pl) {
                    MP_WARN(ctx, "NGX VSR: Failed to create output texture.\n");
                    use_ngx_vsr = false;
                }
            }
        }

        if (use_ngx_vsr) {
            struct pl_frame intermediate_target = {
                .repr = pl_color_repr_rgb,
                .num_planes = 1,
                .planes[0] = {
                    .texture = p->vsr_intermediate_pl,
                    .components = p->vsr_intermediate_pl->params.format->num_components,
                    .component_mapping = {0, 1, 2, 3},
                },
                .color = pl_color_space_srgb,
                .crop = { .x0 = 0, .y0 = 0, .x1 = src_w, .y1 = src_h },
            };

            struct pl_render_params no_osd_params = rparams;
            if (!pl_render_image_mix(p->rr, &mix, &intermediate_target, &no_osd_params)) {
                MP_ERR(ctx, "NGX VSR: Failed rendering to intermediate texture!\n");
                goto done;
            }
            pl_gpu_flush(gpu);

            bool vsr_ok = p->context->fns->ngx_vsr_process(
                p->context,
                p->vsr_intermediate_d3d, src_w, src_h,
                p->vsr_output_d3d, dst_w, dst_h,
                ngx_quality);

            if (!vsr_ok) {
                MP_WARN(ctx, "NGX VSR: Evaluate failed, falling back.\n");
                use_ngx_vsr = false;
            } else {
                vsr_done = true;
                vsr_dst_w = dst_w;
                vsr_dst_h = dst_h;
            }
        }
    }

    // --- AMD FSR stage: spatial upscaling (any D3D11 GPU) ---
#if HAVE_D3D11 && defined(PL_HAVE_D3D11)
    int amd_fsr_mode = p->next_opts->amd_fsr;
    bool use_fsr = !vsr_done && amd_fsr_mode > 0 &&
                   p->context->fns->fsr_available &&
                   p->context->fns->fsr_available(p->context) &&
                   frame->current && mix.num_frames > 0;

    if (use_fsr) {
        struct pl_frame *first_frame = (struct pl_frame *) mix.frames[0];
        struct mp_image *src_mpi = first_frame->user_data;
        int src_w = src_mpi->params.w;
        int src_h = src_mpi->params.h;

        int dst_w = p->dst.x1 - p->dst.x0;
        int dst_h = p->dst.y1 - p->dst.y0;
        if (dst_w <= 0 || dst_h <= 0) {
            dst_w = fbo_w;
            dst_h = fbo_h;
        }

        if (src_w >= dst_w && src_h >= dst_h) {
            MP_DBG(ctx, "FSR: Source %dx%d >= dst %dx%d, skipping.\n",
                   src_w, src_h, dst_w, dst_h);
            use_fsr = false;
        }

        if (use_fsr) {
            MP_DBG(ctx, "FSR: Two-stage render %dx%d -> %dx%d (mode=%d)\n",
                   src_w, src_h, dst_w, dst_h, amd_fsr_mode);

            // Ensure intermediate texture at source resolution
            if (!p->fsr_intermediate_d3d ||
                p->fsr_intermediate_w != src_w || p->fsr_intermediate_h != src_h)
            {
                if (p->fsr_intermediate_pl)
                    pl_tex_destroy(gpu, &p->fsr_intermediate_pl);
                if (p->fsr_intermediate_d3d) {
                    ID3D11Texture2D_Release(p->fsr_intermediate_d3d);
                    p->fsr_intermediate_d3d = NULL;
                }

                p->fsr_intermediate_d3d =
                    p->context->fns->fsr_create_texture(p->context, src_w, src_h);
                if (p->fsr_intermediate_d3d) {
                    p->fsr_intermediate_pl = pl_d3d11_wrap(gpu, pl_d3d11_wrap_params(
                        .tex = (ID3D11Resource *)p->fsr_intermediate_d3d,
                        .w = src_w,
                        .h = src_h,
                    ));
                    p->fsr_intermediate_w = src_w;
                    p->fsr_intermediate_h = src_h;
                }

                if (!p->fsr_intermediate_pl) {
                    MP_WARN(ctx, "FSR: Failed to create intermediate texture.\n");
                    use_fsr = false;
                }
            }

            // Ensure output texture at destination resolution
            if (use_fsr &&
                (!p->fsr_output_d3d ||
                 p->fsr_output_w != dst_w || p->fsr_output_h != dst_h))
            {
                if (p->fsr_output_pl)
                    pl_tex_destroy(gpu, &p->fsr_output_pl);
                if (p->fsr_output_d3d) {
                    ID3D11Texture2D_Release(p->fsr_output_d3d);
                    p->fsr_output_d3d = NULL;
                }

                p->fsr_output_d3d =
                    p->context->fns->fsr_create_texture(p->context, dst_w, dst_h);
                if (p->fsr_output_d3d) {
                    p->fsr_output_pl = pl_d3d11_wrap(gpu, pl_d3d11_wrap_params(
                        .tex = (ID3D11Resource *)p->fsr_output_d3d,
                        .w = dst_w,
                        .h = dst_h,
                    ));
                    p->fsr_output_w = dst_w;
                    p->fsr_output_h = dst_h;
                }

                if (!p->fsr_output_pl) {
                    MP_WARN(ctx, "FSR: Failed to create output texture.\n");
                    use_fsr = false;
                }
            }
        }

        if (use_fsr) {
            struct pl_frame intermediate_target = {
                .repr = pl_color_repr_rgb,
                .num_planes = 1,
                .planes[0] = {
                    .texture = p->fsr_intermediate_pl,
                    .components = p->fsr_intermediate_pl->params.format->num_components,
                    .component_mapping = {0, 1, 2, 3},
                },
                .color = pl_color_space_srgb,
                .crop = { .x0 = 0, .y0 = 0, .x1 = src_w, .y1 = src_h },
            };

            struct pl_render_params no_osd_params = rparams;
            if (!pl_render_image_mix(p->rr, &mix, &intermediate_target, &no_osd_params)) {
                MP_ERR(ctx, "FSR: Failed rendering to intermediate texture!\n");
                goto done;
            }
            pl_gpu_flush(gpu);

            bool fsr_ok = p->context->fns->fsr_process(
                p->context,
                p->fsr_intermediate_d3d, src_w, src_h,
                p->fsr_output_d3d, dst_w, dst_h,
                amd_fsr_mode);

            if (!fsr_ok) {
                MP_WARN(ctx, "FSR: Processing failed, falling back.\n");
                use_fsr = false;
            } else {
                vsr_done = true;
                vsr_dst_w = dst_w;
                vsr_dst_h = dst_h;
            }
        }
    }
#endif // HAVE_D3D11 && PL_HAVE_D3D11

    // --- TrueHDR stage: SDR → HDR conversion ---
    if (use_ngx_truehdr) {
        int hdr_w = p->dst.x1 - p->dst.x0;
        int hdr_h = p->dst.y1 - p->dst.y0;
        if (hdr_w <= 0 || hdr_h <= 0) {
            hdr_w = fbo_w;
            hdr_h = fbo_h;
        }

        MP_DBG(ctx, "NGX TrueHDR: Processing %dx%d (preset=%d)\n",
               hdr_w, hdr_h, nvidia_truehdr_preset);

        // Ensure TrueHDR textures match dimensions
        if (!p->truehdr_input_d3d ||
            p->truehdr_w != hdr_w || p->truehdr_h != hdr_h)
        {
            if (p->truehdr_input_pl)
                pl_tex_destroy(gpu, &p->truehdr_input_pl);
            if (p->truehdr_input_d3d) {
                ID3D11Texture2D_Release(p->truehdr_input_d3d);
                p->truehdr_input_d3d = NULL;
            }
            if (p->truehdr_output_pl)
                pl_tex_destroy(gpu, &p->truehdr_output_pl);
            if (p->truehdr_output_d3d) {
                ID3D11Texture2D_Release(p->truehdr_output_d3d);
                p->truehdr_output_d3d = NULL;
            }

            p->truehdr_input_d3d =
                p->context->fns->ngx_create_texture(p->context, hdr_w, hdr_h);
            if (p->truehdr_input_d3d) {
                p->truehdr_input_pl = pl_d3d11_wrap(gpu, pl_d3d11_wrap_params(
                    .tex = (ID3D11Resource *)p->truehdr_input_d3d,
                    .w = hdr_w,
                    .h = hdr_h,
                ));
            }

            if (p->truehdr_input_pl) {
                p->truehdr_output_d3d =
                    p->context->fns->ngx_create_hdr_texture(p->context, hdr_w, hdr_h);
                if (p->truehdr_output_d3d) {
                    p->truehdr_output_pl = pl_d3d11_wrap(gpu, pl_d3d11_wrap_params(
                        .tex = (ID3D11Resource *)p->truehdr_output_d3d,
                        .w = hdr_w,
                        .h = hdr_h,
                    ));
                }
            }

            if (!p->truehdr_input_pl || !p->truehdr_output_pl) {
                MP_WARN(ctx, "NGX TrueHDR: Failed to create textures.\n");
                use_ngx_truehdr = false;
            } else {
                p->truehdr_w = hdr_w;
                p->truehdr_h = hdr_h;
            }
        }

        if (use_ngx_truehdr) {
            // Determine SDR input: VSR output (mode B) or render from scratch (mode A)
            ID3D11Texture2D *sdr_input_d3d = NULL;
            bool need_sdr_render = true;

            if (vsr_done && p->vsr_output_pl &&
                p->vsr_output_w == hdr_w && p->vsr_output_h == hdr_h)
            {
                // Mode B: chain VSR → TrueHDR (VSR output is RGBA8 SDR at dst resolution)
                sdr_input_d3d = p->vsr_output_d3d;
                need_sdr_render = false;
                MP_DBG(ctx, "NGX TrueHDR: Using VSR output as SDR input (mode B).\n");
            }
#if HAVE_D3D11 && defined(PL_HAVE_D3D11)
            else if (vsr_done && p->fsr_output_pl &&
                     p->fsr_output_w == hdr_w && p->fsr_output_h == hdr_h)
            {
                // Mode B (FSR): chain FSR → TrueHDR
                sdr_input_d3d = p->fsr_output_d3d;
                need_sdr_render = false;
                MP_DBG(ctx, "NGX TrueHDR: Using FSR output as SDR input (mode B).\n");
            }
#endif

            if (need_sdr_render) {
                // Mode A: Render video to SDR RGBA8 input texture
                struct pl_frame truehdr_sdr_target = {
                    .repr = pl_color_repr_rgb,
                    .num_planes = 1,
                    .planes[0] = {
                        .texture = p->truehdr_input_pl,
                        .components = p->truehdr_input_pl->params.format->num_components,
                        .component_mapping = {0, 1, 2, 3},
                    },
                    .color = pl_color_space_srgb,
                    .crop = { .x0 = 0, .y0 = 0, .x1 = hdr_w, .y1 = hdr_h },
                };

                struct pl_render_params no_osd_params = rparams;
                if (!pl_render_image_mix(p->rr, &mix, &truehdr_sdr_target, &no_osd_params)) {
                    MP_ERR(ctx, "NGX TrueHDR: Failed rendering to SDR input texture!\n");
                    goto done;
                }
                pl_gpu_flush(gpu);

                sdr_input_d3d = p->truehdr_input_d3d;
            }

            bool truehdr_ok = p->context->fns->ngx_truehdr_process(
                p->context,
                sdr_input_d3d, hdr_w, hdr_h,
                p->truehdr_output_d3d, hdr_w, hdr_h,
                nvidia_truehdr_preset, 0);

            if (!truehdr_ok) {
                MP_WARN(ctx, "NGX TrueHDR: Evaluate failed, falling back.\n");
                use_ngx_truehdr = false;
                // If VSR succeeded alone, still output that below
            } else {
                // Render TrueHDR FP16 output to FBO via pl_render_image.
                // Set hdr_image.color = target.color for passthrough (the SDK
                // already produced final scRGB-linear HDR pixels).
                pl_tex_clear(gpu, fbo, (float[4]){ 0.0, 0.0, 0.0, 1.0 });

                struct pl_frame hdr_image = {
                    .repr = pl_color_repr_rgb,
                    .num_planes = 1,
                    .planes[0] = {
                        .texture = p->truehdr_output_pl,
                        .components = p->truehdr_output_pl->params.format->num_components,
                        .component_mapping = {0, 1, 2, 3},
                    },
                    .color = target.color,
                    .crop = { .x0 = 0, .y0 = 0, .x1 = hdr_w, .y1 = hdr_h },
                };

                struct pl_render_params hdr_params = rparams;
                hdr_params.background = PL_CLEAR_SKIP;
                hdr_params.border = PL_CLEAR_SKIP;
                pl_render_image(p->rr, &hdr_image, &target, &hdr_params);

                valid = true;
                goto done;
            }
        }
    }

    // --- VSR-only output (no TrueHDR, or TrueHDR failed) ---
    if (vsr_done && !use_ngx_truehdr) {
        pl_tex_clear(gpu, fbo, (float[4]){ 0.0, 0.0, 0.0, 1.0 });

        // Use pl_render_image instead of pl_tex_blit to handle format
        // differences (e.g. RGBA8 VSR output → FP16 HDR FBO) and
        // composite OSD overlays in a single pass.
        struct pl_frame vsr_image = {
            .repr = pl_color_repr_rgb,
            .num_planes = 1,
            .planes[0] = {
                .texture = p->vsr_output_pl,
                .components = p->vsr_output_pl->params.format->num_components,
                .component_mapping = {0, 1, 2, 3},
            },
            .color = pl_color_space_srgb,
            .crop = { .x0 = 0, .y0 = 0,
                      .x1 = vsr_dst_w, .y1 = vsr_dst_h },
        };

        struct pl_render_params vsr_params = rparams;
        vsr_params.background = PL_CLEAR_SKIP;
        vsr_params.border = PL_CLEAR_SKIP;
        pl_render_image(p->rr, &vsr_image, &target, &vsr_params);

        valid = true;
        goto done;
    }
#endif // HAVE_D3D11 && PL_HAVE_D3D11 && HAVE_NGX_VSR

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
    struct priv *p = ctx->priv;
    pl_gpu gpu = p->gpu;
    pl_tex fbo = NULL;
    args->res = NULL;

    if (!frame || !frame->current)
        return;

    // Update options
    m_config_cache_update(p->opts_cache);

    pl_options pars = p->pars;
    struct pl_render_params params = pars->params;
    params.info_callback = NULL;
    params.skip_caching_single_frame = true;
    params.preserve_mixing_cache = false;
    params.frame_mixer = NULL;

    struct pl_peak_detect_params peak_params;
    if (params.peak_detect_params) {
        peak_params = *params.peak_detect_params;
        params.peak_detect_params = &peak_params;
        peak_params.allow_delayed = false;
    }

    // Get the current frame from queue
    struct pl_frame_mix mix;
    struct pl_queue_params qparams = *pl_queue_params(
        .pts = p->last_pts,
    );
#if PL_API_VER >= 340
    qparams.drift_compensation = 0;
#endif
    enum pl_queue_status status = pl_queue_update(p->queue, &mix, &qparams);
    if (status == PL_QUEUE_ERR || !mix.num_frames) {
        MP_ERR(ctx, "No frames available for screenshot.\n");
        return;
    }

    struct pl_frame image = *(struct pl_frame *) mix.frames[0];
    struct mp_image *mpi = image.user_data;
    struct mp_rect src = p->src, dst = p->dst;
    struct mp_osd_res osd = p->osd_res;

    // Check if VSR output is available for this screenshot
    bool use_vsr_output = false;
#if HAVE_D3D11 && defined(PL_HAVE_D3D11) && HAVE_NGX_VSR
    if (p->vsr_output_pl && p->vsr_output_w > 0 && p->vsr_output_h > 0)
        use_vsr_output = true;
#endif
#if HAVE_D3D11 && defined(PL_HAVE_D3D11)
    if (!use_vsr_output && p->fsr_output_pl &&
        p->fsr_output_w > 0 && p->fsr_output_h > 0)
        use_vsr_output = true;
#endif

    if (!args->scaled) {
        int w, h;

        if (use_vsr_output) {
            // Use upscale output dimensions for unscaled screenshot
            w = 0; h = 0;
#if HAVE_D3D11 && defined(PL_HAVE_D3D11) && HAVE_NGX_VSR
            if (p->vsr_output_w > 0) {
                w = p->vsr_output_w;
                h = p->vsr_output_h;
            }
#endif
#if HAVE_D3D11 && defined(PL_HAVE_D3D11)
            if (w == 0 && p->fsr_output_w > 0) {
                w = p->fsr_output_w;
                h = p->fsr_output_h;
            }
#endif
        } else {
            mp_image_params_get_dsize(&mpi->params, &w, &h);
        }

        if (w < 1 || h < 1)
            return;

        if (!use_vsr_output) {
            int src_w = mpi->params.w;
            int src_h = mpi->params.h;
            src = (struct mp_rect) {0, 0, src_w, src_h};
            dst = (struct mp_rect) {0, 0, w, h};

            if (mp_image_crop_valid(&mpi->params))
                src = mpi->params.crop;

            if (mpi->params.rotate % 180 == 90) {
                MPSWAP(int, w, h);
                MPSWAP(int, src_w, src_h);
            }
            mp_rect_rotate(&src, src_w, src_h, mpi->params.rotate);
            mp_rect_rotate(&dst, w, h, mpi->params.rotate);
        } else {
            src = (struct mp_rect) {0, 0, w, h};
            dst = (struct mp_rect) {0, 0, w, h};
        }

        osd = (struct mp_osd_res) {
            .display_par = 1.0,
            .w = mp_rect_w(dst),
            .h = mp_rect_h(dst),
        };
    }

    // Create offscreen FBO, try high bit depth first
    int mpfmt;
    for (int depth = args->high_bit_depth ? 16 : 8; depth; depth -= 8) {
        mpfmt = (depth == 16) ? IMGFMT_RGBA64 : IMGFMT_RGBA;
        pl_fmt fmt = pl_find_fmt(gpu, PL_FMT_UNORM, 4, depth, depth,
                                 PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE);
        if (!fmt)
            continue;

        fbo = pl_tex_create(gpu, pl_tex_params(
            .w = osd.w,
            .h = osd.h,
            .format = fmt,
            .blit_dst = true,
            .renderable = true,
            .host_readable = true,
            .storable = fmt->caps & PL_FMT_CAP_STORABLE,
        ));
        if (fbo)
            break;
    }

    if (!fbo) {
        MP_ERR(ctx, "Failed creating target FBO for screenshot.\n");
        return;
    }

    // Build target frame with sRGB color space for correct tone mapping
    struct pl_frame target = {
        .repr = pl_color_repr_rgb,
        .num_planes = 1,
        .planes[0] = {
            .texture = fbo,
            .components = 4,
            .component_mapping = {0, 1, 2, 3},
        },
    };

    if (args->native_csp) {
        target.color = image.color;
    } else {
        target.color = pl_color_space_srgb;
    }

    apply_crop(&image, src, mpi->params.w, mpi->params.h);
    apply_crop(&target, dst, fbo->params.w, fbo->params.h);

#if HAVE_D3D11 && defined(PL_HAVE_D3D11)
    // If upscale output (VSR or FSR) is available, use it as the image source
    // instead of the original decoded frame. This preserves the upscale effect
    // in screenshots by feeding the cached texture through pl_render_image.
    if (use_vsr_output) {
        pl_tex vsr_tex = NULL;
        int vsr_w = 0, vsr_h = 0;
#if HAVE_NGX_VSR
        if (p->vsr_output_pl && p->vsr_output_w > 0) {
            vsr_tex = p->vsr_output_pl;
            vsr_w = p->vsr_output_w;
            vsr_h = p->vsr_output_h;
        }
#endif
        if (!vsr_tex && p->fsr_output_pl && p->fsr_output_w > 0) {
            vsr_tex = p->fsr_output_pl;
            vsr_w = p->fsr_output_w;
            vsr_h = p->fsr_output_h;
        }
        if (vsr_tex) {
            image = (struct pl_frame){
                .repr = pl_color_repr_rgb,
                .num_planes = 1,
                .planes[0] = {
                    .texture = vsr_tex,
                    .components = vsr_tex->params.format->num_components,
                    .component_mapping = {0, 1, 2, 3},
                },
                .color = pl_color_space_srgb,
                .crop = { .x0 = 0, .y0 = 0,
                          .x1 = vsr_w, .y1 = vsr_h },
            };
            target.color = pl_color_space_srgb;
        }
    }
#endif

    // Render OSD/subtitle overlays
    if (p->osd && (args->subs || args->osd)) {
        double pts = mpi->pts;
        update_overlays(ctx, p->osd, osd, pts, &p->osd_overlay, &target);
    }

    if (!pl_render_image(p->rr, &image, &target, &params)) {
        MP_ERR(ctx, "Failed rendering screenshot frame.\n");
        goto done;
    }

    args->res = mp_image_alloc(mpfmt, fbo->params.w, fbo->params.h);
    if (!args->res)
        goto done;

    args->res->params.color.primaries = target.color.primaries;
    args->res->params.color.transfer = target.color.transfer;
    args->res->params.repr.levels = target.repr.levels;
    args->res->params.color.hdr = target.color.hdr;
    if (args->scaled)
        args->res->params.p_w = args->res->params.p_h = 1;

    bool ok = pl_tex_download(gpu, pl_tex_transfer_params(
        .tex = fbo,
        .ptr = args->res->planes[0],
        .row_pitch = args->res->stride[0],
    ));

    if (!ok)
        TA_FREEP(&args->res);

done:
    pl_tex_destroy(gpu, &fbo);
}

static inline void copy_frame_info_to_mp(struct frame_info *pl,
                                         struct mp_frame_perf *mp)
{
    mp_assert(pl->count <= VO_PASS_PERF_MAX);
    mp->count = MPMIN(pl->count, VO_PASS_PERF_MAX);

    for (int i = 0; i < mp->count; ++i) {
        const struct pl_dispatch_info *pass = &pl->info[i];

        mp_assert(pass->num_samples <= MP_ARRAY_SIZE(pass->samples));

        struct mp_pass_perf *perf = &mp->perf[i];
        perf->count = MPMIN(pass->num_samples, VO_PERF_SAMPLE_COUNT);
        memcpy(perf->samples, pass->samples, perf->count * sizeof(pass->samples[0]));
        perf->last = pass->last;
        perf->peak = pass->peak;
        perf->avg = pass->average;

        strncpy(mp->desc[i], pass->shader->description, sizeof(mp->desc[i]) - 1);
        mp->desc[i][sizeof(mp->desc[i]) - 1] = '\0';
    }
}

static void perfdata(struct render_backend *ctx,
                     struct voctrl_performance_data *out)
{
    struct priv *p = ctx->priv;
    *out = (struct voctrl_performance_data){0};
    copy_frame_info_to_mp(&p->perf_fresh, &out->fresh);
    copy_frame_info_to_mp(&p->perf_redraw, &out->redraw);
}

static void destroy(struct render_backend *ctx)
{
    struct priv *p = ctx->priv;
    if (!p)
        return;

    pl_queue_destroy(&p->queue);

    // Free OSD textures
    for (int i = 0; i < MP_ARRAY_SIZE(p->osd_overlay.entries); i++)
        pl_tex_destroy(p->gpu, &p->osd_overlay.entries[i].tex);
    for (int i = 0; i < p->num_sub_tex; i++)
        pl_tex_destroy(p->gpu, &p->sub_tex[i]);

#if HAVE_D3D11 && defined(PL_HAVE_D3D11) && HAVE_NGX_VSR
    // Free VSR cached textures
    if (p->vsr_intermediate_pl)
        pl_tex_destroy(p->gpu, &p->vsr_intermediate_pl);
    if (p->vsr_intermediate_d3d) {
        ID3D11Texture2D_Release(p->vsr_intermediate_d3d);
        p->vsr_intermediate_d3d = NULL;
    }
    if (p->vsr_output_pl)
        pl_tex_destroy(p->gpu, &p->vsr_output_pl);
    if (p->vsr_output_d3d) {
        ID3D11Texture2D_Release(p->vsr_output_d3d);
        p->vsr_output_d3d = NULL;
    }

    // Free FSR cached textures
    if (p->fsr_intermediate_pl)
        pl_tex_destroy(p->gpu, &p->fsr_intermediate_pl);
    if (p->fsr_intermediate_d3d) {
        ID3D11Texture2D_Release(p->fsr_intermediate_d3d);
        p->fsr_intermediate_d3d = NULL;
    }
    if (p->fsr_output_pl)
        pl_tex_destroy(p->gpu, &p->fsr_output_pl);
    if (p->fsr_output_d3d) {
        ID3D11Texture2D_Release(p->fsr_output_d3d);
        p->fsr_output_d3d = NULL;
    }

    // Free TrueHDR cached textures
    if (p->truehdr_input_pl)
        pl_tex_destroy(p->gpu, &p->truehdr_input_pl);
    if (p->truehdr_input_d3d) {
        ID3D11Texture2D_Release(p->truehdr_input_d3d);
        p->truehdr_input_d3d = NULL;
    }
    if (p->truehdr_output_pl)
        pl_tex_destroy(p->gpu, &p->truehdr_output_pl);
    if (p->truehdr_output_d3d) {
        ID3D11Texture2D_Release(p->truehdr_output_d3d);
        p->truehdr_output_d3d = NULL;
    }
#endif

    pl_renderer_destroy(&p->rr);

    for (int i = 0; i < VO_PASS_PERF_MAX; ++i) {
        pl_shader_info_deref(&p->perf_fresh.info[i].shader);
        pl_shader_info_deref(&p->perf_redraw.info[i].shader);
    }

    pl_options_free(&p->pars);

    hwdec_devices_destroy(ctx->hwdec_devs);

    if (p->context) {
        p->context->fns->destroy(p->context);
        talloc_free(p->context->priv);
        talloc_free(p->context);
    }
}

static void get_vsr_capabilities(struct render_backend *ctx,
                                  struct mpv_vsr_capabilities *out)
{
    struct priv *p = ctx->priv;
    (void)p;
#if HAVE_D3D11 && defined(PL_HAVE_D3D11) && HAVE_NGX_VSR
    if (p->context && p->context->fns->ngx_vsr_available)
        out->nvidia_vsr = p->context->fns->ngx_vsr_available(p->context) ? 1 : 0;
#endif
#if HAVE_D3D11 && defined(PL_HAVE_D3D11)
    if (p->context && p->context->fns->fsr_available)
        out->amd_vsr = p->context->fns->fsr_available(p->context) ? 1 : 0;
#endif
    MP_VERBOSE(ctx, "VSR capabilities: nvidia=%d, amd=%d\n",
               out->nvidia_vsr, out->amd_vsr);
}

static void get_vsr_output_size(struct render_backend *ctx, int *w, int *h)
{
    *w = 0;
    *h = 0;
#if HAVE_D3D11 && defined(PL_HAVE_D3D11) && HAVE_NGX_VSR
    struct priv *p = ctx->priv;
    if (p->vsr_output_pl && p->vsr_output_w > 0 && p->vsr_output_h > 0) {
        *w = p->vsr_output_w;
        *h = p->vsr_output_h;
    }
#endif
#if HAVE_D3D11 && defined(PL_HAVE_D3D11)
    {
#if !(HAVE_NGX_VSR)
        struct priv *p = ctx->priv;
#endif
        if (*w == 0 && *h == 0 &&
            p->fsr_output_pl && p->fsr_output_w > 0 && p->fsr_output_h > 0) {
            *w = p->fsr_output_w;
            *h = p->fsr_output_h;
        }
    }
#endif
}

static void get_truehdr_capabilities(struct render_backend *ctx,
                                      struct mpv_truehdr_capabilities *out)
{
    struct priv *p = ctx->priv;
    (void)p;
#if HAVE_D3D11 && defined(PL_HAVE_D3D11) && HAVE_NGX_VSR
    if (p->context && p->context->fns->ngx_truehdr_available)
        out->nvidia_truehdr = p->context->fns->ngx_truehdr_available(p->context) ? 1 : 0;
#endif
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
    .get_vsr_capabilities = get_vsr_capabilities,
    .get_vsr_output_size = get_vsr_output_size,
    .get_truehdr_capabilities = get_truehdr_capabilities,
    .destroy = destroy,
};
