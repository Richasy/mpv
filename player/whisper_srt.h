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

#ifndef MP_WHISPER_SRT_H
#define MP_WHISPER_SRT_H

#include <stdint.h>
#include <stddef.h>

struct mp_log;

struct ws_srt_segment {
    int64_t start_ms;       // segment start in ms (relative to SRT t=0)
    int64_t end_ms;         // segment end in ms
    char *text;             // talloc-allocated; concatenation of all text
                            // lines for this segment, joined with " ".
                            // Caller should treat it as opaque and free
                            // through ws_srt_segments_free.
};

struct ws_srt_segments {
    struct ws_srt_segment *items;
    int count;
};

// Parse a SRT file from disk. Returns NULL on failure, caller must free
// via ws_srt_segments_free. Empty (no parseable segments) returns an
// allocated structure with count=0.
struct ws_srt_segments *ws_srt_parse_file(void *talloc_parent,
                                          struct mp_log *log,
                                          const char *path);

// Parse an in-memory SRT string of length len. Same semantics as the
// file variant.
struct ws_srt_segments *ws_srt_parse_buffer(void *talloc_parent,
                                            struct mp_log *log,
                                            const char *data, size_t len);

void ws_srt_segments_free(struct ws_srt_segments **segs);

#endif
