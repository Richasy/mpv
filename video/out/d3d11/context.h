#pragma once

#include <dxgi.h>

#include "video/out/gpu/context.h"

struct m_sub_options;

struct d3d11_opts {
    int feature_level;
    int warp;
    bool flip;
    int sync_interval;
    char *adapter_name;
    int output_format;
    int color_space;
    bool exclusive_fs;
    int output_mode;
};

extern const struct m_sub_options d3d11_conf;

// Get the underlying D3D11 swap chain from an RA context. The returned swap chain is
// refcounted and must be released by the caller.
IDXGISwapChain *ra_d3d11_ctx_get_swapchain(struct ra_ctx *ra);

// Sets pl_d3d11_swapchain_params according to requested d3d11-output-format for
// an RA context.
struct pl_d3d11_swapchain_params;
void ra_d3d11_ctx_set_swapchain_params(struct ra_ctx *ra,
                                       struct pl_d3d11_swapchain_params *params);
