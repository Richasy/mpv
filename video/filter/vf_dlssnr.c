/*
 * This file is part of mpv.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>

#include "common/common.h"
#include "common/tags.h"
#include "filters/filter.h"
#include "filters/filter_internal.h"
#include "filters/user_filters.h"
#include "options/m_option.h"
#include "osdep/threads.h"
#include "video/mp_image.h"
#include "video/out/gpu/d3d11_adapter.h"

#include "dlssnr/gpu.h"
#include "dlssnr/params.h"

#define OPT_BASE_STRUCT struct dlssnr_options
static const struct m_option options[] = {
    {"enabled", OPT_BOOL(enabled)},
    {"model-path", OPT_STRING(model_path)},
    {"preset", OPT_INT(preset), M_RANGE(0, 3)},
    {"style", OPT_INT(style), M_RANGE(0, 2)},
    {"intensity", OPT_FLOAT(intensity), M_RANGE(0, 1)},
    {"local-tone", OPT_FLOAT(local_tone), M_RANGE(0, 1)},
    {"local-structure", OPT_FLOAT(local_structure), M_RANGE(0, 1)},
    {"skin-structure", OPT_FLOAT(skin_structure), M_RANGE(-1, 2)},
    {"auto-mask", OPT_BOOL(auto_mask)},
    {"ui-correction", OPT_BOOL(ui_correction)},
    {"scaling", OPT_BOOL(scaling)},
    {"input-resolution", OPT_INT(input_resolution), M_RANGE(25, 100)},
    {"residual-multiplier", OPT_FLOAT(residual_multiplier), M_RANGE(1, 2)},
    {"residual-saturation", OPT_FLOAT(residual_saturation), M_RANGE(0, 2)},
    {"residual-lightness", OPT_FLOAT(residual_lightness), M_RANGE(0, 2)},
    {"shadow-structure", OPT_FLOAT(shadow_structure), M_RANGE(0, 2)},
    {"reflection-glow", OPT_FLOAT(reflection_glow), M_RANGE(0, 2)},
    {"motion-quality", OPT_INT(motion_quality), M_RANGE(0, 5)},
    {"nvof-follow-scaling", OPT_BOOL(nvof_follow_scaling)},
    {"max-height", OPT_INT(max_height), M_RANGE(0, INT_MAX)},
    {0}
};

struct settings {
    _Atomic unsigned references;
    uint64_t serial;
    struct dlssnr_options options;
    char model_path[];
};

struct job {
    struct mp_image *image;
    struct settings *settings;
    uint64_t epoch, history;
};

struct ready_frame {
    struct mp_frame frame;
    struct settings *settings;
    struct dlssnr_gpu_info info;
    uint64_t epoch;
    bool processed;
};

struct priv {
    struct mp_filter *filter;
    mp_thread worker;
    bool worker_started;
    mp_mutex lock;
    mp_cond changed;
    bool stopping, busy;
    struct settings *settings;
    struct job *job;
    struct ready_frame ready;
    uint64_t epoch, history;
    uint64_t processed, passthrough, failed, evaluated;
    uint64_t applied_serial;
    struct dlssnr_gpu_info info;
    struct dlssnr_options applied;
    char logged_error[DLSSNR_ERROR_SIZE];
    bool logged_integrity_warning;
};

static struct settings *new_settings(const struct dlssnr_options *o, uint64_t serial)
{
    const char *path = o->model_path ? o->model_path : "";
    size_t length = strlen(path);
    struct settings *s = malloc(sizeof(*s) + length + 1);
    if (!s)
        return NULL;
    atomic_init(&s->references, 1);
    s->serial = serial;
    s->options = *o;
    memcpy(s->model_path, path, length + 1);
    s->options.model_path = s->model_path;
    return s;
}

static void unref_settings(struct settings *s)
{
    if (s && atomic_fetch_sub_explicit(&s->references, 1, memory_order_acq_rel) == 1)
        free(s);
}

static void free_job(struct job *job)
{
    if (!job)
        return;
    talloc_free(job->image);
    unref_settings(job->settings);
    free(job);
}

static void free_ready(struct ready_frame *ready)
{
    mp_frame_unref(&ready->frame);
    unref_settings(ready->settings);
    *ready = (struct ready_frame){0};
}

static void record_delivery(struct priv *p, const struct ready_frame *ready,
                            bool accepted)
{
    if (!accepted || p->stopping || ready->epoch != p->epoch ||
        ready->frame.type != MP_FRAME_VIDEO || !ready->settings)
        return;
    bool processed = ready->processed && ready->info.status == DLSSNR_ACTIVE;
    if (processed) {
        p->processed++;
        p->applied = ready->settings->options;
        p->applied.model_path = NULL;
        p->applied_serial = ready->settings->serial;
    } else {
        p->passthrough++;
        if (ready->info.status == DLSSNR_RUNTIME_FAILED ||
            ready->info.status == DLSSNR_RUNTIME_MISSING ||
            ready->info.status == DLSSNR_ADAPTER_MISMATCH)
            p->failed++;
    }
    p->info = ready->info;
    if (!p->settings->options.enabled)
        p->info.status = DLSSNR_DISABLED;
    else if (ready->settings->serial != p->settings->serial ||
             (!processed && p->info.status == DLSSNR_ACTIVE))
        p->info.status = DLSSNR_INITIALIZING;
}

static void publish_ready_metadata(struct priv *p, const struct ready_frame *ready)
{
    bool previously_active = p->info.status == DLSSNR_ACTIVE &&
                              p->applied_serial == p->settings->serial;
    p->info = ready->info;
    if (!p->settings->options.enabled)
        p->info.status = DLSSNR_DISABLED;
    else if (ready->settings->serial != p->settings->serial ||
             (ready->info.status == DLSSNR_ACTIVE &&
              (!ready->processed || !previously_active)))
        p->info.status = DLSSNR_INITIALIZING;
}

static const char *status_name(enum dlssnr_status status)
{
    static const char *const names[] = {
        "disabled", "initializing", "active", "source-is-hdr",
        "unsupported-format", "runtime-missing", "runtime-failed", "adapter-mismatch",
    };
    return (unsigned)status < MP_ARRAY_SIZE(names) ? names[status] : "runtime-failed";
}

static enum dlssnr_status source_status(struct mp_image *image,
                                       const struct dlssnr_options *o,
                                       char error[DLSSNR_ERROR_SIZE])
{
    error[0] = 0;
    if (!o->enabled)
        return DLSSNR_DISABLED;
    if (pl_color_transfer_is_hdr(image->params.color.transfer) || image->dovi) {
        snprintf(error, DLSSNR_ERROR_SIZE, "HDR/Dolby Vision source bypassed; native NR is SDR-only");
        return DLSSNR_SOURCE_HDR;
    }
    if (image->params.color.primaries == PL_COLOR_PRIM_BT_2020 ||
        image->params.repr.sys == PL_COLOR_SYSTEM_BT_2020_NC ||
        image->params.repr.sys == PL_COLOR_SYSTEM_BT_2020_C) {
        snprintf(error, DLSSNR_ERROR_SIZE, "BT.2020 SDR conversion is not implemented");
        return DLSSNR_UNSUPPORTED_FORMAT;
    }
    if (o->max_height && image->h > o->max_height) {
        snprintf(error, DLSSNR_ERROR_SIZE, "Source exceeds configured max-height");
        return DLSSNR_UNSUPPORTED_FORMAT;
    }
    if (image->imgfmt != IMGFMT_D3D11 || !image->hwctx || !image->planes[0] ||
        (image->params.hw_subfmt != IMGFMT_NV12 &&
         image->params.hw_subfmt != IMGFMT_P010 &&
         image->params.hw_subfmt != IMGFMT_BGRA &&
         image->params.hw_subfmt != IMGFMT_RGBAF16)) {
        snprintf(error, DLSSNR_ERROR_SIZE,
                 "Native NR requires GPU-resident D3D11 NV12/P010 or opaque RGB frames");
        return DLSSNR_UNSUPPORTED_FORMAT;
    }
    if ((image->params.hw_subfmt == IMGFMT_BGRA ||
         image->params.hw_subfmt == IMGFMT_RGBAF16) &&
        image->params.repr.alpha != PL_ALPHA_NONE) {
        snprintf(error, DLSSNR_ERROR_SIZE, "Transparent RGB sources are not supported");
        return DLSSNR_UNSUPPORTED_FORMAT;
    }
    return DLSSNR_INITIALIZING;
}

static bool color_settings(struct mp_image *image, struct dlssnr_shader_config *out)
{
    struct mp_image_params params = image->params;
    mp_image_params_guess_csp(&params);
    *out = (struct dlssnr_shader_config){
        .width = image->w,
        .height = image->h,
    };
    bool yuv = params.hw_subfmt == IMGFMT_NV12 || params.hw_subfmt == IMGFMT_P010;
    double kr = 0.2126, kb = 0.0722;
    if (yuv) {
        switch (params.repr.sys) {
        case PL_COLOR_SYSTEM_BT_601: kr = 0.299; kb = 0.114; break;
        case PL_COLOR_SYSTEM_BT_709: break;
        case PL_COLOR_SYSTEM_SMPTE_240M: kr = 0.2122; kb = 0.0865; break;
        default: return false;
        }
        if (!dlssnr_yuv_matrix(params.hw_subfmt == IMGFMT_P010 ? 10 : 8,
                              params.repr.levels == PL_COLOR_LEVELS_FULL,
                              kr, kb, out->matrix))
            return false;
        switch (params.chroma_location) {
        case PL_CHROMA_LEFT: out->chroma_x = 0.5f; break;
        case PL_CHROMA_TOP_LEFT: out->chroma_x = out->chroma_y = 0.5f; break;
        case PL_CHROMA_TOP_CENTER: out->chroma_y = 0.5f; break;
        case PL_CHROMA_BOTTOM_LEFT: out->chroma_x = 0.5f; out->chroma_y = -0.5f; break;
        case PL_CHROMA_BOTTOM_CENTER: out->chroma_y = -0.5f; break;
        default: break;
        }
    }
    out->luma[0] = (float)kr;
    out->luma[1] = (float)(1 - kr - kb);
    out->luma[2] = (float)kb;
    return true;
}

static void release_image(void *image)
{
    talloc_free(image);
}

static void release_native_buffer(void *opaque, uint8_t *data)
{
    (void)data;
    struct dlssnr_gpu_output *native = opaque;
    native->release(native->lease);
    free(native);
}

static bool output_hwcontext(AVBufferRef **cache, struct mp_image *source)
{
    AVHWFramesContext *input = (void *)source->hwctx->data;
    if (!input->device_ref)
        return false;
    int width = MP_ALIGN_UP(source->w, 2);
    int height = MP_ALIGN_UP(source->h, 2);
    if (*cache) {
        AVHWFramesContext *old = (void *)(*cache)->data;
        if (old->width == width && old->height == height &&
            old->device_ref->data == input->device_ref->data)
            return true;
    }
    AVBufferRef *next = av_hwframe_ctx_alloc(input->device_ref);
    if (!next)
        return false;
    AVHWFramesContext *frames = (void *)next->data;
    frames->format = AV_PIX_FMT_D3D11;
    frames->sw_format = AV_PIX_FMT_RGBAF16;
    frames->width = width;
    frames->height = height;
    AVD3D11VAFramesContext *d3d11 = frames->hwctx;
    d3d11->BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (av_hwframe_ctx_init(next) < 0) {
        av_buffer_unref(&next);
        return false;
    }
    av_buffer_unref(cache);
    *cache = next;
    return true;
}

static struct mp_image *wrap_output(struct mp_image *source, AVBufferRef *frames,
                                    struct dlssnr_gpu_output *native)
{
    struct dlssnr_gpu_output *lease = malloc(sizeof(*lease));
    AVFrame *frame = av_frame_alloc();
    if (!lease || !frame) {
        free(lease);
        av_frame_free(&frame);
        native->release(native->lease);
        return NULL;
    }
    *lease = *native;
    frame->format = AV_PIX_FMT_D3D11;
    frame->width = source->w;
    frame->height = source->h;
    frame->data[0] = (uint8_t *)native->texture;
    frame->buf[0] = av_buffer_create(NULL, 0, release_native_buffer, lease,
                                    AV_BUFFER_FLAG_READONLY);
    if (!frame->buf[0]) {
        release_native_buffer(lease, NULL);
        av_frame_free(&frame);
        return NULL;
    }
    frame->hw_frames_ctx = av_buffer_ref(frames);
    if (!frame->hw_frames_ctx) {
        av_frame_free(&frame);
        return NULL;
    }
    struct mp_image *output = mp_image_from_av_frame(frame);
    av_frame_free(&frame);
    if (!output)
        return NULL;
    mp_image_copy_attributes(output, source);
    output->params = source->params;
    mp_image_sethwfmt(output, IMGFMT_D3D11, IMGFMT_RGBAF16);
    mp_image_set_size(output, source->w, source->h);
    output->params.repr.sys = PL_COLOR_SYSTEM_RGB;
    output->params.repr.levels = PL_COLOR_LEVELS_FULL;
    output->params.repr.alpha = PL_ALPHA_NONE;
    output->params.repr.bits = (struct pl_bit_encoding){0};
    output->params.chroma_location = PL_CHROMA_UNKNOWN;
    return output;
}

static MP_THREAD_VOID worker_main(void *opaque)
{
    struct priv *p = opaque;
    struct dlssnr_gpu *gpu = NULL;
    AVBufferRef *output_frames = NULL;
    uint64_t history = UINT64_MAX;
    double last_pts = MP_NOPTS_VALUE;
    int last_w = 0, last_h = 0;
    mp_thread_set_name("dlssnr");
    for (;;) {
        mp_mutex_lock(&p->lock);
        while (!p->stopping && !p->job)
            mp_cond_wait(&p->changed, &p->lock);
        if (p->stopping) {
            struct job *abandoned = p->job;
            p->job = NULL;
            mp_mutex_unlock(&p->lock);
            free_job(abandoned);
            break;
        }
        struct job *job = p->job;
        p->job = NULL;
        struct dlssnr_gpu_info info = p->info;
        mp_mutex_unlock(&p->lock);
        struct mp_image *source = job->image;
        struct mp_image *processed = NULL;
        bool evaluated = false;
        struct dlssnr_gpu_input input = {0};
        double duration = isfinite(source->pkt_duration) && source->pkt_duration > 0 ?
                          source->pkt_duration : 1.0 / 24;
        bool discontinuity = source->pts != MP_NOPTS_VALUE &&
            last_pts != MP_NOPTS_VALUE &&
            (source->pts <= last_pts || source->pts - last_pts > MPMAX(0.25, duration * 8));
        bool reset = job->history != history || source->w != last_w ||
                     source->h != last_h || discontinuity;
        history = job->history;
        last_w = source->w;
        last_h = source->h;
        last_pts = isfinite(source->pts) ? source->pts : MP_NOPTS_VALUE;
        if (!color_settings(source, &input.color)) {
            info.status = DLSSNR_UNSUPPORTED_FORMAT;
            snprintf(info.error, sizeof(info.error), "Unsupported SDR YUV matrix");
        } else if (!output_hwcontext(&output_frames, source)) {
            info.status = DLSSNR_UNSUPPORTED_FORMAT;
            snprintf(info.error, sizeof(info.error),
                     "Cannot describe RGBAF16 hardware output on the decoder device");
        } else {
            input.texture = (void *)source->planes[0];
            input.subresource = (unsigned)(uintptr_t)source->planes[1];
            input.lifetime = mp_image_new_ref(source);
            input.release_lifetime = release_image;
            struct dlssnr_gpu_output native = {0};
            evaluated = dlssnr_gpu_process(&gpu, &input, &job->settings->options,
                                           reset, &native, &info);
            if (evaluated) {
                processed = wrap_output(source, output_frames, &native);
                if (!processed) {
                    info.status = DLSSNR_RUNTIME_FAILED;
                    snprintf(info.error, sizeof(info.error), "Cannot wrap the GPU output frame");
                }
            }
        }
        char error_to_log[DLSSNR_ERROR_SIZE] = "";
        bool integrity_warning = false;
        mp_mutex_lock(&p->lock);
        p->busy = false;
        if (evaluated)
            p->evaluated++;
        bool discard = p->stopping || job->epoch != p->epoch;
        if (!discard) {
            struct settings *completed_settings = job->settings;
            job->settings = NULL;
            p->ready = (struct ready_frame){
                .settings = completed_settings,
                .info = info,
                .epoch = job->epoch,
                .processed = processed && p->settings->options.enabled,
            };
            if (p->ready.processed) {
                p->ready.frame = MAKE_FRAME(MP_FRAME_VIDEO, processed);
                processed = NULL;
            } else {
                p->ready.frame = MAKE_FRAME(MP_FRAME_VIDEO, source);
                job->image = NULL;
            }
            publish_ready_metadata(p, &p->ready);
            if (info.model_signature_mismatch && !p->logged_integrity_warning) {
                p->logged_integrity_warning = true;
                integrity_warning = true;
            }
            if (info.error[0] && strcmp(p->logged_error, info.error)) {
                snprintf(p->logged_error, sizeof(p->logged_error), "%s", info.error);
                snprintf(error_to_log, sizeof(error_to_log), "%s", info.error);
            }
        }
        bool wake = !p->stopping;
        mp_mutex_unlock(&p->lock);
        talloc_free(processed);
        free_job(job);
        if (error_to_log[0])
            MP_ERR(p->filter, "%s\n", error_to_log);
        if (integrity_warning)
            MP_WARN(p->filter, "The user-provided reference model has an embedded "
                    "signature hash mismatch; using it as supplied through the "
                    "original mpv-nvngx bridge, without provenance API or model-file changes.\n");
        if (wake)
            mp_filter_wakeup(p->filter);
    }
    av_buffer_unref(&output_frames);
    dlssnr_gpu_destroy(&gpu);
    MP_THREAD_RETURN();
}

static void process(struct mp_filter *f)
{
    struct priv *p = f->priv;
    if (!mp_pin_in_needs_data(f->ppins[1]))
        return;
    mp_mutex_lock(&p->lock);
    struct ready_frame ready = p->ready;
    p->ready = (struct ready_frame){0};
    bool busy = p->busy;
    bool current = !p->stopping && ready.epoch == p->epoch;
    mp_mutex_unlock(&p->lock);
    if (ready.frame.type) {
        if (current) {
            bool accepted = mp_pin_in_write(f->ppins[1], ready.frame);
            mp_mutex_lock(&p->lock);
            record_delivery(p, &ready, accepted);
            mp_mutex_unlock(&p->lock);
            ready.frame = MP_NO_FRAME;
            free_ready(&ready);
            return;
        }
        free_ready(&ready);
    }
    if (busy || !mp_pin_out_request_data(f->ppins[0]))
        return;
    struct mp_frame frame = mp_pin_out_read(f->ppins[0]);
    if (frame.type != MP_FRAME_VIDEO) {
        mp_pin_in_write(f->ppins[1], frame);
        return;
    }
    struct mp_image *image = frame.data;
    char error[DLSSNR_ERROR_SIZE];
    mp_mutex_lock(&p->lock);
    enum dlssnr_status status = source_status(image, &p->settings->options, error);
    if (status != DLSSNR_INITIALIZING) {
        p->history++;
        p->info.status = status;
        snprintf(p->info.error, sizeof(p->info.error), "%s", error);
        bool log = error[0] && strcmp(error, p->logged_error);
        if (log)
            snprintf(p->logged_error, sizeof(p->logged_error), "%s", error);
        mp_mutex_unlock(&p->lock);
        if (log)
            MP_WARN(f, "%s\n", error);
        bool accepted = mp_pin_in_write(f->ppins[1], frame);
        if (accepted) {
            mp_mutex_lock(&p->lock);
            p->passthrough++;
            mp_mutex_unlock(&p->lock);
        }
        return;
    }
    struct job *job = calloc(1, sizeof(*job));
    if (!job) {
        p->info.status = DLSSNR_RUNTIME_FAILED;
        snprintf(p->info.error, sizeof(p->info.error), "Cannot allocate a frame job");
        mp_mutex_unlock(&p->lock);
        MP_ERR(f, "Cannot allocate a frame job\n");
        bool accepted = mp_pin_in_write(f->ppins[1], frame);
        if (accepted) {
            mp_mutex_lock(&p->lock);
            p->passthrough++;
            p->failed++;
            mp_mutex_unlock(&p->lock);
        }
        return;
    }
    job->image = image;
    job->settings = p->settings;
    atomic_fetch_add_explicit(&p->settings->references, 1, memory_order_relaxed);
    job->epoch = p->epoch;
    job->history = p->history;
    p->job = job;
    p->busy = true;
    mp_cond_signal(&p->changed);
    mp_mutex_unlock(&p->lock);
}

static void reset(struct mp_filter *f)
{
    struct priv *p = f->priv;
    mp_mutex_lock(&p->lock);
    p->epoch++;
    p->history++;
    struct job *queued = p->job;
    if (queued) {
        p->job = NULL;
        p->busy = false;
    }
    struct ready_frame ready = p->ready;
    p->ready = (struct ready_frame){0};
    p->info.status = p->settings->options.enabled ? DLSSNR_INITIALIZING : DLSSNR_DISABLED;
    mp_mutex_unlock(&p->lock);
    free_job(queued);
    free_ready(&ready);
}

static bool set_option(struct mp_filter *f, const char *name, const char *value)
{
    struct priv *p = f->priv;
    if (!name || !value)
        return false;
    const struct m_option *option = NULL;
    for (const struct m_option *o = options; o->name; o++) {
        if (!strcmp(o->name, name)) {
            option = o;
            break;
        }
    }
    bool empty_path = option && !strcmp(option->name, "model-path");
    if (!option || (!*value && !empty_path))
        return false;
    mp_mutex_lock(&p->lock);
    struct dlssnr_options candidate = p->settings->options;
    bool path_changed = !strcmp(option->name, "model-path");
    if (path_changed)
        candidate.model_path = NULL;
    int result = m_option_parse(f->log, option, bstr0(option->name), bstr0(value),
                                (char *)&candidate + option->offset);
    char error[DLSSNR_ERROR_SIZE] = "";
    bool valid = result >= 0 && dlssnr_options_valid(&candidate, error, sizeof(error));
    if (!valid) {
        if (error[0])
            snprintf(p->info.error, sizeof(p->info.error), "%s", error);
        mp_mutex_unlock(&p->lock);
        if (error[0])
            MP_ERR(f, "%s\n", error);
        if (path_changed)
            talloc_free(candidate.model_path);
        return false;
    }
    if (dlssnr_options_equal(&candidate, &p->settings->options)) {
        mp_mutex_unlock(&p->lock);
        if (path_changed)
            talloc_free(candidate.model_path);
        return true;
    }
    struct settings *next = new_settings(&candidate, p->settings->serial + 1);
    if (!next) {
        mp_mutex_unlock(&p->lock);
        if (path_changed)
            talloc_free(candidate.model_path);
        return false;
    }
    struct settings *old = p->settings;
    bool structural = !dlssnr_model_options_equal(&old->options, &candidate) ||
        old->options.enabled != candidate.enabled ||
        old->options.scaling != candidate.scaling ||
        (candidate.scaling && old->options.input_resolution != candidate.input_resolution);
    if (structural)
        p->history++;
    p->settings = next;
    p->info.status = candidate.enabled ? DLSSNR_INITIALIZING : DLSSNR_DISABLED;
    mp_mutex_unlock(&p->lock);
    unref_settings(old);
    if (path_changed)
        talloc_free(candidate.model_path);
    return true;
}

static bool command(struct mp_filter *f, struct mp_filter_command *cmd)
{
    struct priv *p = f->priv;
    if (cmd->type == MP_FILTER_COMMAND_TEXT)
        return set_option(f, cmd->cmd, cmd->arg);
    if (cmd->type == MP_FILTER_COMMAND_IS_ACTIVE) {
        mp_mutex_lock(&p->lock);
        cmd->is_active = p->info.status == DLSSNR_ACTIVE && p->settings->options.enabled &&
                         p->applied_serial == p->settings->serial;
        mp_mutex_unlock(&p->lock);
        return true;
    }
    if (cmd->type != MP_FILTER_COMMAND_GET_META)
        return false;
    struct mp_tags *tags = talloc_zero(NULL, struct mp_tags);
    mp_mutex_lock(&p->lock);
    bool active = p->info.status == DLSSNR_ACTIVE && p->settings->options.enabled &&
                  p->applied_serial == p->settings->serial;
    mp_tags_set_str(tags, "requested", p->settings->options.enabled ? "yes" : "no");
    mp_tags_set_str(tags, "status", status_name(p->info.status));
    mp_tags_set_str(tags, "active", active ? "yes" : "no");
    for (const struct m_option *o = options; o->name; o++) {
        char *value = m_option_print(o, (char *)&p->settings->options + o->offset);
        mp_tags_set_str(tags, o->name, value ? value : "");
        talloc_free(value);
    }
#define NUMBER(name, value) \
    mp_tags_set_str(tags, name, mp_tprintf(64, "%" PRIu64, (uint64_t)(value)))
    NUMBER("processed-frames", p->processed);
    NUMBER("passthrough-frames", p->passthrough);
    NUMBER("failed-frames", p->failed);
    NUMBER("evaluated-frames", p->evaluated);
    NUMBER("requested-generation", p->settings->serial);
    NUMBER("applied-generation", p->applied_serial);
    NUMBER("runtime-loads", p->info.runtime_loads);
    NUMBER("feature-builds", p->info.feature_builds);
#undef NUMBER
    mp_tags_set_str(tags, "proc", mp_tprintf(64, "%dx%d", p->info.proc_width, p->info.proc_height));
    mp_tags_set_str(tags, "zero-copy", active ? "yes" : "no");
    mp_tags_set_str(tags, "gpu-copies", "decoder-to-shared-staging;shared-result-to-output");
    char luid[MP_D3D11_ADAPTER_LUID_STRING_SIZE] = "";
    if (p->info.gpu_name[0])
        mp_d3d11_adapter_format_luid(luid, p->info.luid.LowPart, p->info.luid.HighPart);
    mp_tags_set_str(tags, "gpu-luid", luid);
    mp_tags_set_str(tags, "gpu-name", p->info.gpu_name);
    mp_tags_set_str(tags, "last-infer-ms", mp_tprintf(64, "%.3f", p->info.wall_ms));
    mp_tags_set_str(tags, "timing-kind", p->evaluated ?
                    "wall-submit-to-gpu-complete" : "not-measured");
    mp_tags_set_str(tags, "execution-thread", "worker");
    mp_tags_set_str(tags, "gpu-completion", "worker-fence-wait");
    mp_tags_set_str(tags, "motion-status", "disabled-zero-guidance");
    mp_tags_set_str(tags, "motion-supported", "no");
    mp_tags_set_str(tags, "caller-compatibility", p->info.caller_compatibility ?
                    "original-mpv-nvngx-bridge-v1" : "none");
    mp_tags_set_str(tags, "model-signature", p->info.model_signature_mismatch ?
                    "hash-mismatch-reference-artifact" : "not-checked");
    mp_tags_set_str(tags, "last-error", p->info.error);
    if (p->applied_serial) {
        mp_tags_set_str(tags, "applied-preset", mp_tprintf(32, "%d", p->applied.preset));
        mp_tags_set_str(tags, "applied-style", mp_tprintf(32, "%d", p->applied.style));
        mp_tags_set_str(tags, "applied-intensity", mp_tprintf(32, "%.6g", p->applied.intensity));
    }
    mp_mutex_unlock(&p->lock);
    *(struct mp_tags **)cmd->res = tags;
    return true;
}

static void destroy(struct mp_filter *f)
{
    struct priv *p = f->priv;
    mp_mutex_lock(&p->lock);
    p->stopping = true;
    mp_cond_signal(&p->changed);
    mp_mutex_unlock(&p->lock);
    if (p->worker_started)
        mp_thread_join(p->worker);
    free_job(p->job);
    free_ready(&p->ready);
    unref_settings(p->settings);
    mp_cond_destroy(&p->changed);
    mp_mutex_destroy(&p->lock);
}

static const struct mp_filter_info filter = {
    .name = "dlssnr",
    .priv_size = sizeof(struct priv),
    .process = process,
    .command = command,
    .reset = reset,
    .destroy = destroy,
};

static struct mp_filter *create(struct mp_filter *parent, void *configuration)
{
    char error[DLSSNR_ERROR_SIZE];
    struct dlssnr_options *opts = configuration;
    if (!dlssnr_options_valid(opts, error, sizeof(error))) {
        MP_ERR(parent, "dlssnr: %s\n", error);
        talloc_free(configuration);
        return NULL;
    }
    struct settings *settings = new_settings(opts, 1);
    talloc_free(configuration);
    if (!settings)
        return NULL;
    struct mp_filter *f = mp_filter_create(parent, &filter);
    if (!f) {
        unref_settings(settings);
        return NULL;
    }
    struct priv *p = f->priv;
    p->filter = f;
    p->settings = settings;
    p->info.status = settings->options.enabled ? DLSSNR_INITIALIZING : DLSSNR_DISABLED;
    mp_mutex_init(&p->lock);
    mp_cond_init(&p->changed);
    mp_filter_add_pin(f, MP_PIN_IN, "in");
    mp_filter_add_pin(f, MP_PIN_OUT, "out");
    if (mp_thread_create(&p->worker, worker_main, p)) {
        MP_ERR(f, "Cannot start the native NR worker\n");
        talloc_free(f);
        return NULL;
    }
    p->worker_started = true;
    return f;
}

#ifndef DLSSNR_DELIVERY_TEST
const struct mp_user_filter_entry vf_dlssnr = {
    .desc = {
        .description = "Experimental native community NGX Feature18 NR",
        .name = "dlssnr",
        .priv_size = sizeof(struct dlssnr_options),
        .priv_defaults = &dlssnr_defaults,
        .options = options,
    },
    .create = create,
};
#endif
