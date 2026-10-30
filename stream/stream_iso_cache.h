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

#ifndef STREAM_ISO_CACHE_H
#define STREAM_ISO_CACHE_H

#include <stdint.h>

#include "stream.h"
#include "misc/thread_tools.h"
#include "mpv_talloc.h"

// libbluray and libdvdnav adapters share mpv's sparse byte-range LRU. A second
// synchronous read-ahead cache here would delay every small metadata miss until
// an entire chunk has arrived, duplicate storage and evict hot metadata FIFO.
struct iso_cache {
    stream_t *stream;
    int64_t pos;
};

static inline struct iso_cache *iso_cache_create(void *parent, stream_t *stream)
{
    struct iso_cache *c = talloc_zero(parent, struct iso_cache);
    c->stream = stream;
    return c;
}

static inline void iso_cache_free(struct iso_cache *c)
{
    talloc_free(c);
}

// A short result is a real EOF/error, never an unfilled read-ahead page. Cached
// data remains owned by the stream and is released after the disc library.
static inline int iso_cache_read(struct iso_cache *c, void *buf,
                                  int64_t offset, int size)
{
    if (offset < 0 || size < 0 || offset > INT64_MAX - size ||
        mp_cancel_test(c->stream->cancel))
        return -1;
    if (!size)
        return 0;
    if (!stream_seek(c->stream, offset))
        return -1;
    int got = stream_read(c->stream, buf, size);
    return got || !c->stream->error ? got : -1;
}

#endif /* STREAM_ISO_CACHE_H */
