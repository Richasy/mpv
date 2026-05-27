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
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <assert.h>
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>

#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>

#include "common/common.h"
#include "common/tags.h"
#include "osdep/timer.h"
#include "osdep/windows_utils.h"
#include "filters/f_autoconvert.h"
#include "filters/filter.h"
#include "filters/filter_internal.h"
#include "filters/user_filters.h"
#include "player/core.h"
#include "refqueue.h"
#include "video/hwdec.h"
#include "video/mp_image.h"
#include "video/mp_image_pool.h"
#include "video/out/gpu/d3d11_helpers.h"
#include "video/out/vo.h"

// For video processor extensions identifiers reference see:
// https://chromium.googlesource.com/chromium/src/+/5f354f38/ui/gl/swap_chain_presenter.cc

#ifndef NVIDIA_PPE_INTERFACE_GUID
DEFINE_GUID(NVIDIA_PPE_INTERFACE_GUID,
            0xd43ce1b3, 0x1f4b, 0x48ac, 0xba, 0xee,
            0xc3, 0xc2, 0x53, 0x75, 0xe6, 0xf7);
#endif

#ifndef NVIDIA_TRUE_HDR_INTERFACE_GUID
DEFINE_GUID(NVIDIA_TRUE_HDR_INTERFACE_GUID,
            0xfdd62bb4, 0x620b, 0x4fd7, 0x9a, 0xb3,
            0x1e, 0x59, 0xd0, 0xd5, 0x44, 0xb3);
#endif

#ifndef INTEL_VPE_INTERFACE_GUID
DEFINE_GUID(INTEL_VPE_INTERFACE_GUID,
            0xedd1d4b9, 0x8659, 0x4cbc, 0xa4, 0xd6,
            0x98, 0x31, 0xa2, 0x16, 0x3a, 0xc3);
#endif

static const unsigned int intel_vpe_fn_version = 0x1;
static const unsigned int intel_vpe_version    = 0x3;

static const unsigned int intel_vpe_fn_scaling  = 0x37;
static const unsigned int intel_vpe_scaling_vsr = 0x2;

static const unsigned int intel_vpe_fn_mode      = 0x20;
static const unsigned int intel_vpe_mode_preproc = 0x1;

static const struct m_opt_choice_alternatives d3d11vpp_processor_caps[] = {
    {"blend", D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_DEINTERLACE_BLEND},
    {"bob", D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_DEINTERLACE_BOB},
    {"adaptive", D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_DEINTERLACE_ADAPTIVE},
    {"mocomp", D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_DEINTERLACE_MOTION_COMPENSATION},
    {"ivtc", D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_INVERSE_TELECINE},
    {"none", 0},
    {0}
};

enum scaling_mode {
    SCALING_BASIC,
    SCALING_INTEL_VSR,
    SCALING_NVIDIA_RTX,
};

struct opts {
    bool deint_enabled;
    float scale;
    int scaling_mode;
    bool interlaced_only;
    int mode;
    int field_parity;
    int format;
    bool nvidia_true_hdr;
};

struct priv {
    struct opts *opts;

    ID3D11Device *vo_dev;

    ID3D11DeviceContext *device_ctx;
    ID3D11VideoDevice *video_dev;
    ID3D11VideoContext *video_ctx;

    ID3D11VideoProcessor *video_proc;
    ID3D11VideoProcessorEnumerator *vp_enum;
    D3D11_VIDEO_FRAME_FORMAT d3d_frame_format;

    bool require_filtering;
    bool true_hdr_active;
    bool true_hdr_unsupported;

    struct mp_image_params params, out_params;
    int c_w, c_h;

    AVBufferRef *av_device_ref;
    AVBufferRef *hw_pool;

    struct mp_refqueue *queue;

    UINT num_past_views;
    ID3D11VideoProcessorInputView **past_views;
    UINT num_future_views;
    ID3D11VideoProcessorInputView **future_views;

    UINT output_seq;
};

static void flush_frames(struct mp_filter *vf)
{
    struct priv *p = vf->priv;
    mp_refqueue_flush(p->queue);
    p->output_seq = 0;
}

static void destroy_video_proc(struct mp_filter *vf)
{
    struct priv *p = vf->priv;

    if (p->video_proc)
        ID3D11VideoProcessor_Release(p->video_proc);
    p->video_proc = NULL;

    if (p->vp_enum)
        ID3D11VideoProcessorEnumerator_Release(p->vp_enum);
    p->vp_enum = NULL;
}

static void enable_nvidia_rtx_extension(struct mp_filter *vf)
{
    struct priv *p = vf->priv;

    struct nvidia_ext {
        unsigned int version;
        unsigned int method;
        unsigned int enable;
    } ext = {1, 2, 1};

    HRESULT hr = ID3D11VideoContext_VideoProcessorSetStreamExtension(p->video_ctx,
                                                                     p->video_proc,
                                                                     0,
                                                                     &NVIDIA_PPE_INTERFACE_GUID,
                                                                     sizeof(ext),
                                                                     &ext);

    if (FAILED(hr)) {
        MP_WARN(vf, "Failed to enable NVIDIA RTX Super Resolution: %s\n", mp_HRESULT_to_str(hr));
    } else {
        MP_VERBOSE(vf, "NVIDIA RTX Super Resolution enabled.\n");
    }
}

static bool supports_nvidia_true_hdr(struct mp_filter *vf)
{
    struct priv *p = vf->priv;

    UINT supported = 0;
    HRESULT hr = ID3D11VideoContext_VideoProcessorGetStreamExtension(p->video_ctx,
                                                                     p->video_proc,
                                                                     0,
                                                                     &NVIDIA_TRUE_HDR_INTERFACE_GUID,
                                                                     sizeof(supported),
                                                                     &supported);

    if (FAILED(hr) || !supported) {
        MP_WARN(vf, "NVIDIA RTX Video HDR not supported.\n");
        return false;
    }

    return true;
}

static bool enable_nvidia_true_hdr(struct mp_filter *vf)
{
    struct priv *p = vf->priv;

    if (!supports_nvidia_true_hdr(vf))
        return false;

    struct nvidia_ext {
        unsigned int version;
        unsigned int method;
        unsigned int enable : 1;
        unsigned int reserved : 31;
    } ext = {4, 3, 1};

    HRESULT hr = ID3D11VideoContext_VideoProcessorSetStreamExtension(p->video_ctx,
                                                                     p->video_proc,
                                                                     0,
                                                                     &NVIDIA_TRUE_HDR_INTERFACE_GUID,
                                                                     sizeof(ext),
                                                                     &ext);

    if (FAILED(hr)) {
        MP_WARN(vf, "Failed to enable NVIDIA RTX Video HDR: %s\n", mp_HRESULT_to_str(hr));
        return false;
    }

    MP_VERBOSE(vf, "NVIDIA RTX Video HDR enabled.\n");
    return true;
}

// Returns a stable string describing the current NVIDIA RTX Video HDR state.
// Exposed to clients via the `vf-metadata/<label>/nvidia-true-hdr-status`
// property so the UI can react when the SDR->HDR conversion is automatically
// skipped (see upstream mpv issue #17800). Possible values:
//   "disabled"       - the filter option is not set
//   "active"         - SDR->HDR conversion is currently running
//   "source-is-hdr"  - skipped because the source is already HDR
//   "display-is-sdr" - skipped because the display target is in SDR mode
//   "display-unknown"- skipped because the display target colorspace is
//                      not yet known (e.g. before the first frame is
//                      rendered, or the filter is not attached to a VO)
//   "unsupported"    - skipped because the driver does not support the
//                      RTX Video HDR extension on this GPU
static const char *nvidia_true_hdr_status_str(struct mp_filter *vf)
{
    struct priv *p = vf->priv;

    if (!p->opts->nvidia_true_hdr)
        return "disabled";
    if (p->true_hdr_unsupported)
        return "unsupported";
    if (p->true_hdr_active)
        return "active";

    // Source params haven't been populated yet (no frame has flowed
    // through the filter). We don't know enough about the source yet
    // to classify the skip reason, so report it as unknown.
    if (p->params.color.transfer == PL_COLOR_TRC_UNKNOWN)
        return "display-unknown";

    if (pl_color_transfer_is_hdr(p->params.color.transfer))
        return "source-is-hdr";

    struct mp_stream_info *info = mp_filter_find_stream_info(vf);
    if (!info || !info->dr_vo)
        return "display-unknown";

    struct mp_image_params target = vo_get_target_params(info->dr_vo);
    if (target.color.transfer == PL_COLOR_TRC_UNKNOWN)
        return "display-unknown";
    if (!pl_color_transfer_is_hdr(target.color.transfer))
        return "display-is-sdr";

    // All activation conditions are satisfied but the active flag hasn't
    // been flipped yet (transient state for at most one frame after a
    // display SDR->HDR switch). The next process() call will turn the
    // converter on; report "active" so the UI doesn't flicker through a
    // bogus skip reason.
    return "active";
}

// Decide whether NVIDIA RTX Video HDR should currently be active.
// RTX Video HDR is an SDR-to-HDR inverse tone-mapper run inside the video
// processor; tagging the output as HDR while the GPU/display can't actually
// perform the conversion (because the source is already HDR or because the
// display is in SDR mode) results in a broken picture. This mirrors what
// mpcvr does (see DX11VideoProcessor.cpp `rtxHDR` logic).
static bool should_enable_nvidia_true_hdr_now(struct mp_filter *vf)
{
    struct priv *p = vf->priv;

    if (!p->opts->nvidia_true_hdr)
        return false;

    // The driver was probed once and reported no support for RTX Video HDR.
    if (p->true_hdr_unsupported)
        return false;

    // Don't run an SDR->HDR converter on HDR sources.
    if (pl_color_transfer_is_hdr(p->params.color.transfer))
        return false;

    // We need a VO with a known target colorspace to tell whether the display
    // is currently in HDR mode. Target params get populated by the VO after
    // the first frame is rendered; if they aren't available yet, treat the
    // display as SDR so we don't tag the first frame as HDR on an SDR
    // monitor. The decision is re-evaluated on every subsequent frame.
    struct mp_stream_info *info = mp_filter_find_stream_info(vf);
    if (!info || !info->dr_vo)
        return false;

    struct mp_image_params target = vo_get_target_params(info->dr_vo);

    // The target colorspace is briefly reported as UNKNOWN while the VO
    // tears down and recreates its swap chain in response to our own
    // out_params change (HDR10 <-> SDR). Treating UNKNOWN as "not HDR" here
    // would flip true_hdr_active back to false, force another out_params
    // recompute and swap-chain rebuild, then the next frame sees HDR again
    // and flips it back on -- an active <-> display-unknown oscillation that
    // makes the screen visibly flicker between HDR and SDR several times per
    // second. Once we have committed to a decision and the target was known
    // HDR at that point, stick with it across these transient UNKNOWN
    // windows; only an explicit SDR target should turn us off.
    if (target.color.transfer == PL_COLOR_TRC_UNKNOWN)
        return p->true_hdr_active;

    if (!pl_color_transfer_is_hdr(target.color.transfer))
        return false;

    return true;
}

// Recompute p->out_params and p->require_filtering from p->params, options
// and the current p->true_hdr_active decision. Safe to call multiple times.
static void recompute_out_params(struct mp_filter *vf)
{
    struct priv *p = vf->priv;

    p->out_params = p->params;
    p->out_params.w = (int)(p->opts->scale * p->params.w);
    p->out_params.w += p->out_params.w % 2 != 0;
    p->out_params.h = (int)(p->opts->scale * p->params.h);
    p->out_params.h += p->out_params.h % 2 != 0;
    p->out_params.crop.x0 = lrintf(p->opts->scale * p->out_params.crop.x0);
    p->out_params.crop.x1 = lrintf(p->opts->scale * p->out_params.crop.x1);
    p->out_params.crop.y0 = lrintf(p->opts->scale * p->out_params.crop.y0);
    p->out_params.crop.y1 = lrintf(p->opts->scale * p->out_params.crop.y1);

    if (p->opts->format)
        p->out_params.hw_subfmt = p->opts->format;

    if (p->true_hdr_active) {
        // NVIDIA RTX Video HDR seems to require BT.2020+PQ RGB output.
        p->out_params.color = pl_color_space_hdr10;
        p->out_params.color.hdr.max_luma = 1000;
        if (!p->opts->format)
            p->out_params.hw_subfmt = IMGFMT_X2BGR10;
    }

    p->require_filtering = !mp_image_params_static_equal(&p->params, &p->out_params);
}

static void enable_intel_vsr_extension(struct mp_filter *vf)
{
    struct priv *p = vf->priv;

    struct intel_vpe_ext {
        unsigned int function;
        const void* param;
    } ext;

    ext = (struct intel_vpe_ext){intel_vpe_fn_version, &intel_vpe_version};
    HRESULT hr = ID3D11VideoContext_VideoProcessorSetOutputExtension(p->video_ctx,
                                                                     p->video_proc,
                                                                     &INTEL_VPE_INTERFACE_GUID,
                                                                     sizeof(ext),
                                                                     &ext);
    if (FAILED(hr))
        goto failed;

    ext = (struct intel_vpe_ext){intel_vpe_fn_mode, &intel_vpe_mode_preproc};
    hr = ID3D11VideoContext_VideoProcessorSetOutputExtension(p->video_ctx,
                                                             p->video_proc,
                                                             &INTEL_VPE_INTERFACE_GUID,
                                                             sizeof(ext),
                                                             &ext);
    if (FAILED(hr))
        goto failed;

    ext = (struct intel_vpe_ext){intel_vpe_fn_scaling, &intel_vpe_scaling_vsr};
    hr = ID3D11VideoContext_VideoProcessorSetStreamExtension(p->video_ctx,
                                                             p->video_proc,
                                                             0,
                                                             &INTEL_VPE_INTERFACE_GUID,
                                                             sizeof(ext),
                                                             &ext);
    if (FAILED(hr))
        goto failed;

    MP_VERBOSE(vf, "Intel Video Super Resolution enabled.\n");
    return;

failed:
    MP_WARN(vf, "Failed to enable Intel Video Super Resolution: %s\n", mp_HRESULT_to_str(hr));
}

static int recreate_video_proc(struct mp_filter *vf)
{
    struct priv *p = vf->priv;
    HRESULT hr;

    destroy_video_proc(vf);

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC vpdesc = {
        .InputFrameFormat = p->d3d_frame_format,
        .InputWidth = p->c_w,
        .InputHeight = p->c_h,
        .OutputWidth = p->out_params.w,
        .OutputHeight = p->out_params.h,
    };
    hr = ID3D11VideoDevice_CreateVideoProcessorEnumerator(p->video_dev, &vpdesc,
                                                          &p->vp_enum);
    if (FAILED(hr))
        goto fail;

    int mode = 0;
    if (mp_refqueue_should_deint(p->queue))
        mode = p->opts->mode ? p->opts->mode : D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_DEINTERLACE_ADAPTIVE;

    D3D11_VIDEO_PROCESSOR_RATE_CONVERSION_CAPS rc_caps = {0};
    D3D11_VIDEO_PROCESSOR_CAPS caps;
    hr = ID3D11VideoProcessorEnumerator_GetVideoProcessorCaps(p->vp_enum, &caps);
    if (FAILED(hr))
        goto fail;

    static const char *frame_type[] = {
        [D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE] = "progressive",
        [D3D11_VIDEO_FRAME_FORMAT_INTERLACED_TOP_FIELD_FIRST] = "interlaced tff",
        [D3D11_VIDEO_FRAME_FORMAT_INTERLACED_BOTTOM_FIELD_FIRST] = "interlaced bff",
    };
    mp_require((size_t)p->d3d_frame_format < MP_ARRAY_SIZE(frame_type));
    MP_VERBOSE(vf, "Found %u rate conversion caps for %s frame. Looking for caps=%#x.\n",
               caps.RateConversionCapsCount, frame_type[p->d3d_frame_format], mode);

    int rindex = -1;
    for (UINT n = 0; n < caps.RateConversionCapsCount; n++) {
        D3D11_VIDEO_PROCESSOR_RATE_CONVERSION_CAPS rcaps;
        hr = ID3D11VideoProcessorEnumerator_GetVideoProcessorRateConversionCaps
                (p->vp_enum, n, &rcaps);
        if (FAILED(hr))
            goto fail;
        // Init rc_caps for fallback if no matching mode is found.
        if (n == 0)
            rc_caps = rcaps;
        const char *marker = WHITE_CIRCLE;
        if ((rcaps.ProcessorCaps & mode) == mode) {
            if (rindex < 0) {
                rindex = n;
                rc_caps = rcaps;
                marker = BLACK_CIRCLE;
            }
        }
        MP_VERBOSE(vf, "%s %d: processor_caps=%#x past_frames=%u future_frames=%u "
                       "itelecine_caps=%#x custom_rate_count=%u\n", marker, n,
                   rcaps.ProcessorCaps, rcaps.PastFrames, rcaps.FutureFrames,
                   rcaps.ITelecineCaps, rcaps.CustomRateCount);

        for (UINT c = 0; c < rcaps.CustomRateCount; c++) {
            D3D11_VIDEO_PROCESSOR_CUSTOM_RATE cr;
            if (FAILED(ID3D11VideoProcessorEnumerator_GetVideoProcessorCustomRate(p->vp_enum, n, c, &cr)))
                continue;
            MP_DBG(vf, "\t%u: custom_rate=%u/%u out_frames=%u in_interlaced=%d in_frames_or_fields=%u\n",
                   c, cr.CustomRate.Numerator, cr.CustomRate.Denominator, cr.OutputFrames,
                   cr.InputInterlaced, cr.InputFramesOrFields);
        }
    }

    if (rindex < 0) {
        MP_WARN(vf, "No video processor found matching %s mode, using #0.\n",
                m_opt_choice_str(d3d11vpp_processor_caps, mode));
        rindex = 0;
    }

    hr = ID3D11VideoDevice_CreateVideoProcessor(p->video_dev, p->vp_enum, rindex,
                                                &p->video_proc);
    if (FAILED(hr)) {
        MP_ERR(vf, "Failed to create D3D11 video processor.\n");
        goto fail;
    }

    p->num_past_views = rc_caps.PastFrames;
    if (p->num_past_views)
        MP_TARRAY_GROW(p, p->past_views, p->num_past_views - 1);

    p->num_future_views = rc_caps.FutureFrames;
    if (p->num_future_views)
        MP_TARRAY_GROW(p, p->future_views, p->num_future_views - 1);

    // Force BOB if requested by user, this doesn't really make much sense, but
    // since we have an option, just allow to not use any ref frames.
    if (mode == D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_DEINTERLACE_BOB ||
        mode == D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_DEINTERLACE_BLEND) {
        p->num_past_views = 0;
        p->num_future_views = 0;
        // Warn explicitly here, because forcing unsupported mode will cause
        // obviously wrong result.
        if ((rc_caps.ProcessorCaps & mode) != mode) {
            MP_WARN(vf, "%s mode requested, but not supported. Consider using "
                        "another mode for correct deinterlacing.\n",
                    m_opt_choice_str(d3d11vpp_processor_caps, mode));
        }
    }

    mp_refqueue_set_refs(p->queue, p->num_past_views, p->num_future_views);

    // Note: libavcodec does not support cropping left/top with hwaccel.
    RECT src_rc = {
        .right = p->params.w,
        .bottom = p->params.h,
    };
    ID3D11VideoContext_VideoProcessorSetStreamSourceRect(p->video_ctx,
                                                         p->video_proc,
                                                         0, TRUE, &src_rc);

    // This is supposed to stop drivers from fucking up the video quality.
    ID3D11VideoContext_VideoProcessorSetStreamAutoProcessingMode(p->video_ctx,
                                                                 p->video_proc,
                                                                 0, FALSE);

    bool half_rate = !mp_refqueue_output_fields(p->queue) && mp_refqueue_should_deint(p->queue);
    MP_DBG(vf, "Using %u past and %u future frames for processing at %s rate.\n",
           p->num_past_views, p->num_future_views, half_rate ? "half" : "normal");
    // None of the major GPU vendors seem to support custom rates for proper
    // IVTC frame dropping. Only frame reconstruction is supported.
    ID3D11VideoContext_VideoProcessorSetStreamOutputRate(p->video_ctx,
                                                         p->video_proc,
                                                         0,
                                                         half_rate ?
                                                            D3D11_VIDEO_PROCESSOR_OUTPUT_RATE_HALF :
                                                            D3D11_VIDEO_PROCESSOR_OUTPUT_RATE_NORMAL,
                                                         FALSE, 0);

    if (p->true_hdr_active) {
        if (enable_nvidia_true_hdr(vf)) {
            MP_WARN(vf, "Tagging image output as HDR with max-luma=1000 nits for "
                        "NVIDIA RTX Video HDR. This is only a guess. "
                        "Adjust the value to match NVIDIA settings with "
                        "`--vf-add=format=max-luma=<value>`.\n");
            if (p->opts->format && p->out_params.hw_subfmt != IMGFMT_X2BGR10) {
                MP_WARN(vf, "Requested %s format is not supported for NVIDIA RTX Video HDR. "
                            "Consider using %s instead or leave it unspecified.\n",
                        mp_imgfmt_to_name(p->opts->format),
                        mp_imgfmt_to_name(IMGFMT_X2BGR10));
            }
        } else {
            // Driver doesn't actually support RTX Video HDR. Cache this so we
            // stop trying, and revert out_params so we don't tag the output
            // as HDR while the driver does nothing.
            p->true_hdr_active = false;
            p->true_hdr_unsupported = true;
            recompute_out_params(vf);
        }
    }

    mp_image_params_guess_csp(&p->params);
    mp_image_params_guess_csp(&p->out_params);

    ID3D11VideoContext1 *video_ctx1;
    hr = ID3D11VideoContext_QueryInterface(p->video_ctx, &IID_ID3D11VideoContext1, (void **)&video_ctx1);
    if (SUCCEEDED(hr)) {
        DXGI_COLOR_SPACE_TYPE in = mp_params_to_dxgi_colorspace(vf->log, &p->params);
        DXGI_COLOR_SPACE_TYPE out = mp_params_to_dxgi_colorspace(vf->log, &p->out_params);
        if (in != out)
            MP_VERBOSE(vf, "Converting %s to %s.\n", d3d11_get_csp_name(in), d3d11_get_csp_name(out));
        ID3D11VideoContext1_VideoProcessorSetStreamColorSpace1(video_ctx1,
                                                               p->video_proc,
                                                               0, in);
        ID3D11VideoContext1_VideoProcessorSetOutputColorSpace1(video_ctx1,
                                                               p->video_proc,
                                                               out);
        SAFE_RELEASE(video_ctx1);
    } else {
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE csp = {
            .YCbCr_Matrix = p->params.repr.sys != PL_COLOR_SYSTEM_BT_601,
            .Nominal_Range = p->params.repr.levels == PL_COLOR_LEVELS_LIMITED ? 1 : 2,
        };
        ID3D11VideoContext_VideoProcessorSetStreamColorSpace(p->video_ctx,
                                                            p->video_proc,
                                                            0, &csp);
        ID3D11VideoContext_VideoProcessorSetOutputColorSpace(p->video_ctx,
                                                            p->video_proc,
                                                            &csp);
    }

    switch (p->opts->scaling_mode) {
    case SCALING_INTEL_VSR:
        enable_intel_vsr_extension(vf);
        break;
    case SCALING_NVIDIA_RTX:
        enable_nvidia_rtx_extension(vf);
        break;
    }

    return 0;
fail:
    destroy_video_proc(vf);
    return -1;
}

static struct mp_image *alloc_out(struct mp_filter *vf)
{
    struct priv *p = vf->priv;

    if (!mp_update_av_hw_frames_pool(&p->hw_pool, p->av_device_ref,
                                     IMGFMT_D3D11, p->out_params.hw_subfmt,
                                     p->out_params.w, p->out_params.h, false))
    {
        MP_ERR(vf, "Failed to create hw pool\n");
        return NULL;
    }

    AVFrame *av_frame = av_frame_alloc();
    MP_HANDLE_OOM(av_frame);
    if (av_hwframe_get_buffer(p->hw_pool, av_frame, 0) < 0) {
        MP_ERR(vf, "Failed to allocate frame from hw pool\n");
        av_frame_free(&av_frame);
        return NULL;
    }

    struct mp_image *img = mp_image_from_av_frame(av_frame);
    av_frame_free(&av_frame);
    if (!img) {
        MP_ERR(vf, "Internal error when converting AVFrame\n");
        return NULL;
    }

    return img;
}

static ID3D11VideoProcessorInputView *create_input_view(struct mp_filter *vf,
                                                        struct mp_image *img)
{
    struct priv *p = vf->priv;
    if (!img)
        return NULL;

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC desc = {
        .ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D,
        .Texture2D.ArraySlice = (intptr_t)img->planes[1],
    };
    ID3D11VideoProcessorInputView *view = NULL;
    HRESULT hr = ID3D11VideoDevice_CreateVideoProcessorInputView(
        p->video_dev, (ID3D11Resource *)img->planes[0],
        p->vp_enum, &desc, &view);
    if (FAILED(hr))
        MP_WARN(vf, "Could not create ID3D11VideoProcessorInputView\n");
    return view;
}

static struct mp_image *render(struct mp_filter *vf)
{
    struct priv *p = vf->priv;
    int res = -1;
    HRESULT hr;
    ID3D11VideoProcessorInputView *in_view = NULL;
    ID3D11VideoProcessorOutputView *out_view = NULL;
    struct mp_image *in = NULL, *out = NULL;

    UINT num_past = 0;
    UINT num_future = 0;

    in = mp_refqueue_get(p->queue, 0);
    if (!in)
        goto cleanup;
    ID3D11Texture2D *d3d_tex = (void *)in->planes[0];

    D3D11_VIDEO_FRAME_FORMAT d3d_frame_format;
    if (!mp_refqueue_should_deint(p->queue)) {
        d3d_frame_format = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    } else if (mp_refqueue_top_field_first(p->queue)) {
        d3d_frame_format = D3D11_VIDEO_FRAME_FORMAT_INTERLACED_TOP_FIELD_FIRST;
    } else {
        d3d_frame_format = D3D11_VIDEO_FRAME_FORMAT_INTERLACED_BOTTOM_FIELD_FIRST;
    }

    D3D11_TEXTURE2D_DESC texdesc;
    ID3D11Texture2D_GetDesc(d3d_tex, &texdesc);
    if (!p->video_proc || p->c_w != texdesc.Width || p->c_h != texdesc.Height ||
        p->d3d_frame_format != d3d_frame_format)
    {
        p->c_w = texdesc.Width;
        p->c_h = texdesc.Height;
        p->d3d_frame_format = d3d_frame_format;
        p->output_seq = 0;
        // recreate_video_proc() may probe NVIDIA RTX Video HDR support and,
        // on failure, mutate p->out_params back to non-HDR. Do this before
        // allocating the output frame so we don't tag a frame as HDR (or
        // allocate it as X2BGR10) when the driver actually has no support.
        if (recreate_video_proc(vf) < 0)
            goto cleanup;
    }

    out = alloc_out(vf);
    if (!out) {
        MP_WARN(vf, "failed to allocate frame\n");
        goto cleanup;
    }

    ID3D11Texture2D *d3d_out_tex = (void *)out->planes[0];

    mp_image_copy_attributes(out, in);
    // TODO: sanitize out_params based the processing enabled.
    out->params = p->out_params;

    ID3D11VideoContext_VideoProcessorSetStreamFrameFormat(p->video_ctx,
                                                          p->video_proc,
                                                          0, d3d_frame_format);

    in_view = create_input_view(vf, in);
    if (!in_view)
        goto cleanup;

    for (int i = p->num_past_views; i > 0; i--) {
        ID3D11VideoProcessorInputView *v =
            create_input_view(vf, mp_refqueue_get(p->queue, -i));
        if (v)
            p->past_views[num_past++] = v;
    }

    for (int i = 1; i <= p->num_future_views; i++) {
        ID3D11VideoProcessorInputView *v =
            create_input_view(vf, mp_refqueue_get(p->queue, i));
        if (v)
            p->future_views[num_future++] = v;
    }

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outdesc = {
        .ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D,
    };
    hr = ID3D11VideoDevice_CreateVideoProcessorOutputView(p->video_dev,
                                                          (ID3D11Resource *)d3d_out_tex,
                                                          p->vp_enum, &outdesc,
                                                          &out_view);
    if (FAILED(hr)) {
        MP_ERR(vf, "Could not create ID3D11VideoProcessorOutputView\n");
        goto cleanup;
    }

    bool half_rate = !mp_refqueue_output_fields(p->queue) && mp_refqueue_should_deint(p->queue);
    D3D11_VIDEO_PROCESSOR_STREAM stream = {
        .Enable = TRUE,
        .pInputSurface = in_view,
        .OutputIndex = mp_refqueue_is_second_field(p->queue),
        .InputFrameOrField = p->output_seq * (half_rate ? 2 : 1),
        .PastFrames = num_past,
        .FutureFrames = num_future,
        .ppPastSurfaces = num_past ? p->past_views : NULL,
        .ppFutureSurfaces = num_future ? p->future_views : NULL,
    };
    hr = ID3D11VideoContext_VideoProcessorBlt(p->video_ctx, p->video_proc,
                                              out_view, p->output_seq, 1, &stream);
    if (FAILED(hr)) {
        MP_ERR(vf, "VideoProcessorBlt failed.\n");
        goto cleanup;
    }

    p->output_seq++;
    res = 0;
cleanup:
    SAFE_RELEASE(in_view);
    SAFE_RELEASE(out_view);
    for (int i = 0; i < num_past; i++)
        SAFE_RELEASE(p->past_views[i]);
    for (int i = 0; i < num_future; i++)
        SAFE_RELEASE(p->future_views[i]);
    if (res < 0)
        TA_FREEP(&out);
    return out;
}

static void vf_d3d11vpp_process(struct mp_filter *vf)
{
    struct priv *p = vf->priv;

    struct mp_image *in_fmt = mp_refqueue_execute_reinit(p->queue);
    if (in_fmt) {
        av_buffer_unref(&p->hw_pool);

        destroy_video_proc(vf);

        p->params = in_fmt->params;
        p->true_hdr_active = should_enable_nvidia_true_hdr_now(vf);
        recompute_out_params(vf);
    } else if (p->opts->nvidia_true_hdr) {
        // Re-evaluate every frame so we react to the display switching
        // between HDR and SDR mode (target_params is populated by the VO
        // after the first frame and updated on subsequent draws).
        bool want = should_enable_nvidia_true_hdr_now(vf);
        if (want != p->true_hdr_active) {
            MP_VERBOSE(vf, "NVIDIA RTX Video HDR turned %s.\n", want ? "on" : "off");
            p->true_hdr_active = want;
            recompute_out_params(vf);
            // Force the video processor to be recreated so the driver-side
            // RTX Video HDR state matches our new decision.
            destroy_video_proc(vf);
        }
    }

    if (!mp_refqueue_can_output(p->queue))
        return;

    if (!mp_refqueue_should_deint(p->queue) && !p->require_filtering) {
        // no filtering
        struct mp_image *in = mp_image_new_ref(mp_refqueue_get(p->queue, 0));
        if (!in) {
            mp_filter_internal_mark_failed(vf);
            return;
        }
        mp_refqueue_write_out_pin(p->queue, in);
    } else {
        mp_refqueue_write_out_pin(p->queue, render(vf));
    }
}

static void uninit(struct mp_filter *vf)
{
    struct priv *p = vf->priv;

    destroy_video_proc(vf);

    flush_frames(vf);
    talloc_free(p->queue);
    av_buffer_unref(&p->hw_pool);
    av_buffer_unref(&p->av_device_ref);

    if (p->video_ctx)
        ID3D11VideoContext_Release(p->video_ctx);

    if (p->video_dev)
        ID3D11VideoDevice_Release(p->video_dev);

    if (p->device_ctx)
        ID3D11DeviceContext_Release(p->device_ctx);

    if (p->vo_dev)
        ID3D11Device_Release(p->vo_dev);
}

static bool vf_d3d11vpp_command(struct mp_filter *vf, struct mp_filter_command *cmd)
{
    struct priv *p = vf->priv;

    if (cmd->type != MP_FILTER_COMMAND_GET_META)
        return false;

    struct mp_tags **ptags = cmd->res;
    struct mp_tags *tags = talloc_zero(NULL, struct mp_tags);
    mp_tags_set_str(tags, "nvidia-true-hdr-status",
                    nvidia_true_hdr_status_str(vf));
    mp_tags_set_str(tags, "nvidia-true-hdr-active",
                    p->true_hdr_active ? "yes" : "no");
    mp_tags_set_str(tags, "nvidia-true-hdr-requested",
                    p->opts->nvidia_true_hdr ? "yes" : "no");
    *ptags = tags;
    return true;
}

static const struct mp_filter_info vf_d3d11vpp_filter = {
    .name = "d3d11vpp",
    .process = vf_d3d11vpp_process,
    .command = vf_d3d11vpp_command,
    .reset = flush_frames,
    .destroy = uninit,
    .priv_size = sizeof(struct priv),
};

static struct mp_filter *vf_d3d11vpp_create(struct mp_filter *parent,
                                            void *options)
{
    struct mp_filter *f = mp_filter_create(parent, &vf_d3d11vpp_filter);
    if (!f) {
        talloc_free(options);
        return NULL;
    }

    mp_filter_add_pin(f, MP_PIN_IN, "in");
    mp_filter_add_pin(f, MP_PIN_OUT, "out");

    struct priv *p = f->priv;
    p->opts = talloc_steal(p, options);

    // Special path for vf_d3d11_create_outconv(): disable all processing except
    // possibly surface format conversions.
    if (!p->opts) {
        static const struct opts opts = {0};
        p->opts = (struct opts *)&opts;
    }

    p->queue = mp_refqueue_alloc(f);

    struct mp_stream_info *info = mp_filter_find_stream_info(f);
    if (!info || !info->hwdec_devs)
        goto fail;

    struct hwdec_imgfmt_request params = {
        .imgfmt = IMGFMT_D3D11,
        .probing = false,
    };
    hwdec_devices_request_for_img_fmt(info->hwdec_devs, &params);

    struct mp_hwdec_ctx *hwctx =
        hwdec_devices_get_by_imgfmt_and_type(info->hwdec_devs, IMGFMT_D3D11,
                                             AV_HWDEVICE_TYPE_D3D11VA);
    if (!hwctx || !hwctx->av_device_ref)
        goto fail;
    p->av_device_ref = av_buffer_ref(hwctx->av_device_ref);
    AVHWDeviceContext *avhwctx = (void *)p->av_device_ref->data;
    AVD3D11VADeviceContext *d3dctx = avhwctx->hwctx;

    p->vo_dev = d3dctx->device;
    ID3D11Device_AddRef(p->vo_dev);

    HRESULT hr;

    hr = ID3D11Device_QueryInterface(p->vo_dev, &IID_ID3D11VideoDevice,
                                     (void **)&p->video_dev);
    if (FAILED(hr))
        goto fail;

    ID3D11Device_GetImmediateContext(p->vo_dev, &p->device_ctx);
    if (!p->device_ctx)
        goto fail;
    hr = ID3D11DeviceContext_QueryInterface(p->device_ctx, &IID_ID3D11VideoContext,
                                            (void **)&p->video_ctx);
    if (FAILED(hr))
        goto fail;

    mp_refqueue_add_in_format(p->queue, IMGFMT_D3D11, 0);

    // Force BLEND mode by not sending ref frames and setting half rate.
    // Use half rate for IVTC modes, custom rate would be perfect for that,
    // but it's not supported by any driver in practice.
    bool out_fields = p->opts->mode != D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_DEINTERLACE_BLEND &&
                      p->opts->mode != D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_INVERSE_TELECINE;

    mp_refqueue_set_refs(p->queue, 0, 0);
    mp_refqueue_set_mode(p->queue,
        (p->opts->deint_enabled ? MP_MODE_DEINT : 0) |
        (out_fields ? MP_MODE_OUTPUT_FIELDS : 0) |
        (p->opts->interlaced_only ? MP_MODE_INTERLACED_ONLY : 0));
    mp_refqueue_set_parity(p->queue, p->opts->field_parity);

    return f;

fail:
    talloc_free(f);
    return NULL;
}

#define OPT_BASE_STRUCT struct opts
static const m_option_t vf_opts_fields[] = {
    {"format", OPT_IMAGEFORMAT(format)},
    {"deint", OPT_BOOL(deint_enabled)},
    {"scale", OPT_FLOAT(scale)},
    {"scaling-mode", OPT_CHOICE(scaling_mode,
        {"standard", SCALING_BASIC},
        {"intel", SCALING_INTEL_VSR},
        {"nvidia", SCALING_NVIDIA_RTX})},
    {"interlaced-only", OPT_BOOL(interlaced_only)},
    {"mode", OPT_CHOICE_C(mode, d3d11vpp_processor_caps)},
    {"parity", OPT_CHOICE(field_parity,
        {"tff", MP_FIELD_PARITY_TFF},
        {"bff", MP_FIELD_PARITY_BFF},
        {"auto", MP_FIELD_PARITY_AUTO})},
    {"nvidia-true-hdr", OPT_BOOL(nvidia_true_hdr)},
    {0}
};

const struct mp_user_filter_entry vf_d3d11vpp = {
    .desc = {
        .description = "D3D11 Video Post-Process Filter",
        .name = "d3d11vpp",
        .priv_size = sizeof(OPT_BASE_STRUCT),
        .priv_defaults = &(const OPT_BASE_STRUCT) {
            .deint_enabled = false,
            .scale = 1.0,
            .scaling_mode = SCALING_BASIC,
            .mode = 0,
            .field_parity = MP_FIELD_PARITY_AUTO,
        },
        .options = vf_opts_fields,
    },
    .create = vf_d3d11vpp_create,
};
