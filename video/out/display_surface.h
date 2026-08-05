#ifndef MP_VO_DISPLAY_SURFACE_H_
#define MP_VO_DISPLAY_SURFACE_H_

#include <stdbool.h>
#include <stdint.h>

struct vo_display_surface_state;

struct vo_display_surface_snapshot {
    void *surface;
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

#endif
