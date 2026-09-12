/*
 * This file is part of mpv.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <assert.h>

#define DLSSNR_DELIVERY_TEST
#include "../vf_dlssnr.c"

int main(void)
{
    assert(!strcmp(options[0].name, "enabled"));
    assert(!strcmp(options[1].name, "model-path"));
    assert(!strcmp(options[2].name, "preset"));
    assert(!strcmp(options[19].name, "max-height"));
    assert(!strcmp(options[20].name, "cache-path"));

    char model[] = "ngx\\model.dll";
    char cache[] = "C:\\app data\\cache";
    struct dlssnr_options copied = dlssnr_defaults;
    copied.model_path = model;
    copied.cache_path = cache;
    struct settings *owned = new_settings(&copied, 1);
    assert(owned);
    model[0] = 'X';
    cache[0] = 'X';
    assert(!strcmp(owned->options.model_path, "ngx\\model.dll"));
    assert(!strcmp(owned->options.cache_path, "C:\\app data\\cache"));
    unref_settings(owned);

    struct settings current = {.serial = 3, .options = dlssnr_defaults};
    struct settings old = {.serial = 2, .options = dlssnr_defaults};
    current.options.model_path = "ngx\\model.dll";
    current.options.cache_path = "C:\\app data\\cache";
    struct priv p = {
        .settings = &current, .epoch = 7,
        .info = {.status = DLSSNR_INITIALIZING},
    };
    struct ready_frame ready = {
        .frame = {.type = MP_FRAME_VIDEO},
        .settings = &current,
        .info = {.status = DLSSNR_ACTIVE},
        .epoch = 7, .processed = true,
    };
    p.ready = ready;
    p.evaluated++;
    publish_ready_metadata(&p, &p.ready);
    assert(p.processed == 0 && p.applied_serial == 0);
    assert(p.info.status == DLSSNR_INITIALIZING);

    record_delivery(&p, &ready, false);
    assert(p.processed == 0 && p.applied_serial == 0);
    p.epoch++;
    record_delivery(&p, &ready, true);
    assert(p.processed == 0 && p.applied_serial == 0);
    assert(p.info.status != DLSSNR_ACTIVE && p.evaluated == 1);

    ready.epoch = p.epoch;
    ready.settings = &old;
    record_delivery(&p, &ready, true);
    assert(p.processed == 1 && p.applied_serial == old.serial);
    assert(p.info.status == DLSSNR_INITIALIZING);

    ready.settings = &current;
    publish_ready_metadata(&p, &ready);
    assert(p.processed == 1 && p.applied_serial == old.serial);
    assert(p.info.status == DLSSNR_INITIALIZING);
    record_delivery(&p, &ready, true);
    assert(p.processed == 2 && p.applied_serial == current.serial);
    assert(p.info.status == DLSSNR_ACTIVE);
    assert(!p.applied.model_path && !p.applied.cache_path);

    current.serial++;
    ready.processed = false;
    ready.info.status = DLSSNR_RUNTIME_FAILED;
    publish_ready_metadata(&p, &ready);
    assert(p.processed == 2 && p.passthrough == 0 && p.failed == 0);
    assert(p.info.status == DLSSNR_RUNTIME_FAILED);
    record_delivery(&p, &ready, true);
    assert(p.processed == 2 && p.passthrough == 1 && p.failed == 1);
    assert(p.applied_serial != current.serial && p.info.status == DLSSNR_RUNTIME_FAILED);
    puts("Pending, rejected, stale, failed and delivered-frame proof tests passed");
    return 0;
}
