/*
 * Sparse read-ahead block cache for ISO-over-HTTP playback.
 *
 * When libbluray/libdvdnav reads disc metadata, they make many small random
 * reads. Over HTTP, each read triggers a new Range request with high latency.
 * This cache reads larger chunks (1MB) per HTTP request and caches them,
 * so subsequent reads to nearby offsets are served from memory.
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

#ifndef STREAM_ISO_CACHE_H
#define STREAM_ISO_CACHE_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "stream.h"
#include "common/msg.h"
#include "mpv_talloc.h"

// 1MB per cache chunk, aligned to ISO sector size (2048)
#define ISO_CACHE_CHUNK_SIZE  (1024 * 1024)
// Max 64 chunks = 64MB total cache
#define ISO_CACHE_MAX_CHUNKS  64

struct iso_cache_chunk {
    int64_t offset;   // byte offset in stream, -1 if unused
    int32_t size;     // actual bytes stored
    uint8_t *data;
};

struct iso_cache {
    stream_t *stream;
    struct mp_log *log;
    struct iso_cache_chunk chunks[ISO_CACHE_MAX_CHUNKS];
    int num_chunks;
    int64_t pos; // current logical position (for seek/read callbacks)
};

static inline struct iso_cache *iso_cache_create(void *talloc_parent,
                                                  stream_t *stream,
                                                  struct mp_log *log)
{
    struct iso_cache *c = talloc_zero(talloc_parent, struct iso_cache);
    c->stream = stream;
    c->log = log;
    c->pos = 0;
    for (int i = 0; i < ISO_CACHE_MAX_CHUNKS; i++)
        c->chunks[i].offset = -1;
    return c;
}

static inline void iso_cache_free(struct iso_cache *c)
{
    if (!c)
        return;
    for (int i = 0; i < c->num_chunks; i++)
        free(c->chunks[i].data);
    // c itself is talloc-managed
}

// Find a cached chunk that contains the byte range [offset, offset+size).
// Returns pointer to start of requested data within the chunk, or NULL.
static inline uint8_t *iso_cache_lookup(struct iso_cache *c,
                                         int64_t offset, int size)
{
    for (int i = 0; i < c->num_chunks; i++) {
        struct iso_cache_chunk *ch = &c->chunks[i];
        if (ch->offset < 0)
            continue;
        if (offset >= ch->offset && offset + size <= ch->offset + ch->size)
            return ch->data + (offset - ch->offset);
    }
    return NULL;
}

// Read from cache, fetching from HTTP if needed.
// Returns number of bytes read, or -1 on error.
static inline int iso_cache_read(struct iso_cache *c, void *buf,
                                  int64_t offset, int size)
{
    // Try cache first
    uint8_t *cached = iso_cache_lookup(c, offset, size);
    if (cached) {
        memcpy(buf, cached, size);
        return size;
    }

    // Cache miss: read a larger chunk from HTTP
    int read_size = size > ISO_CACHE_CHUNK_SIZE ? size : ISO_CACHE_CHUNK_SIZE;

    // Align read offset down to sector boundary (2048 bytes)
    int64_t aligned_offset = (offset / 2048) * 2048;
    // Ensure we still cover the requested range
    int64_t end_needed = offset + size;
    if (aligned_offset + read_size < end_needed)
        read_size = (int)(end_needed - aligned_offset);

    // Allocate or reuse a cache slot
    struct iso_cache_chunk *slot = NULL;
    if (c->num_chunks < ISO_CACHE_MAX_CHUNKS) {
        slot = &c->chunks[c->num_chunks++];
    } else {
        // FIFO eviction: reuse oldest entry
        slot = &c->chunks[0];
        free(slot->data);
        memmove(&c->chunks[0], &c->chunks[1],
                (ISO_CACHE_MAX_CHUNKS - 1) * sizeof(c->chunks[0]));
        slot = &c->chunks[ISO_CACHE_MAX_CHUNKS - 1];
        c->num_chunks = ISO_CACHE_MAX_CHUNKS;
    }

    slot->data = malloc(read_size);
    if (!slot->data) {
        slot->offset = -1;
        slot->size = 0;
        return -1;
    }

    // Seek and read from HTTP stream
    if (!stream_seek(c->stream, aligned_offset)) {
        mp_dbg(c->log, "ISO cache: seek to %"PRId64" failed\n", aligned_offset);
        free(slot->data);
        slot->data = NULL;
        slot->offset = -1;
        slot->size = 0;
        return -1;
    }

    int got = stream_read(c->stream, slot->data, read_size);
    if (got <= 0) {
        mp_dbg(c->log, "ISO cache: read at %"PRId64" failed\n", aligned_offset);
        free(slot->data);
        slot->data = NULL;
        slot->offset = -1;
        slot->size = 0;
        return -1;
    }

    slot->offset = aligned_offset;
    slot->size = got;

    // Now serve the requested data from the newly cached chunk
    if (offset >= slot->offset && offset + size <= slot->offset + slot->size) {
        memcpy(buf, slot->data + (offset - slot->offset), size);
        return size;
    }

    // Partial read (near end of file)
    int avail = (int)(slot->offset + slot->size - offset);
    if (avail > 0 && offset >= slot->offset) {
        int to_copy = avail < size ? avail : size;
        memcpy(buf, slot->data + (offset - slot->offset), to_copy);
        return to_copy;
    }

    return -1;
}

#endif /* STREAM_ISO_CACHE_H */
