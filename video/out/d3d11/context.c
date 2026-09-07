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

#include "common/msg.h"
#include "options/m_config.h"
#include "osdep/io.h"
#include "osdep/timer.h"
#include "osdep/windows_utils.h"
#include "ta/ta_talloc.h"

#include "video/out/gpu/context.h"
#include "video/out/gpu/d3d11_helpers.h"
#include "video/out/gpu/spirv.h"
#include "video/out/w32_common.h"
#include "video/out/win32/displayconfig.h"
#include "context.h"
#include "ra_d3d11.h"

#include <shellscalingapi.h>

#ifdef PL_HAVE_D3D11
#include <libplacebo/d3d11.h>
#endif

#define OPT_BASE_STRUCT struct d3d11_opts
const struct m_sub_options d3d11_conf = {
    .opts = (const struct m_option[]) {
        {"d3d11-warp", OPT_CHOICE(warp,
            {"auto", -1},
            {"no", 0},
            {"yes", 1}),
             .flags = UPDATE_VO,
        },
        {"d3d11-feature-level", OPT_CHOICE(feature_level,
            {"12_1", D3D_FEATURE_LEVEL_12_1},
            {"12_0", D3D_FEATURE_LEVEL_12_0},
            {"11_1", D3D_FEATURE_LEVEL_11_1},
            {"11_0", D3D_FEATURE_LEVEL_11_0},
            {"10_1", D3D_FEATURE_LEVEL_10_1},
            {"10_0", D3D_FEATURE_LEVEL_10_0},
            {"9_3", D3D_FEATURE_LEVEL_9_3},
            {"9_2", D3D_FEATURE_LEVEL_9_2},
            {"9_1", D3D_FEATURE_LEVEL_9_1}),
            .flags = UPDATE_VO,
        },
        {"d3d11-flip", OPT_BOOL(flip), .flags = UPDATE_VO},
        {"d3d11-sync-interval", OPT_INT(sync_interval), M_RANGE(0, 4)},
        {"d3d11-adapter",
            OPT_STRING_VALIDATE(adapter_name, mp_dxgi_validate_adapter),
            .flags = UPDATE_VO,
        },
        {"d3d11-output-format", OPT_CHOICE(output_format,
            {"auto",     DXGI_FORMAT_UNKNOWN},
            {"rgba8",    DXGI_FORMAT_R8G8B8A8_UNORM},
            {"bgra8",    DXGI_FORMAT_B8G8R8A8_UNORM},
            {"rgb10_a2", DXGI_FORMAT_R10G10B10A2_UNORM},
            {"rgba16f",  DXGI_FORMAT_R16G16B16A16_FLOAT}),
            .flags = UPDATE_VO,
        },
        {"d3d11-output-csp", OPT_CHOICE(color_space,
            {"auto", -1},
            {"srgb",    DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709},
            {"linear",  DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709},
            {"pq",      DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020},
            {"bt.2020", DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020}),
            .flags = UPDATE_VO,
        },
        {"d3d11-exclusive-fs", OPT_BOOL(exclusive_fs)},
        {"d3d11-output-mode", OPT_CHOICE(output_mode,
            {"auto", -1},
            {"window", 0},
            {"composition", 1}),
            .flags = UPDATE_VO,
        },
        {0}
    },
    .defaults = &(const struct d3d11_opts) {
        .feature_level = D3D_FEATURE_LEVEL_12_1,
        .warp = -1,
        .flip = true,
        .sync_interval = 1,
        .adapter_name = NULL,
        .output_format = DXGI_FORMAT_UNKNOWN,
        .color_space = -1,
        .output_mode = -1,
    },
    .size = sizeof(struct d3d11_opts)
};

struct priv {
    struct d3d11_opts *opts;
    struct m_config_cache *opts_cache;

    struct mp_vo_opts *vo_opts;
    struct m_config_cache *vo_opts_cache;

    struct ra_tex *backbuffer;
    ID3D11Device *device;
    IDXGISwapChain *swapchain;
    struct mp_d3d11_adapter_info adapter;
    char *adapter_name;
    struct pl_color_space swapchain_csp;

    int64_t perf_freq;
    unsigned last_sync_refresh_count;
    int64_t last_sync_qpc_time;
    int64_t vsync_duration_qpc;
    int64_t last_submit_qpc;
    HRESULT present_result;

    // Composition mode: cache the monitor currently hosting the host HWND so
    // we can notify the core (VO_EVENT_WIN_STATE) when it changes.
    HMONITOR composition_monitor;

    struct mp_dxgi_factory_ctx dxgi_ctx;
};

static struct ra_tex *get_backbuffer(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    ID3D11Texture2D *backbuffer = NULL;
    struct ra_tex *tex = NULL;
    HRESULT hr;

    hr = IDXGISwapChain_GetBuffer(p->swapchain, 0, &IID_ID3D11Texture2D,
                                  (void**)&backbuffer);
    if (FAILED(hr)) {
        MP_ERR(ctx, "Couldn't get swapchain image\n");
        goto done;
    }

    tex = ra_d3d11_wrap_tex(ctx->ra, (ID3D11Resource *)backbuffer);
done:
    SAFE_RELEASE(backbuffer);
    return tex;
}

static bool resize(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    HRESULT hr;

    if (p->backbuffer) {
        MP_ERR(ctx, "Attempt at resizing while a frame was in progress!\n");
        return false;
    }

    hr = IDXGISwapChain_ResizeBuffers(p->swapchain, 0, ctx->vo->dwidth,
        ctx->vo->dheight, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        MP_FATAL(ctx, "Couldn't resize swapchain: %s\n", mp_HRESULT_to_str(hr));
        return false;
    }

    return true;
}

static bool d3d11_reconfig(struct ra_ctx *ctx)
{
    if (ctx->opts.composition) {
        vo_w32_composition_size(ctx->vo);
    } else {
        vo_w32_config(ctx->vo);
    }
    return resize(ctx);
}

static int d3d11_color_depth(struct ra_swapchain *sw)
{
    struct priv *p = sw->priv;

    DXGI_OUTPUT_DESC1 desc1;
    if (!mp_dxgi_output_desc_from_swapchain(&p->dxgi_ctx, p->swapchain, &desc1))
        desc1.BitsPerColor = 0;

    DXGI_SWAP_CHAIN_DESC desc;

    HRESULT hr = IDXGISwapChain_GetDesc(p->swapchain, &desc);
    if (FAILED(hr)) {
        MP_ERR(sw->ctx, "Failed to query swap chain description: %s!\n",
               mp_HRESULT_to_str(hr));
        return desc1.BitsPerColor;
    }

    const struct ra_format *ra_fmt =
        ra_d3d11_get_ra_format(sw->ctx->ra, desc.BufferDesc.Format);
    if (!ra_fmt || !ra_fmt->component_depth[0])
        return desc1.BitsPerColor;

    if (!desc1.BitsPerColor)
        return ra_fmt->component_depth[0];

    return MPMIN(ra_fmt->component_depth[0], desc1.BitsPerColor);
}

static struct pl_color_space d3d11_target_color_space(struct ra_swapchain *sw)
{
    struct priv *p = sw->priv;
    DXGI_OUTPUT_DESC1 desc;

    if (sw->ctx->opts.composition) {
        m_config_cache_update(p->vo_opts_cache);
        HWND hwnd = (HWND)(intptr_t)p->vo_opts->d3d11_composition_hwnd;
        if (hwnd) {
            if (mp_dxgi_output_desc_from_hwnd(&p->dxgi_ctx, hwnd, &desc))
                return mp_dxgi_desc_to_color_space(&desc);
        }
        if (mp_dxgi_output_desc_from_device(p->device, &desc))
            return mp_dxgi_desc_to_color_space(&desc);
        return (struct pl_color_space){0};
    }

    if (mp_dxgi_output_desc_from_hwnd(&p->dxgi_ctx, vo_w32_hwnd(sw->ctx->vo), &desc))
        return mp_dxgi_desc_to_color_space(&desc);

    return (struct pl_color_space){0};
}

static float d3d11_target_ref_luma(struct ra_swapchain *sw)
{
    struct priv *p = sw->priv;
    HWND hwnd;
    if (sw->ctx->opts.composition) {
        m_config_cache_update(p->vo_opts_cache);
        hwnd = (HWND)(intptr_t)p->vo_opts->d3d11_composition_hwnd;
    } else {
        hwnd = vo_w32_hwnd(sw->ctx->vo);
    }
    if (!hwnd)
        return 0;

    return mp_dxgi_sdr_white_level_from_hwnd(&p->dxgi_ctx, hwnd);
}

static bool d3d11_start_frame(struct ra_swapchain *sw, struct ra_fbo *out_fbo)
{
    struct priv *p = sw->priv;

    if (FAILED(p->present_result))
        return false;

    if (!out_fbo)
        return true;

    mp_assert(!p->backbuffer);

    p->backbuffer = get_backbuffer(sw->ctx);
    if (!p->backbuffer)
        return false;

    *out_fbo = (struct ra_fbo) {
        .tex = p->backbuffer,
        .flip = false,
        .color_space = p->swapchain_csp
    };
    return true;
}

static bool d3d11_submit_frame(struct ra_swapchain *sw,
                               const struct vo_frame *frame)
{
    struct priv *p = sw->priv;

    ra_d3d11_flush(sw->ctx->ra);
    ra_tex_free(sw->ctx->ra, &p->backbuffer);
    return true;
}

static int64_t qpc_to_ns(struct ra_swapchain *sw, int64_t qpc)
{
    struct priv *p = sw->priv;

    // Convert QPC units (1/perf_freq seconds) to nanoseconds. This will work
    // without overflow because the QPC value is guaranteed not to roll-over
    // within 100 years, so perf_freq must be less than 2.9*10^9.
    return qpc / p->perf_freq * INT64_C(1000000000) +
        qpc % p->perf_freq * INT64_C(1000000000) / p->perf_freq;
}

static int64_t qpc_ns_now(struct ra_swapchain *sw)
{
    LARGE_INTEGER perf_count;
    QueryPerformanceCounter(&perf_count);
    return qpc_to_ns(sw, perf_count.QuadPart);
}

static void d3d11_swap_buffers(struct ra_swapchain *sw)
{
    struct priv *p = sw->priv;

    if (FAILED(p->present_result))
        return;

    m_config_cache_update(p->opts_cache);

    LARGE_INTEGER perf_count;
    QueryPerformanceCounter(&perf_count);
    p->last_submit_qpc = perf_count.QuadPart;

    HRESULT hr = IDXGISwapChain_Present(p->swapchain, p->opts->sync_interval, 0);
    if (FAILED(hr)) {
        p->present_result = hr;
        MP_FATAL(sw->ctx, "Couldn't present swapchain: %s\n",
                 mp_HRESULT_to_str(hr));
        if (sw->ctx->opts.composition)
            vo_w32_swapchain(sw->ctx->vo, NULL);
    }
}

static void d3d11_get_vsync(struct ra_swapchain *sw, struct vo_vsync_info *info)
{
    struct priv *p = sw->priv;
    HRESULT hr;

    m_config_cache_update(p->opts_cache);

    // The calculations below are only valid if mpv presents on every vsync
    if (p->opts->sync_interval != 1)
        return;

    // They're also only valid for flip model swapchains
    DXGI_SWAP_CHAIN_DESC desc;
    hr = IDXGISwapChain_GetDesc(p->swapchain, &desc);
    if (FAILED(hr) || (desc.SwapEffect != DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL &&
                       desc.SwapEffect != DXGI_SWAP_EFFECT_FLIP_DISCARD))
    {
        return;
    }

    // GetLastPresentCount returns a sequential ID for the frame submitted by
    // the last call to IDXGISwapChain::Present()
    UINT submit_count;
    hr = IDXGISwapChain_GetLastPresentCount(p->swapchain, &submit_count);
    if (FAILED(hr))
        return;

    // GetFrameStatistics returns two pairs. The first is (PresentCount,
    // PresentRefreshCount) which relates a present ID (on the same timeline as
    // GetLastPresentCount) to the physical vsync it was displayed on. The
    // second is (SyncRefreshCount, SyncQPCTime), which relates a physical vsync
    // to a timestamp on the same clock as QueryPerformanceCounter.
    DXGI_FRAME_STATISTICS stats;
    hr = IDXGISwapChain_GetFrameStatistics(p->swapchain, &stats);
    if (hr == DXGI_ERROR_FRAME_STATISTICS_DISJOINT) {
        p->last_sync_refresh_count = 0;
        p->last_sync_qpc_time = 0;
    }
    if (FAILED(hr))
        return;

    // Detecting skipped vsyncs is possible but not supported yet
    info->skipped_vsyncs = -1;

    // Get the number of physical vsyncs that have passed since the last call.
    // Check for 0 here, since sometimes GetFrameStatistics returns S_OK but
    // with 0s in some (all?) members of DXGI_FRAME_STATISTICS.
    unsigned src_passed = 0;
    if (stats.SyncRefreshCount && p->last_sync_refresh_count)
        src_passed = stats.SyncRefreshCount - p->last_sync_refresh_count;
    p->last_sync_refresh_count = stats.SyncRefreshCount;

    // Get the elapsed time passed between the above vsyncs
    unsigned sqt_passed = 0;
    if (stats.SyncQPCTime.QuadPart && p->last_sync_qpc_time)
        sqt_passed = stats.SyncQPCTime.QuadPart - p->last_sync_qpc_time;
    p->last_sync_qpc_time = stats.SyncQPCTime.QuadPart;

    // If any vsyncs have passed, estimate the physical frame rate
    if (src_passed && sqt_passed)
        p->vsync_duration_qpc = sqt_passed / src_passed;
    if (p->vsync_duration_qpc)
        info->vsync_duration = qpc_to_ns(sw, p->vsync_duration_qpc);

    // If the physical frame rate is known and the other members of
    // DXGI_FRAME_STATISTICS are non-0, estimate the timing of the next frame
    if (p->vsync_duration_qpc && stats.PresentCount &&
        stats.PresentRefreshCount && stats.SyncRefreshCount &&
        stats.SyncQPCTime.QuadPart)
    {
        // It's not clear if PresentRefreshCount and SyncRefreshCount can refer
        // to different frames, but in case they can, assuming mpv presents on
        // every frame, guess the present count that relates to SyncRefreshCount.
        unsigned expected_sync_pc = stats.PresentCount +
            (stats.SyncRefreshCount - stats.PresentRefreshCount);

        // Now guess the timestamp of the last submitted frame based on the
        // timestamp of the frame at SyncRefreshCount and the frame rate
        int queued_frames = submit_count - expected_sync_pc;
        int64_t last_queue_display_time_qpc = stats.SyncQPCTime.QuadPart +
            queued_frames * p->vsync_duration_qpc;

        // Only set the estimated display time if it's after the last submission
        // time. It could be before if mpv skips a lot of frames.
        if (last_queue_display_time_qpc >= p->last_submit_qpc) {
            info->last_queue_display_time = mp_time_ns() +
                (qpc_to_ns(sw, last_queue_display_time_qpc) - qpc_ns_now(sw));
        }
    }
}

static bool d3d11_set_fullscreen(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    HRESULT hr;

    m_config_cache_update(p->opts_cache);

    if (!p->swapchain) {
        MP_ERR(ctx, "Full screen configuration was requested before D3D11 "
                    "swap chain was ready!");
        return false;
    }

    // we only want exclusive FS if we are entering FS and
    // exclusive FS is enabled. Otherwise disable exclusive FS.
    bool enable_exclusive_fs = p->vo_opts->fullscreen &&
                               p->opts->exclusive_fs;

    MP_VERBOSE(ctx, "%s full-screen exclusive mode while %s fullscreen\n",
               enable_exclusive_fs ? "Enabling" : "Disabling",
               ctx->vo->opts->fullscreen ? "entering" : "leaving");

    hr = IDXGISwapChain_SetFullscreenState(p->swapchain,
                                           enable_exclusive_fs, NULL);
    if (FAILED(hr))
        return false;

    if (!resize(ctx))
        return false;

    return true;
}

// ----- Composition mode display info helpers --------------------------------
// In composition mode mpv does not own the HWND (the host application, e.g. a
// WinUI XAML SwapChainPanel, owns it). Therefore we cannot reuse w32_common's
// window/monitor tracking. Instead we look up monitor info directly from the
// composition HWND provided via the d3d11-composition-hwnd option.

static double composition_refresh_rate_from_gdi(const wchar_t *device)
{
    DEVMODEW dm = { .dmSize = sizeof dm };
    if (!EnumDisplaySettingsW(device, ENUM_CURRENT_SETTINGS, &dm))
        return 0.0;

    // See w32_common.c: a return of 0 or 1 means "hardware default".
    if (dm.dmDisplayFrequency <= 1)
        return 0.0;

    double rv = dm.dmDisplayFrequency;
    // Convert common integer rates back to the fractional NTSC rates.
    switch (dm.dmDisplayFrequency) {
        case  23: case  29: case  47: case  59: case  71:
        case  89: case  95: case 119: case 143: case 164:
        case 239: case 359: case 479:
            rv = (rv + 1) / 1.001;
    }
    return rv;
}

static HWND composition_get_hwnd(struct priv *p)
{
    m_config_cache_update(p->vo_opts_cache);
    return (HWND)(intptr_t)p->vo_opts->d3d11_composition_hwnd;
}

static HMONITOR composition_get_monitor(struct priv *p)
{
    HWND hwnd = composition_get_hwnd(p);
    if (!hwnd)
        return NULL;
    return MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
}

static double composition_get_display_fps(struct priv *p)
{
    HMONITOR mon = composition_get_monitor(p);
    if (!mon)
        return 0.0;
    MONITORINFOEXW mi = { .cbSize = sizeof mi };
    if (!GetMonitorInfoW(mon, (MONITORINFO *)&mi))
        return 0.0;

    double freq = mp_w32_displayconfig_get_refresh_rate(mi.szDevice);
    if (freq == 0.0)
        freq = composition_refresh_rate_from_gdi(mi.szDevice);
    return freq;
}

static double composition_get_hidpi_scale(struct priv *p)
{
    HMONITOR mon = composition_get_monitor(p);
    if (!mon)
        return 0.0;
    UINT dx = 0, dy = 0;
    if (GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy) != S_OK || !dx)
        return 0.0;
    return dx / 96.0;
}

static void composition_get_display_res(struct priv *p, int res[2])
{
    res[0] = res[1] = 0;
    HMONITOR mon = composition_get_monitor(p);
    if (!mon)
        return;
    MONITORINFO mi = { .cbSize = sizeof mi };
    if (!GetMonitorInfoW(mon, &mi))
        return;
    res[0] = mi.rcMonitor.right - mi.rcMonitor.left;
    res[1] = mi.rcMonitor.bottom - mi.rcMonitor.top;
}

// Build a display-name list for the composition HWND.
//
// Unlike the vo_w32_common path, our HWND is the host application's
// top-level window (set via the d3d11-composition-hwnd option), not a
// child window owned by mpv whose client rect equals the visible video
// area. The host HWND's client rect can be much larger than the actual
// SwapChainPanel that mpv renders to, so EnumDisplayMonitors over it can
// trivially intersect every monitor and produce a misleading multi-monitor
// list. Use only the monitor Windows associates with the host HWND, so
// "display-names" reflects the screen the user perceives the player on.
static char **composition_get_disp_names(struct ra_ctx *ctx, struct priv *p)
{
    HWND hwnd = composition_get_hwnd(p);
    HMONITOR assoc = composition_get_monitor(p);
    if (!assoc)
        return NULL;

    MONITORINFOEXW mi = { .cbSize = sizeof mi };
    if (!GetMonitorInfoW(assoc, (MONITORINFO *)&mi))
        return NULL;

    char assoc_dev[64] = "<unknown>";
    WideCharToMultiByte(CP_UTF8, 0, mi.szDevice, -1, assoc_dev,
                        sizeof assoc_dev, NULL, NULL);

    MP_VERBOSE(ctx, "composition_get_disp_names: hwnd=%p assoc=%p (%s)\n",
               (void *)hwnd, (void *)assoc, assoc_dev);

    char **names = NULL;
    int count = 0;
    MP_TARRAY_APPEND(NULL, names, count, mp_to_utf8(NULL, mi.szDevice));
    MP_TARRAY_APPEND(NULL, names, count, NULL);
    return names;
}

// Poll the monitor hosting the composition HWND and flag VO_EVENT_WIN_STATE
// when it changes, so the core re-queries display FPS / DPI / ICC /
// display-names. Composition mode has no Win32 message loop hooked up here, so
// this is the only signal callers get for monitor moves.
static void composition_poll_monitor(struct ra_ctx *ctx, int *events)
{
    struct priv *p = ctx->priv;
    HMONITOR mon = composition_get_monitor(p);
    if (mon != p->composition_monitor) {
        // Log new monitor's GDI device name so we can confirm in -v output
        // that the composition window's host monitor actually changes when
        // the user drags the window across screens. Without this it's very
        // hard to tell whether MonitorFromWindow / the host hwnd is wrong,
        // or whether the core just isn't re-querying display-names.
        wchar_t dev_w[32] = {0};
        char dev[64] = "<unknown>";
        if (mon) {
            MONITORINFOEXW mi = { .cbSize = sizeof mi };
            if (GetMonitorInfoW(mon, (MONITORINFO *)&mi)) {
                wcsncpy(dev_w, mi.szDevice, 31);
                WideCharToMultiByte(CP_UTF8, 0, dev_w, -1, dev,
                                    sizeof dev, NULL, NULL);
            }
        }
        MP_VERBOSE(ctx, "composition: host monitor changed to %p (%s), "
                   "signalling VO_EVENT_WIN_STATE\n", (void *)mon, dev);
        p->composition_monitor = mon;
        if (events)
            *events |= VO_EVENT_WIN_STATE;
    }
}

static int composition_control(struct ra_ctx *ctx, int *events, int request,
                               void *arg)
{
    struct priv *p = ctx->priv;
    int ret = VO_TRUE;

    switch (request) {
    case VOCTRL_GET_DISPLAY_FPS: {
        double fps = composition_get_display_fps(p);
        if (fps <= 0)
            return VO_NOTAVAIL;
        *(double *)arg = fps;
        break;
    }
    case VOCTRL_GET_HIDPI_SCALE: {
        double scale = composition_get_hidpi_scale(p);
        if (scale <= 0)
            return VO_NOTAVAIL;
        *(double *)arg = scale;
        break;
    }
    case VOCTRL_GET_DISPLAY_RES: {
        int res[2];
        composition_get_display_res(p, res);
        if (!res[0] || !res[1])
            return VO_NOTAVAIL;
        ((int *)arg)[0] = res[0];
        ((int *)arg)[1] = res[1];
        break;
    }
    case VOCTRL_GET_DISPLAY_NAMES: {
        char **names = composition_get_disp_names(ctx, p);
        if (!names)
            return VO_NOTAVAIL;
        *(char ***)arg = names;
        break;
    }
    case VOCTRL_CHECK_EVENTS:
        composition_poll_monitor(ctx, events);
        break;
    default:
        break;
    }
    return ret;
}

static int d3d11_control(struct ra_ctx *ctx, int *events, int request, void *arg)
{
    struct priv *p = ctx->priv;
    int ret = -1;
    bool fullscreen_switch_needed = false;

    switch (request) {
    case VOCTRL_GET_D3D11_ADAPTER: {
        if (!p->adapter.valid)
            return VO_NOTAVAIL;
        struct voctrl_d3d11_adapter *adapter = arg;
        *adapter = (struct voctrl_d3d11_adapter){
            .name = p->adapter_name,
            .luid_low = p->adapter.desc.AdapterLuid.LowPart,
            .luid_high = p->adapter.desc.AdapterLuid.HighPart,
            .ordinal = p->adapter.ordinal,
            .software =
                p->adapter.desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE ||
                (p->adapter.desc.VendorId == 0x1414 &&
                 p->adapter.desc.DeviceId == 0x8c),
        };
        return VO_TRUE;
    }
    case VOCTRL_VO_OPTS_CHANGED: {
        void *changed_option;

        while (m_config_cache_get_next_changed(p->vo_opts_cache,
                                               &changed_option))
        {
            struct mp_vo_opts *vo_opts = p->vo_opts_cache->opts;

            if (changed_option == &vo_opts->fullscreen) {
                fullscreen_switch_needed = true;
            }
        }

        break;
    }
    default:
        break;
    }

    // if leaving full screen, handle d3d11 stuff first, then general
    // windowing
    if (fullscreen_switch_needed && !p->vo_opts->fullscreen) {
        if (!d3d11_set_fullscreen(ctx))
            return VO_FALSE;

        fullscreen_switch_needed = false;
    }

    if (ctx->opts.composition) {
        ret = composition_control(ctx, events, request, arg);
    } else {
        ret = vo_w32_control(ctx->vo, events, request, arg);
    }

    // if entering full screen, handle d3d11 after general windowing stuff
    if (fullscreen_switch_needed && p->vo_opts->fullscreen) {
        if (!d3d11_set_fullscreen(ctx))
            return VO_FALSE;

        fullscreen_switch_needed = false;
    }

    if (*events & VO_EVENT_RESIZE) {
        if (!resize(ctx))
            return VO_ERROR;
    }
    return ret;
}

static void d3d11_uninit(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;

    if (p->swapchain)
        IDXGISwapChain_SetFullscreenState(p->swapchain, FALSE, NULL);

    if (ctx->ra)
        ra_tex_free(ctx->ra, &p->backbuffer);
    if (!ctx->opts.composition) {
        SAFE_RELEASE(p->swapchain);
        vo_w32_uninit(ctx->vo);
    } else {
        vo_w32_swapchain(ctx->vo, NULL);
        SAFE_RELEASE(p->swapchain);
    }
    SAFE_RELEASE(p->device);
    mp_dxgi_factory_uninit(&p->dxgi_ctx);

    // Destroy the RA last to prevent objects we hold from showing up in D3D's
    // leak checker
    if (ctx->ra)
        ctx->ra->fns->destroy(ctx->ra);
}

static const struct ra_swapchain_fns d3d11_swapchain = {
    .color_depth  = d3d11_color_depth,
    .target_csp   = d3d11_target_color_space,
    .target_ref_luma = d3d11_target_ref_luma,
    .start_frame  = d3d11_start_frame,
    .submit_frame = d3d11_submit_frame,
    .swap_buffers = d3d11_swap_buffers,
    .get_vsync    = d3d11_get_vsync,
};

static bool d3d11_init(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv = talloc_zero(ctx, struct priv);
    p->opts_cache = m_config_cache_alloc(ctx, ctx->global, &d3d11_conf);
    p->opts = p->opts_cache->opts;

    p->vo_opts_cache = m_config_cache_alloc(ctx, ctx->vo->global, &vo_sub_opts);
    p->vo_opts = p->vo_opts_cache->opts;

    LARGE_INTEGER perf_freq;
    QueryPerformanceFrequency(&perf_freq);
    p->perf_freq = perf_freq.QuadPart;

    struct ra_swapchain *sw = ctx->swapchain = talloc_zero(ctx, struct ra_swapchain);
    sw->priv = p;
    sw->ctx = ctx;
    sw->fns = &d3d11_swapchain;

    struct d3d11_device_opts dopts = {
        .debug = ctx->opts.debug,
        .allow_warp = p->opts->warp != 0,
        .force_warp = p->opts->warp == 1,
        .max_feature_level = p->opts->feature_level,
        .max_frame_latency = ctx->vo->opts->swapchain_depth,
        .adapter_name = p->opts->adapter_name,
    };
    if (!mp_d3d11_create_present_device(ctx->log, &dopts, &p->device,
                                        &p->adapter))
        goto error;
    p->adapter_name = mp_to_utf8(p, p->adapter.desc.Description);

    if (!spirv_compiler_init(ctx))
        goto error;
    ctx->ra = ra_d3d11_create(p->device, ctx->log, ctx->spirv);
    if (!ctx->ra)
        goto error;

    ctx->opts.composition = p->opts->output_mode == 1;
    if (!ctx->opts.composition && !vo_w32_init(ctx->vo))
        goto error;

    if (ctx->opts.composition && !vo_w32_composition_size(ctx->vo))
        goto error;

    if (!ctx->opts.composition && ctx->opts.want_alpha)
        vo_w32_set_transparency(ctx->vo, ctx->opts.want_alpha);

    UINT usage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
    if (ID3D11Device_GetFeatureLevel(p->device) >= D3D_FEATURE_LEVEL_11_0 &&
        p->opts->output_format != DXGI_FORMAT_B8G8R8A8_UNORM)
    {
        usage |= DXGI_USAGE_UNORDERED_ACCESS;
    }

    struct d3d11_swapchain_opts scopts = {
        .window = ctx->opts.composition ? NULL : vo_w32_hwnd(ctx->vo),
        .width = ctx->vo->dwidth,
        .height = ctx->vo->dheight,
        .format = p->opts->output_format,
        .color_space = p->opts->color_space,
        .configured_csp = &p->swapchain_csp,
        .flip = p->opts->flip,
        // Add one frame for the backbuffer
        .length = ctx->vo->opts->swapchain_depth + 1,
        .usage = usage,
    };
    if (!mp_d3d11_create_swapchain(p->device, ctx->log, &scopts, &p->swapchain))
        goto error;

    if (ctx->opts.composition)
        vo_w32_swapchain(ctx->vo, p->swapchain);

    return true;

error:
    d3d11_uninit(ctx);
    return false;
}

static void d3d11_update_render_opts(struct ra_ctx *ctx)
{
    if (ctx->opts.composition)
        return;
    vo_w32_set_transparency(ctx->vo, ctx->opts.want_alpha);
}

IDXGISwapChain *ra_d3d11_ctx_get_swapchain(struct ra_ctx *ra)
{
    if (ra->swapchain->fns != &d3d11_swapchain)
        return NULL;

    struct priv *p = ra->priv;

    IDXGISwapChain_AddRef(p->swapchain);

    return p->swapchain;
}

#ifdef PL_HAVE_D3D11
void ra_d3d11_ctx_set_swapchain_params(struct ra_ctx *ra,
                                       struct pl_d3d11_swapchain_params *params)
{
    mp_assert(ra->swapchain->fns == &d3d11_swapchain);

    struct priv *p = ra->priv;
    int fmt = p->opts->output_format;

    switch (fmt) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        params->alpha_bits = 8;
        params->color_bits = 8;
        // This is redundant, as alpha bits would force 8-bit output format,
        // but set it anyway to be explicit.
        params->disable_10bit_sdr = true;
        break;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        params->alpha_bits = 2;
        params->color_bits = 10;
        break;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        params->alpha_bits = 16;
        params->color_bits = 16;
        break;
    default:
        params->alpha_bits = ra->opts.want_alpha ? 8 : 0;
    };
}
#endif

const struct ra_ctx_fns ra_ctx_d3d11 = {
    .type               = "d3d11",
    .name               = "d3d11",
    .description        = "Direct3D 11",
    .reconfig           = d3d11_reconfig,
    .control            = d3d11_control,
    .update_render_opts = d3d11_update_render_opts,
    .init               = d3d11_init,
    .uninit             = d3d11_uninit,
};
