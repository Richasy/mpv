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

#ifndef MP_TRANSLATION_H
#define MP_TRANSLATION_H

#include <stdbool.h>
#include <stdint.h>

#include "whisper_translate.h"

struct MPContext;
struct mp_log;
struct mp_translation;

enum mp_translation_source {
    MP_TRANSLATION_SOURCE_WHISPER = 0,
    MP_TRANSLATION_SOURCE_SUBTITLE,
    MP_TRANSLATION_SOURCE_COUNT,
};

enum mp_translation_task_flags {
    MP_TRANSLATION_FILTER_SHORT = 1 << 0,
    MP_TRANSLATION_FILTER_REPEAT = 1 << 1,
};

enum mp_translation_submit_result {
    MP_TRANSLATION_SUBMIT_QUEUED = 0,
    MP_TRANSLATION_SUBMIT_NO_BACKEND,
    MP_TRANSLATION_SUBMIT_TOO_LATE,
    MP_TRANSLATION_SUBMIT_INACTIVE,
    MP_TRANSLATION_SUBMIT_BACKPRESSURE,
};

enum mp_translation_result_kind {
    MP_TRANSLATION_RESULT_TRANSLATED = 0,
    MP_TRANSLATION_RESULT_REUSED,
    MP_TRANSLATION_RESULT_FALLBACK_FAILURE,
    MP_TRANSLATION_RESULT_FALLBACK_SHORT,
    MP_TRANSLATION_RESULT_FALLBACK_LOOP,
    MP_TRANSLATION_RESULT_FALLBACK_RPM,
    MP_TRANSLATION_RESULT_FALLBACK_BUDGET,
    MP_TRANSLATION_RESULT_FALLBACK_QUEUE,
    MP_TRANSLATION_RESULT_FALLBACK_LATE,
};

struct mp_translation_result {
    enum mp_translation_source source;
    uint64_t generation;
    uint64_t cue_id;
    uint64_t revision;
    int seq;
    double pts;
    double duration;
    const char *text;
    const char *translated;
    bool rate_limited;
    const char *error;
    enum mp_translation_result_kind kind;
};

struct mp_translation_limits {
    bool enabled;
    int horizon_sec;
    int seek_debounce_ms;
    int min_text_chars;
    int reuse_cache_capacity;
    int reuse_cache_window_ms;
    int repeat_loop_threshold;
    int repeat_loop_window_ms;
    int rpm_limit;
    int session_request_limit;
};

struct mp_translation_backend_ops {
    void *(*acquire)(void *ctx);
    void (*release)(void *ctx);
    void (*call)(void *ctx, void *talloc_ctx, const char *text,
                 struct wt_call_result *out);
    void (*get_status)(void *ctx, struct wt_status *out);
};

typedef void (*mp_translation_wakeup_fn)(void *ctx);
typedef void (*mp_translation_result_fn)(
    void *ctx, const struct mp_translation_result *result);

struct mp_translation *mp_translation_create(
    void *talloc_parent, struct mp_log *log,
    mp_translation_wakeup_fn wakeup, void *wakeup_ctx);
void mp_translation_destroy(struct mp_translation **translation);

// Replace the backend and transfer its initial reference to the scheduler.
// The backend operations must make acquired references safe across threads.
void mp_translation_set_backend(
    struct mp_translation *translation,
    const struct mp_translation_backend_ops *ops,
    void *backend);
bool mp_translation_has_backend(struct mp_translation *translation);

void mp_translation_register_source(
    struct mp_translation *translation, enum mp_translation_source source);
void mp_translation_unregister_source(
    struct mp_translation *translation, enum mp_translation_source source);
uint64_t mp_translation_invalidate_source(
    struct mp_translation *translation, enum mp_translation_source source);
void mp_translation_invalidate_all(struct mp_translation *translation);
void mp_translation_set_playback_pts(
    struct mp_translation *translation, double pts);

enum mp_translation_submit_result mp_translation_submit(
    struct mp_translation *translation,
    enum mp_translation_source source,
    uint64_t cue_id,
    uint64_t revision,
    const char *text,
    double pts,
    double duration,
    unsigned flags);
void mp_translation_drain(
    struct mp_translation *translation,
    enum mp_translation_source source,
    mp_translation_result_fn callback,
    void *callback_ctx);
int mp_translation_pending_count(
    struct mp_translation *translation, enum mp_translation_source source);

void mp_translation_get_limits(
    struct mp_translation *translation, struct mp_translation_limits *out);
void mp_translation_set_limits(
    struct mp_translation *translation,
    const struct mp_translation_limits *limits);
int mp_translation_update_limits_json(
    struct mp_translation *translation, const char *json, char **error);

bool mp_translation_validate_common_config(
    const char *json, char **error);
char *mp_translation_mask_config(void *talloc_parent, const char *json);
int mp_translation_set_common_config(
    struct mp_translation *translation, const char *json, char **error);
bool mp_translation_common_config_explicit(
    struct mp_translation *translation);
bool mp_translation_common_config_enabled(
    struct mp_translation *translation);
char *mp_translation_get_common_config(
    struct mp_translation *translation, void *talloc_parent);

int mp_translation_configure_legacy_whisper(
    struct mp_translation *translation,
    enum wt_provider provider,
    const char *source_lang,
    const char *target_lang,
    const char *ai_json,
    const char *limits_json);
bool mp_translation_set_legacy_ai(
    struct mp_translation *translation, const char *json);
int mp_translation_set_legacy_limits(
    struct mp_translation *translation, const char *json);
void mp_translation_stop_legacy_whisper(
    struct mp_translation *translation);
char *mp_translation_get_legacy_status(
    struct mp_translation *translation, void *talloc_parent);

struct mp_translation *mpctx_get_translation(struct MPContext *mpctx);
void mpctx_destroy_translation(struct MPContext *mpctx);

#endif
