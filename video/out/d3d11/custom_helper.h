#pragma once

#include <d3d11.h>
#include <pthread.h>

#include "player/core.h"
#include "player/client.h"
#include "common/global.h"
#include "osdep/atomic.h"
#include "../../out/vo.h"
#include "../../out/gpu/context.h"

struct mp_client_api {
	struct MPContext* mpctx;

	pthread_mutex_t lock;

	atomic_bool uses_vo_libmpv;

	// -- protected by lock

	struct mpv_handle** clients;
	int num_clients;
	bool shutting_down; // do not allow new clients
	bool have_terminator; // a client took over the role of destroying the core
	bool terminate_core_thread; // make libmpv core thread exit

	struct mp_custom_protocol* custom_protocols;
	int num_custom_protocols;

	struct mpv_render_context* render_context;
	struct mpv_opengl_cb_context* gl_cb_ctx;
};

struct mpv_handle {
	// -- immmutable
	char name[MAX_CLIENT_NAME];
	struct mp_log* log;
	struct MPContext* mpctx;
	struct mp_client_api* clients;

	// -- not thread-safe
	struct mpv_event* cur_event;
	struct mpv_event_property cur_property_event;

	pthread_mutex_t lock;

	pthread_mutex_t wakeup_lock;
	pthread_cond_t wakeup;

	// -- protected by wakeup_lock
	bool need_wakeup;
	void (*wakeup_cb)(void* d);
	void* wakeup_cb_ctx;
	int wakeup_pipe[2];

	// -- protected by lock

	uint64_t event_mask;
	bool queued_wakeup;
	int suspend_count;

	mpv_event* events;      // ringbuffer of max_events entries
	int max_events;         // allocated number of entries in events
	int first_event;        // events[first_event] is the first readable event
	int num_events;         // number of readable events
	int reserved_events;    // number of entries reserved for replies
	size_t async_counter;   // pending other async events
	bool choked;            // recovering from queue overflow

	struct observe_property** properties;
	int num_properties;
	int lowest_changed;     // attempt at making change processing incremental
	uint64_t property_event_masks; // or-ed together event masks of all properties

	bool fuzzy_initialized; // see scripting.c wait_loaded()
	bool is_weak;           // can not keep core alive on its own
	struct mp_log_buffer* messages;
};

struct gpu_priv {
	struct mp_log* log;
	struct ra_ctx* ctx;

	char* context_name;
	char* context_type;
	struct ra_ctx_opts opts;
	struct gl_video* renderer;

	int events;
};

struct priv {
	struct d3d11_opts* opts;

	struct ra_tex* backbuffer;
	ID3D11Device* device;
	IDXGISwapChain* swapchain;
	struct mp_colorspace swapchain_csp;

	int64_t perf_freq;
	unsigned last_sync_refresh_count;
	int64_t last_sync_qpc_time;
	int64_t vsync_duration_qpc;
	int64_t last_submit_qpc;
};

/* Used by internal to determin if custom d3d11 device is injected. */
bool is_custom_device(struct mpv_global* _global);

/* Used by internal to set device from MPContext. */
bool bind_device(struct mpv_global* _global, ID3D11Device** _dev);

/* Used by internal to get custom device's pointer if injected. */
void* get_device(struct mpv_global* _global);

/* struct to keep track of composition status */
typedef struct d3d11_comp_opts {
	int width;
	int height;
	bool is_fullscreen;
} d3d11_comp_opts;

int d3d11_comp_control(struct vo* vo, int* events, int request, void* arg);
void d3d11_comp_uninit(void);


/* 
 * --- Following functions should be export. --- 
 */

/* Used by user to inject custom device */
void mpv_set_custom_d3d11device(mpv_handle* ctx, ID3D11Device* d3d11device);

IDXGISwapChain* mpv_get_swapchain(mpv_handle* ctx);

void mpv_bind_d3d11_comp_opts(d3d11_comp_opts* opts);

void mpv_invoke_d3d11_resize(void);