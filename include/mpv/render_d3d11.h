/* Copyright (C) 2018 the mpv developers
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef MPV_CLIENT_API_RENDER_D3D11_H_
#define MPV_CLIENT_API_RENDER_D3D11_H_

#include "render.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * D3D11 backend
 * -------------
 *
 * This header contains definitions for using D3D11 with the render.h API
 * via the gpu-next (libplacebo) rendering pipeline.
 *
 * Use mpv_render_context_create() with MPV_RENDER_PARAM_API_TYPE set to
 * MPV_RENDER_API_TYPE_D3D11, and MPV_RENDER_PARAM_D3D11_INIT_PARAMS provided.
 *
 * Call mpv_render_context_render() with MPV_RENDER_PARAM_D3D11_FBO to render
 * the video frame to a caller-provided ID3D11Texture2D.
 *
 * The caller is responsible for managing the ID3D11Device and the render
 * target texture lifetime. mpv does not create or manage any swapchain;
 * the caller controls presentation and composition timing.
 *
 * This is designed for scenarios like WinUI/XAML integration where the
 * caller wants to render mpv output into a shared D3D11 texture for
 * composition with other layers (e.g. danmaku overlay, UI controls).
 *
 * Hardware decoding (D3D11VA) is supported when the same device is used
 * for both decoding and rendering.
 */

/**
 * For initializing the mpv D3D11 state via MPV_RENDER_PARAM_D3D11_INIT_PARAMS.
 */
typedef struct mpv_d3d11_init_params {
    /**
     * Pointer to the caller's ID3D11Device. mpv does not take ownership;
     * the caller must keep the device alive for the lifetime of the
     * mpv_render_context.
     *
     * Type: ID3D11Device* (as void* to avoid requiring d3d11.h in this header)
     */
    void *device;
} mpv_d3d11_init_params;

/**
 * For MPV_RENDER_PARAM_D3D11_FBO.
 */
typedef struct mpv_d3d11_fbo {
    /**
     * Pointer to the caller's ID3D11Texture2D render target. mpv will render
     * into this texture each frame. The texture must be created with
     * D3D11_BIND_RENDER_TARGET and D3D11_BIND_SHADER_RESOURCE flags.
     *
     * Type: ID3D11Texture2D* (as void* to avoid requiring d3d11.h)
     */
    void *texture;
    /**
     * Width of the render target in pixels.
     */
    int w;
    /**
     * Height of the render target in pixels.
     */
    int h;
    /**
     * DXGI_FORMAT of the render target (e.g. DXGI_FORMAT_B8G8R8A8_UNORM).
     * Set to 0 for auto-detection from the texture's actual format.
     */
    int format;
} mpv_d3d11_fbo;

#ifdef __cplusplus
}
#endif

#endif
