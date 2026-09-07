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

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "filter.h"

struct sh_stream;
struct mp_codec_params;
struct mp_image_params;
struct mp_decoder_list;
struct demux_packet;

// (free with talloc_free(mp_decoder_wrapper.f)
struct mp_decoder_wrapper {
    // Filter with no input and 1 output, which returns the decoded data.
    struct mp_filter *f;

    // Can be set by user.
    struct mp_recorder_sink *recorder_sink;
};

// Create the decoder wrapper for the given stream, plus underlying decoder.
// The src stream must be selected, and remain valid and selected until the
// wrapper is destroyed.
struct mp_decoder_wrapper *mp_decoder_wrapper_create(struct mp_filter *parent,
                                                     struct sh_stream *src);

// Number of extra hw surfaces the player retains on top of the default budget.
// Video only.
void mp_decoder_wrapper_set_extra_hw_frames(struct mp_decoder_wrapper *d, int n);

// Legacy decoder framedrop control.
void mp_decoder_wrapper_set_frame_drops(struct mp_decoder_wrapper *d, int num);
int mp_decoder_wrapper_get_frames_dropped(struct mp_decoder_wrapper *d);

double mp_decoder_wrapper_get_container_fps(struct mp_decoder_wrapper *d);

// Whether to prefer spdif wrapper over real decoders on next reinit.
void mp_decoder_wrapper_set_spdif_flag(struct mp_decoder_wrapper *d, bool spdif);

// Whether to decode only 1 frame and then stop, and cache the frame across resets.
void mp_decoder_wrapper_set_coverart_flag(struct mp_decoder_wrapper *d, bool c);

// True if a pts reset was observed (audio only, heuristic).
bool mp_decoder_wrapper_get_pts_reset(struct mp_decoder_wrapper *d);

void mp_decoder_wrapper_set_play_dir(struct mp_decoder_wrapper *d, int dir);

struct mp_decoder_list *video_decoder_list(void);
struct mp_decoder_list *audio_decoder_list(void);

// For precise seeking: if possible, try to drop frames up until the given PTS.
// This is automatically unset if the target is reached, or on reset.
void mp_decoder_wrapper_set_start_pts(struct mp_decoder_wrapper *d, double pts);

enum dec_ctrl {
    VDCTRL_FORCE_HWDEC_FALLBACK, // force software decoding fallback
    VDCTRL_GET_HWDEC,
    VDCTRL_REINIT,
    VDCTRL_GET_BFRAMES,
    // framedrop mode: 0=none, 1=standard, 2=hrseek
    VDCTRL_SET_FRAMEDROP,
    // int*: extra hw surfaces retained
    VDCTRL_SET_EXTRA_HW_FRAMES,
    VDCTRL_CHECK_FORCED_EOF,
};

int mp_decoder_wrapper_control(struct mp_decoder_wrapper *d,
                               enum dec_ctrl cmd, void *arg);

// Force it to reevaluate output parameters (for overrides like aspect).
void mp_decoder_wrapper_reset_params(struct mp_decoder_wrapper *d);

void mp_decoder_wrapper_get_video_dec_params(struct mp_decoder_wrapper *d,
                                             struct mp_image_params *p);

bool mp_decoder_wrapper_reinit(struct mp_decoder_wrapper *d);

// Audio frame tap. Used by the whisper realtime caption pipeline so that it can
// reuse the primary audio decoder's PCM output instead of opening a second
// demuxer + decoder for the same media (which doubles network traffic on
// streaming sources like emby/jellyfin and may trigger anti-abuse limits).
//
// Lifecycle and threading rules (callers MUST follow):
// - Observer callbacks may run on the decoder's internal worker thread when
//   the decoder uses dec_dispatch; otherwise on the user thread driving the
//   wrapper. Treat them as "unknown thread, must be quick and non-blocking".
// - on_frame is invoked just before each output frame (audio only) is written
//   to the wrapper's output pin. The frame is borrowed; if the observer wants
//   to keep it past the callback, it MUST call mp_frame_ref().
// - on_event signals lifecycle transitions:
//     MP_AFRAME_TAP_RESET   - external reset (e.g. seek). Observer should
//                             discard buffered frames and reset its state.
//     MP_AFRAME_TAP_DESTROY - wrapper is being destroyed. Observer MUST
//                             unhook (the pointer becomes invalid after) and
//                             discard any borrowed frame references.
// - End of stream propagates as on_frame(MP_FRAME_EOF); there is no separate
//   EOF event so observers don't have to keep a parallel state machine.
// - Observer callbacks MUST NOT call any mp_decoder_wrapper_* API on the same
//   wrapper (would deadlock) and MUST NOT block.
enum mp_aframe_tap_event {
    MP_AFRAME_TAP_RESET,
    MP_AFRAME_TAP_DESTROY,
};

struct mp_aframe_observer {
    void *ctx;
    void (*on_frame)(void *ctx, struct mp_frame frame);
    void (*on_event)(void *ctx, enum mp_aframe_tap_event ev);
};

// Install (or replace, when obs != NULL) / remove (obs == NULL) the audio
// frame observer. Only meaningful for audio decoder wrappers; on video/sub
// wrappers the observer is silently retained but never called.
//
// When called with obs == NULL, this BLOCKS until any in-flight callback has
// returned. After the call returns the caller is guaranteed that no further
// callback will be invoked and may safely free the observer's ctx.
void mp_decoder_wrapper_set_aframe_observer(struct mp_decoder_wrapper *d,
                                            const struct mp_aframe_observer *obs);

struct mp_decoder {
    // Bidirectional filter; takes MP_FRAME_PACKET for input.
    struct mp_filter *f;

    // Can be set by decoder impl. on init for "special" functionality.
    int (*control)(struct mp_filter *f, enum dec_ctrl cmd, void *arg);
};

struct mp_decoder_fns {
    struct mp_decoder *(*create)(struct mp_filter *parent,
                                 struct mp_codec_params *codec,
                                 const char *decoder);
    void (*add_decoders)(struct mp_decoder_list *list);
};

extern const struct mp_decoder_fns vd_lavc;
extern const struct mp_decoder_fns ad_lavc;
extern const struct mp_decoder_fns ad_spdif;
extern const struct mp_decoder_fns ad_dsd;

// Convenience wrapper for lavc based decoders. Treat lavc_state as private;
// init to all-0 on init and resets.
struct lavc_state {
    bool eof_returned;
    bool packets_sent;
};
void lavc_process(struct mp_filter *f, struct lavc_state *state,
                  int (*send)(struct mp_filter *f, struct demux_packet *pkt),
                  int (*receive)(struct mp_filter *f, struct mp_frame *res));

// ad_spdif.c
struct mp_decoder_list *select_spdif_codec(const char *codec, const char *pref);

// ad_dsd.c
struct mp_decoder_list *select_dsd_codec(const char *codec, const char *pref);
