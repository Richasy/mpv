/*
 * Copyright (C) 2025 mpv project
 *
 * Persistent audio extractor session for the whisper-lookahead NETWORK
 * pipeline. Holds a single libavformat context across many chunk
 * extractions so that we read the network stream sequentially -- avoiding
 * the O(N^2) blowup of re-opening per chunk on servers that don't honor
 * byte-Range requests (alist /dav, signed-URL CDNs, etc).
 *
 * The session exposes a forward-only output cursor measured in output
 * samples (s16 mono 16000 Hz). advance_to() pulls and discards samples up
 * to a target time; extract_to() pulls samples up to a target time and
 * writes them to a fresh WAV file. Surplus samples produced by the
 * resampler past the requested boundary are buffered and consumed by the
 * next call, so chunk seams are sample-accurate (no duplication, no drop).
 *
 * HTTP auth/headers/UA are inherited from mpv's stream-lavf options via
 * mp_setup_av_network_options().
 *
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

#ifndef MPV_WHISPER_PCM_H
#define MPV_WHISPER_PCM_H

#include <stdbool.h>

struct mp_log;
struct mp_cancel;
struct mpv_global;

struct wpcm_session;

// Open a persistent extractor session. preferred_audio_idx is the
// AVFormatContext.streams[] index to bind to (== mpv sh_stream->ff_index);
// pass -1 to let ffmpeg pick the best audio stream.
//
// Returns NULL on failure or cancellation.
struct wpcm_session *wpcm_session_open(struct mp_log *log,
                                       struct mpv_global *global,
                                       struct mp_cancel *cancel,
                                       const char *url,
                                       int preferred_audio_idx);

// Current output cursor in seconds. Equals total_output_samples_produced
// (consumed by advance_to and extract_to) divided by 16000.
double wpcm_session_cursor_sec(struct wpcm_session *s);

// True once the underlying input has hit EOF AND all decoder/swr state has
// been drained. After this, advance_to/extract_to become no-ops and the
// session must be closed (and reopened if seeking back).
bool wpcm_session_eof(struct wpcm_session *s);

// Pull samples from the input and discard them until the cursor reaches
// (or passes) t0_sec. If t0_sec <= cursor, returns immediately.
//
// Returns 0 on success (or EOF reached before target), -1 on hard error,
// -2 on cancellation.
int wpcm_session_advance_to(struct wpcm_session *s, double t0_sec);

// Pull samples and write them to a fresh WAV file at out_wav_path until
// the cursor reaches (or passes) t1_sec. Cursor advances by the number of
// samples actually written; surplus samples produced by the resampler in
// the last frame are retained for the next call so chunk seams are
// sample-accurate.
//
// Returns 0 on success (any samples written, or clean EOF), -1 on error,
// -2 on cancellation.
int wpcm_session_extract_to(struct wpcm_session *s,
                            double t1_sec,
                            const char *out_wav_path);

void wpcm_session_close(struct wpcm_session *s);

// Convenience single-shot wrapper: open + advance_to(t0) + extract_to(t1)
// + close. Useful for tests / one-off extractions; not recommended for
// chunked transcription (open is expensive on network sources).
int wpcm_extract_chunk_to_wav(struct mp_log *log,
                              struct mpv_global *global,
                              struct mp_cancel *cancel,
                              const char *url,
                              double t0_sec,
                              double t1_sec,
                              const char *out_wav_path);

#endif
