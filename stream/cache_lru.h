/*
 * Byte-range LRU cache for streaming network sources.
 *
 * Designed to absorb pathological yo-yo seek patterns produced by demuxers
 * over high-latency network streams (typical case: ffmpeg mov demuxer reading
 * from an mp4 with moov in the tail; every demuxer packet triggers a tail
 * seek for sample table, then a head seek for sample data, with each pair
 * costing a full TLS handshake + redirect chain).
 *
 * The cache stores fixed-size byte buckets (default 64 KiB) in a chaining
 * hash table keyed by (file_offset >> bucket_shift). LRU eviction is enforced
 * by a doubly-linked list of buckets.
 *
 * Misses are served by one of up to max_cursors backend connections
 * ("cursors"). The owning stream's own backend is cursor 0; further cursors
 * are separate connections opened on demand with stream_open_lru_cursor().
 * A demuxer that reads two distant regions of one file in alternation (for
 * example an mp4 whose subtitle samples are stored far away from the video
 * samples with the same timestamps) then keeps one sequential cursor per
 * region instead of paying a reconnect for every switch. A miss that lies a
 * short distance (read_through) ahead of a cursor is reached by reading
 * forward on that cursor; the skipped bytes are cached as well.
 *
 * Threading model: SINGLE THREADED. The cache assumes the caller serialises
 * all entry points; mpv's stream layer is single-threaded (the demux thread
 * owns it). No internal locking.
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

#ifndef MPV_STREAM_CACHE_LRU_H
#define MPV_STREAM_CACHE_LRU_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

struct mp_log;
struct stream;

struct stream_lru_cache;

#define STREAM_LRU_MAX_CURSORS 4

struct stream_lru_cache_params {
    // Total memory budget for cached bucket payloads. Must be > 0 to
    // actually create a cache; 0 makes stream_lru_cache_create return NULL.
    size_t capacity_bytes;
    // Size of one cache bucket. Clamped to [4 KiB, 1 MiB] and rounded up to
    // the next power of two. 0 selects the default (64 KiB).
    uint32_t bucket_size;
    // Target bytes per miss before stopping the per-fetch fill_buffer loop.
    // 0 = legacy single-shot fetch. Clamped to bucket_size.
    uint32_t min_fetch_size;
    // Bytes to opportunistically read forward into the cache on every miss
    // that lands in the "file tail" region (see tail_threshold). 0 disables
    // tail prefetch. This targets non-faststart mp4s where the moov atom
    // sits at the end of the file: a sequential prefetch pulls a chunk of
    // moov on one connection instead of letting the demuxer yo-yo between
    // moov and mdat with a fresh TLS handshake per iteration. The cache
    // itself rate-limits re-triggering: once a window has been pulled,
    // subsequent reads inside it hit and do not re-fire. Capped at
    // (capacity_buckets - 1) * bucket_size so the prefetch can never evict
    // the bucket we are about to return.
    uint32_t tail_prefetch;
    // Distance from the file end (in bytes) at which a miss is considered
    // "tail" for prefetch purposes. Ignored when tail_prefetch is 0.
    uint64_t tail_threshold;
    // Upper bound of concurrent backend connections, including the owning
    // stream's own backend. Clamped to [1, STREAM_LRU_MAX_CURSORS]; 1 keeps
    // every miss on the owning stream's backend.
    int max_cursors;
    // A miss at most this many bytes ahead of a cursor is served by reading
    // forward on that cursor instead of seeking. 0 disables reading forward.
    uint32_t read_through;
};

// Create a new LRU cache for owner (the stream whose backend is cursor 0).
// The cache and its backing storage are talloc-managed children of owner.
// Returns NULL if params->capacity_bytes is 0.
struct stream_lru_cache *stream_lru_cache_create(
    struct stream *owner, const struct stream_lru_cache_params *params);

// Free the cache, all owned buckets, and every additional connection it
// opened. Safe to call with NULL.
void stream_lru_cache_destroy(struct stream_lru_cache *c);

// Read len bytes starting at logical offset pos through the cache.
// On a miss, this positions one of the cache's backend connections at pos
// (reading forward, seeking, or opening a connection) and fills buckets.
//
// Contract:
//   - Returns the number of bytes actually written to buf (0 to len).
//   - On a successful read (return > 0) the function clears s->error,
//     matching the contract of stream_lavf::fill_buffer.
//   - On 0-return, s->error reflects whether this was EOF (s->error == 0)
//     or a hard error (s->error != 0). An error of an additional connection
//     is reported through s->error once no connection could recover it.
int stream_lru_cache_read(struct stream_lru_cache *c, struct stream *s,
                          int64_t pos, void *buf, int len);

// Discard the known position of the owning stream's backend (cursor 0).
// Call after operations that moved that backend out-of-band of the cache's
// bookkeeping (an in-place reopen, STREAM_CTRL_AVSEEK). The next miss served
// by cursor 0 seeks it first.
void stream_lru_cache_invalidate_backend_pos(struct stream_lru_cache *c);

// Returns true if the cache has self-disabled due to repeated backend
// failures. In that case the caller should fall back to direct backend reads.
bool stream_lru_cache_is_disabled(struct stream_lru_cache *c);

// Print summary statistics (hit/miss/eviction counters, hit ratio, byte
// totals) at MSGL_INFO. Useful from close_f.
void stream_lru_cache_log_stats(struct stream_lru_cache *c);

#endif // MPV_STREAM_CACHE_LRU_H
