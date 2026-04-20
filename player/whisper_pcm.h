/*
 * Copyright (C) 2025 mpv project
 *
 * Per-chunk audio extractor for the whisper-lookahead NETWORK pipeline.
 *
 * Given a URL/path that mpv is already playing, opens an independent
 * libavformat context, seeks to [t0, t1), decodes the best audio stream
 * to s16 mono 16000 Hz, and writes a small WAV file at out_wav_path.
 *
 * HTTP auth/headers/UA are inherited from the mpv stream-lavf options
 * via mp_setup_av_network_options().
 *
 * Returns 0 on success, -1 on unrecoverable error, -2 on cancellation.
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

struct mp_log;
struct mp_cancel;
struct mpv_global;

int wpcm_extract_chunk_to_wav(struct mp_log *log,
                              struct mpv_global *global,
                              struct mp_cancel *cancel,
                              const char *url,
                              double t0_sec,
                              double t1_sec,
                              const char *out_wav_path);

#endif
