#ifndef MP_VO_DISPLAY_SURFACE_H_
#define MP_VO_DISPLAY_SURFACE_H_

#include <stdbool.h>
#include <stdint.h>

struct vo_display_surface_state;

struct vo_display_surface_snapshot {
    void *surface;
    uint64_t epoch;
};

struct vo_display_surface_frame_snapshot {
    uint64_t serial;
    uint64_t epoch;
};

typedef void (*vo_display_surface_retain_fn)(void *surface);

struct vo_display_surface_state *vo_display_surface_state_create(void *parent);

bool vo_display_surface_publish(struct vo_display_surface_state *state,
                                void *surface,
                                vo_display_surface_retain_fn retain);

struct vo_display_surface_snapshot
vo_display_surface_acquire(struct vo_display_surface_state *state);

uint64_t vo_display_surface_epoch(struct vo_display_surface_state *state);

// A decoded video image submitted to this surface may be published only after
// its swapchain Present succeeds. Empty/OSD/error redraws must not call prepare.
void vo_display_surface_prepare_frame(struct vo_display_surface_state *state,
                                      uint64_t frame_id);
bool vo_display_surface_present_frame(struct vo_display_surface_state *state,
                                      void *surface);
struct vo_display_surface_frame_snapshot
vo_display_surface_last_frame(struct vo_display_surface_state *state);

#endif
