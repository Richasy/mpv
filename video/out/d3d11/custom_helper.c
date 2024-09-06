#include "custom_helper.h"

static d3d11_comp_opts* comp_opts;
static bool new_resize_event = false;

bool is_custom_device(struct mpv_global* _global)
{
	MPContext* mpctx = _global->client_api->mpctx;
	if (mpctx->custom_d3d11device) {
		return true;
	}
	return false;
}

bool is_custom_device2(struct ra_ctx* ctx)
{
	struct mpv_global* glob = ctx->global;
	return glob->client_api->mpctx->custom_d3d11device;
}

bool bind_device(struct mpv_global* _global, ID3D11Device** _dev)
{
	MPContext* mpctx = _global->client_api->mpctx;
	ID3D11Device* dev = (ID3D11Device*)mpctx->d3d11_device;
	*_dev = dev;
	return true;
}

void* get_device(struct mpv_global* _global)
{
	MPContext* mpctx = _global->client_api->mpctx;
	if (mpctx->custom_d3d11device) {
		ID3D11Device* dev = (ID3D11Device*)mpctx->custom_d3d11device;
		return (void*)dev;
	}
	return NULL;
}

void mpv_set_custom_d3d11device(mpv_handle* ctx, ID3D11Device* d3d11device)
{
	MPContext* mpctx = ctx->mpctx;
	mpctx->custom_d3d11device = true;
	mpctx->d3d11_device = d3d11device;
}

IDXGISwapChain* mpv_get_swapchain(mpv_handle* ctx)
{
	MPContext* mpctx = ctx->mpctx;
	struct gpu_priv* p = mpctx->video_out->priv;
	struct ra_swapchain* sw = p->ctx->swapchain;
	return sw->priv->swapchain;
}

void mpv_bind_d3d11_comp_opts(d3d11_comp_opts* opts)
{
	if (opts != NULL)
		comp_opts = opts;
}

void mpv_invoke_d3d11_resize(void)
{
	new_resize_event = true;
}

/*
	Following methods are designed to replace w32 function.
*/

int d3d11_comp_control(struct vo* vo, int* events, int request, void* arg)
{
	if (comp_opts != NULL && new_resize_event) {
		*events |= VO_EVENT_RESIZE;
		vo->dwidth = comp_opts->width;
		vo->dheight = comp_opts->height;

		new_resize_event = false;
	}
	return VO_TRUE;
}

void d3d11_comp_uninit(void)
{
	// do nothing
}