/*
 * Byte-range LRU cache for streaming network sources.
 *
 * See cache_lru.h for the public contract and threading model.
 *
 * Design summary (rev 4):
 *   - One fill_buffer per stream_lru_cache_read call by default; optionally a
 *     short bounded fill_buffer loop within a single backend connection if
 *     min_fetch_size > 0. The cache is latency-transparent: a miss costs at
 *     most one backend round-trip plus, in the bounded-loop case, a few
 *     extra TCP recv() syscalls on the same connection (no new TLS).
 *   - One bucket per bucket-index. A bucket caches a contiguous byte range
 *     [start_offset, start_offset+valid_size) where start_offset lies inside
 *     [bidx*bucket_size, (bidx+1)*bucket_size). The range never crosses a
 *     bucket boundary (we cap fetches at the boundary).
 *   - A miss within an already-cached bidx REPLACES the bucket entry. We
 *     don't try to merge non-contiguous sub-ranges. Contiguous forward reads
 *     therefore share a single bucket; jumping back into a partially-cached
 *     bucket at a different offset trades the old slice for the new one.
 *
 * Rationale for bounded-loop fetch (--stream-lru-cache-min-fetch):
 *   The earlier "single fill_buffer per miss" design (rev 3) avoided the
 *   30-60 s single-call regression caused by "loop fill_buffer until 64 KiB"
 *   on CDNs that throttle per-GET throughput. But single-shot fetches leave
 *   valid_size at whatever ONE TCP recv() returned (often 1-4 KiB on TLS
 *   over a slow CDN), and any read whose offset crosses valid_size triggers
 *   a fresh miss and a fresh TLS handshake. Demuxers that issue small
 *   forward steps in a hot region (e.g. mp4 mov demuxer scanning sample
 *   tables in moov, ~1-2 KiB per step) therefore observed almost-100% miss
 *   rates with the rev-3 cache.
 *
 *   Rev 4 reintroduces a fill loop but with hard bounds: an explicit byte
 *   target (default 16 KiB, configurable via --stream-lru-cache-min-fetch)
 *   AND a per-fetch iteration cap (LRU_MAX_FETCH_ITERS). The byte target
 *   is intentionally much smaller than bucket_size, so the worst-case
 *   per-fetch latency stays bounded even on the throttled CDNs that broke
 *   the old aggressive loop. Setting min_fetch_size to 0 restores rev-3
 *   single-shot behaviour as an escape hatch.
 *
 * Backend cursors (rev 5):
 *   A single backend connection cannot serve a demuxer that alternates
 *   between two distant regions of the file: an mp4 whose selected
 *   subtitle track is stored gigabytes away from the video samples with the
 *   same timestamps makes the mov demuxer jump there and back for every
 *   subtitle sample. Each jump used to be a hard seek, i.e. a new TCP + TLS
 *   connection plus a full redirect chain (about 2 s through an Emby 302),
 *   so playback spent most of its time reconnecting. Every miss is now
 *   served by one of up to max_cursors connections, chosen in this order:
 *     1. a cursor already at the miss, or at most read_through bytes behind
 *        it, which reads forward (caching the skipped bytes) -- no seek;
 *     2. a cursor whose position became unknown, e.g. after a reopen;
 *     3. a newly opened connection, when fewer than max_cursors are open;
 *     4. the least recently used cursor, repositioned with a seek.
 *   Each region therefore keeps its own sequential connection. New
 *   connections start at the miss with a single ranged request. A cursor
 *   that stops returning data before the known end of the file (a dead
 *   keep-alive socket, a shortened range) is restarted at the miss once; an
 *   additional connection that fails is closed, and after repeated failures
 *   the cache stays on the owning stream's backend. When the owning
 *   stream's own backend fails while other connections are open, all of
 *   them are closed at once: a server that limits concurrent connections
 *   would otherwise keep refusing the owner behind an idle additional
 *   connection. Each additional connection has its own cancel object, a
 *   slave of the owning stream's.
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

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cache_lru.h"

#include "common/common.h"
#include "common/msg.h"
#include "misc/thread_tools.h"
#include "mpv_talloc.h"
#include "stream.h"

#define LRU_DEFAULT_BUCKET_SIZE   (64u * 1024u)
#define LRU_MIN_BUCKET_SIZE       (4u * 1024u)
#define LRU_MAX_BUCKET_SIZE       (1u * 1024u * 1024u)

// Hard cap on fill_buffer iterations per fetch_at() call, regardless of the
// configured min_fetch target. Stops a misbehaving / extremely slow backend
// from inflating any single miss into an unbounded latency hit.
#define LRU_MAX_FETCH_ITERS       8

// Self-disable after this many consecutive backend failures, so that a dead
// connection or a non-byte-range server doesn't keep generating useless
// per-fetch retries forever. Caller should then fall back to the direct
// backend path.
#define LRU_DISABLE_AFTER_FAIL    8

// Stop opening additional connections after this many of them failed to
// open or failed while in use: the server most likely limits concurrent
// connections, and every further attempt would cost a round-trip.
#define LRU_EXTRA_CURSOR_FAILURES 2

struct bucket_entry {
    uint64_t bucket_idx;            // (start_offset >> bucket_shift); also hash key
    int64_t  start_offset;          // first cached byte; in [bidx*bsz, (bidx+1)*bsz)
    uint32_t valid_size;            // bytes from start_offset; never crosses bsz boundary
    bool     is_eof_bucket;         // hitting end of cached range = clean EOF
    uint8_t *data;                  // talloc child of this entry, size == bucket_size
    struct bucket_entry *prev_lru, *next_lru;
    struct bucket_entry *next_hash;
};

// One backend connection. cursors[0] is the owning stream itself; the others
// are streams opened by stream_open_lru_cursor() and owned by the cache.
struct lru_cursor {
    struct stream *backend;
    // Own cancel object of an additional connection (a slave of the owner's,
    // freed after the backend); NULL for cursors[0].
    struct mp_cancel *cancel;
    int64_t  pos;                   // backend byte position; -1 = unknown
    uint64_t last_used;             // use_clock value of the latest use
    int      id;                    // stable number for diagnostics
};

struct stream_lru_cache {
    struct mp_log *log;

    uint32_t bucket_size;           // power of two
    uint32_t bucket_shift;          // log2(bucket_size)
    uint64_t bucket_mask;           // bucket_size - 1
    uint32_t min_fetch_size;        // target bytes per miss before stopping
                                    // the fill_buffer loop (0 = single shot)
    uint32_t tail_prefetch_size;    // bytes to pull forward on first tail-region
                                    // miss (0 = disabled). Already capped at
                                    // (capacity_buckets - 1) * bucket_size.
    uint64_t tail_threshold;        // miss is "tail" if pos >= file_size - this.

    size_t   capacity_bytes;
    size_t   capacity_buckets;
    size_t   used_buckets;

    struct lru_cursor cursors[STREAM_LRU_MAX_CURSORS];
    int      num_cursors;           // cursors[0] (the owner) is always present
    int      max_cursors;
    uint32_t read_through;          // max forward gap reached by reading
    uint64_t use_clock;
    int      next_cursor_id;
    int      extra_cursor_failures;
    bool     disabled;
    int      consecutive_failures;

    struct bucket_entry **table;    // chaining hash table
    size_t   table_capacity;        // power of two

    struct bucket_entry *lru_head;  // most recently used
    struct bucket_entry *lru_tail;  // least recently used (eviction victim)

    // Stats
    uint64_t hit_count;
    uint64_t miss_count;
    uint64_t evict_count;
    uint64_t bytes_served;          // bytes returned to caller (hit + miss)
    uint64_t bytes_read_from_backend;
    uint64_t backend_seeks;
    uint64_t backend_reconnects;    // cursor reopened in place because it
                                    // returned no data before the known end
                                    // of the file (e.g. dead keep-alive)
    uint64_t cursor_opens;          // additional connections opened
    uint64_t cursor_open_failures;
    int      peak_cursors;
    uint64_t read_throughs;         // misses reached by reading forward
    uint64_t read_through_bytes;
};

// ---- splitmix64 hashing (L3) -----------------------------------------------

static inline uint64_t splitmix64(uint64_t x)
{
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

static inline size_t hash_slot(struct stream_lru_cache *c, uint64_t idx)
{
    return (size_t)(splitmix64(idx) & (uint64_t)(c->table_capacity - 1));
}

// ---- hash table ------------------------------------------------------------

static struct bucket_entry *table_find(struct stream_lru_cache *c, uint64_t idx)
{
    for (struct bucket_entry *e = c->table[hash_slot(c, idx)];
         e; e = e->next_hash)
    {
        if (e->bucket_idx == idx)
            return e;
    }
    return NULL;
}

static void table_insert(struct stream_lru_cache *c, struct bucket_entry *e)
{
    size_t slot = hash_slot(c, e->bucket_idx);
    e->next_hash = c->table[slot];
    c->table[slot] = e;
}

static void table_remove(struct stream_lru_cache *c, struct bucket_entry *e)
{
    size_t slot = hash_slot(c, e->bucket_idx);
    struct bucket_entry **p = &c->table[slot];
    while (*p && *p != e)
        p = &(*p)->next_hash;
    if (*p)
        *p = e->next_hash;
    e->next_hash = NULL;
}

// ---- LRU list --------------------------------------------------------------

static void lru_unlink(struct stream_lru_cache *c, struct bucket_entry *e)
{
    if (e->prev_lru)
        e->prev_lru->next_lru = e->next_lru;
    else
        c->lru_head = e->next_lru;
    if (e->next_lru)
        e->next_lru->prev_lru = e->prev_lru;
    else
        c->lru_tail = e->prev_lru;
    e->prev_lru = e->next_lru = NULL;
}

static void lru_push_head(struct stream_lru_cache *c, struct bucket_entry *e)
{
    e->prev_lru = NULL;
    e->next_lru = c->lru_head;
    if (c->lru_head)
        c->lru_head->prev_lru = e;
    c->lru_head = e;
    if (!c->lru_tail)
        c->lru_tail = e;
}

static void evict_one(struct stream_lru_cache *c)
{
    struct bucket_entry *victim = c->lru_tail;
    if (!victim)
        return;
    lru_unlink(c, victim);
    table_remove(c, victim);
    talloc_free(victim);            // also frees victim->data via talloc parent
    c->used_buckets--;
    c->evict_count++;
}

static struct bucket_entry *bucket_alloc(struct stream_lru_cache *c)
{
    struct bucket_entry *e = talloc_zero(c, struct bucket_entry);
    e->data = talloc_size(e, c->bucket_size);
    return e;
}

// Insert a freshly populated bucket. Bucket is pushed to MRU first to satisfy
// H4: even when capacity is small, the just-fetched bucket is the most
// recently used and won't be the eviction victim if we now have to make room.
static void cache_insert(struct stream_lru_cache *c, struct bucket_entry *e)
{
    table_insert(c, e);
    lru_push_head(c, e);
    c->used_buckets++;
    while (c->used_buckets > c->capacity_buckets)
        evict_one(c);
}

static void bucket_touch(struct stream_lru_cache *c, struct bucket_entry *e)
{
    if (c->lru_head == e)
        return;
    lru_unlink(c, e);
    lru_push_head(c, e);
}

// ---- fetch -----------------------------------------------------------------
//
// One fill_buffer call by default; optionally a short bounded loop on the
// same backend connection (no extra seek, no extra TLS handshake) when
// c->min_fetch_size > 0. The loop stops as soon as the cumulative bytes
// read reach c->min_fetch_size, the iteration cap (LRU_MAX_FETCH_ITERS)
// is hit, or the backend signals EOF / error / cancel.

static void cache_drop_bidx(struct stream_lru_cache *c, uint64_t bidx)
{
    struct bucket_entry *e = table_find(c, bidx);
    if (!e)
        return;
    lru_unlink(c, e);
    table_remove(c, e);
    talloc_free(e);
    c->used_buckets--;
}

// ---- backend cursors -------------------------------------------------------

static bool is_owner(struct stream_lru_cache *c, struct lru_cursor *cur)
{
    return cur == &c->cursors[0];
}

static void cursor_touch(struct stream_lru_cache *c, struct lru_cursor *cur)
{
    cur->last_used = ++c->use_clock;
}

static bool can_open_cursor(struct stream_lru_cache *c)
{
    return c->num_cursors < c->max_cursors &&
           c->extra_cursor_failures < LRU_EXTRA_CURSOR_FAILURES;
}

// Close an additional connection. Invalidates pointers to the last cursor,
// which moves into the freed slot.
static void cursor_close(struct stream_lru_cache *c, struct lru_cursor *cur)
{
    mp_assert(!is_owner(c, cur));
    // The backend may still use its cancel object while it closes.
    free_stream(cur->backend);
    talloc_free(cur->cancel);
    *cur = c->cursors[c->num_cursors - 1];
    c->num_cursors--;
}

static void cursor_close_extra(struct stream_lru_cache *c)
{
    while (c->num_cursors > 1)
        cursor_close(c, &c->cursors[c->num_cursors - 1]);
}

// The owner's backend failed while additional connections were open. A
// server that limits concurrent connections answers that way, and an idle
// additional connection would keep its response open and go on blocking the
// owner. Close all of them and stop opening new ones. Returns true if any
// connection was closed, i.e. retrying the owner's operation may succeed.
static bool cursor_fallback(struct stream_lru_cache *c, const char *operation)
{
    if (c->num_cursors <= 1)
        return false;
    MP_WARN(c, "lru_cache: connection 0 failed to %s while %d connections"
               " were open; continuing with one\n",
            operation, c->num_cursors);
    cursor_close_extra(c);
    c->extra_cursor_failures = LRU_EXTRA_CURSOR_FAILURES;
    return true;
}

// An additional connection failed while in use: close it. Once enough of
// them failed, the server most likely rejects concurrent connections, so the
// cache stops opening them and keeps using the owner's backend only.
static void cursor_failed(struct stream_lru_cache *c, struct lru_cursor *cur,
                          const char *operation)
{
    MP_WARN(c, "lru_cache: connection %d failed to %s at pos=%" PRId64
               " (err=%d); closing it\n",
            cur->id, operation, cur->pos, cur->backend->error);
    cursor_close(c, cur);
    if (++c->extra_cursor_failures >= LRU_EXTRA_CURSOR_FAILURES &&
        c->num_cursors > 1)
    {
        MP_WARN(c, "lru_cache: additional connections keep failing;"
                   " continuing with one\n");
        cursor_close_extra(c);
    }
}

// Open another connection that starts at pos. Returns NULL on failure.
static struct lru_cursor *cursor_open(struct stream_lru_cache *c,
                                      struct stream *s, int64_t pos)
{
    int id = ++c->next_cursor_id;
    char name[32];
    snprintf(name, sizeof(name), "connection%d", id);
    // Cancelling the owner cancels this connection too, but its backend gets
    // an object of its own to install a wakeup callback on.
    struct mp_cancel *cancel = mp_cancel_new(NULL);
    mp_cancel_set_parent(cancel, s->cancel);
    struct stream *backend = stream_open_lru_cursor(s, cancel, pos, name);
    if (!backend) {
        talloc_free(cancel);
        if (!mp_cancel_test(s->cancel)) {
            c->cursor_open_failures++;
            c->extra_cursor_failures++;
            MP_WARN(c, "lru_cache: could not open connection %d at pos=%"
                       PRId64 "\n", id, pos);
        }
        return NULL;
    }
    struct lru_cursor *cur = &c->cursors[c->num_cursors++];
    *cur = (struct lru_cursor){
        .backend = backend,
        .cancel = cancel,
        .pos = pos,
        .id = id,
    };
    c->cursor_opens++;
    c->peak_cursors = MPMAX(c->peak_cursors, c->num_cursors);
    MP_VERBOSE(c, "lru_cache: opened connection %d at pos=%" PRId64
                  " (%d of %d)\n", id, pos, c->num_cursors, c->max_cursors);
    return cur;
}

// Reposition cur at pos with a backend seek. A failure of the owner's
// backend is reported through its s->error.
static bool cursor_seek(struct stream_lru_cache *c, struct lru_cursor *cur,
                        int64_t pos)
{
    struct stream *backend = cur->backend;
    MP_VERBOSE(c, "lru_cache: connection %d seek %" PRId64 " -> %" PRId64
                  "\n", cur->id, cur->pos, pos);
    if (!backend->seek || backend->seek(backend, pos) <= 0) {
        cur->pos = -1;
        return false;
    }
    cur->pos = pos;
    c->backend_seeks++;
    return true;
}

// Read forward on cur until it reaches target, caching every byte read. A
// bucket holds one contiguous slice: the slice under the cursor is continued
// in place, any other slice of that bucket is replaced. Returns true when
// target was reached; EOF, an error or cancellation return false, and after
// a backend error the cursor's position is unknown. Never seeks.
static bool cursor_pull(struct stream_lru_cache *c, struct stream *s,
                        struct lru_cursor *cur, int64_t target,
                        int64_t file_size)
{
    struct stream *backend = cur->backend;
    while (cur->pos < target) {
        if (mp_cancel_test(s->cancel))
            return false;
        uint64_t bidx = (uint64_t)cur->pos >> c->bucket_shift;
        int64_t bucket_end = ((int64_t)bidx << c->bucket_shift)
                             + (int64_t)c->bucket_size;
        struct bucket_entry *e = table_find(c, bidx);
        if (e && (cur->pos < e->start_offset ||
                  cur->pos > e->start_offset + (int64_t)e->valid_size))
        {
            cache_drop_bidx(c, bidx);
            e = NULL;
        }
        bool fresh = !e;
        if (fresh) {
            e = bucket_alloc(c);
            e->bucket_idx = bidx;
            e->start_offset = cur->pos;
        }
        int want = (int)(MPMIN(target, bucket_end) - cur->pos);
        int chunk = backend->fill_buffer(backend,
                                         e->data + (cur->pos - e->start_offset),
                                         want);
        if (chunk <= 0) {
            if (fresh)
                talloc_free(e);
            if (backend->error != 0)
                cur->pos = -1;
            return false;
        }
        cur->pos += chunk;
        c->bytes_read_from_backend += chunk;
        uint32_t valid = (uint32_t)(cur->pos - e->start_offset);
        if (valid > e->valid_size)
            e->valid_size = valid;
        if (file_size > 0 &&
            e->start_offset + (int64_t)e->valid_size >= file_size)
            e->is_eof_bucket = true;
        if (fresh) {
            cache_insert(c, e);
        } else {
            bucket_touch(c, e);
        }
    }
    return true;
}

// Choose the cursor that serves a miss at pos and move it there, in order of
// cost (see the rev 5 design summary at the top of this file). Returns NULL
// on failure or cancellation.
static struct lru_cursor *cursor_for(struct stream_lru_cache *c,
                                     struct stream *s, int64_t pos,
                                     int64_t file_size)
{
    // 1. A cursor at pos, or a short distance behind it.
    struct lru_cursor *closest = NULL;
    for (int i = 0; i < c->num_cursors; i++) {
        struct lru_cursor *cur = &c->cursors[i];
        if (cur->pos < 0 || cur->pos > pos ||
            pos - cur->pos > (int64_t)c->read_through)
            continue;
        if (!closest || cur->pos > closest->pos)
            closest = cur;
    }
    if (closest) {
        int64_t gap = pos - closest->pos;
        if (gap == 0 || cursor_pull(c, s, closest, pos, file_size)) {
            if (gap > 0) {
                c->read_throughs++;
                c->read_through_bytes += gap;
                MP_DBG(c, "lru_cache: connection %d read %" PRId64
                          " bytes forward to pos=%" PRId64 "\n",
                       closest->id, gap, pos);
            }
            cursor_touch(c, closest);
            return closest;
        }
        if (mp_cancel_test(s->cancel))
            return NULL;
        if (closest->pos < 0) {
            if (is_owner(c, closest)) {
                cursor_fallback(c, "read");
            } else {
                cursor_failed(c, closest, "read");
            }
        }
    }

    // 2. A cursor whose position is unknown needs a seek anyway.
    struct lru_cursor *cur = NULL;
    for (int i = 0; i < c->num_cursors; i++) {
        if (c->cursors[i].pos < 0) {
            cur = &c->cursors[i];
            break;
        }
    }

    // 3. Another connection, which a ranged request starts right at pos.
    if (!cur && can_open_cursor(c)) {
        cur = cursor_open(c, s, pos);
        if (cur) {
            cursor_touch(c, cur);
            return cur;
        }
        if (mp_cancel_test(s->cancel))
            return NULL;
    }

    // 4. The least recently used cursor.
    if (!cur) {
        cur = &c->cursors[0];
        for (int i = 1; i < c->num_cursors; i++) {
            if (c->cursors[i].last_used < cur->last_used)
                cur = &c->cursors[i];
        }
    }
    if (cursor_seek(c, cur, pos)) {
        cursor_touch(c, cur);
        return cur;
    }
    if (mp_cancel_test(s->cancel))
        return NULL;
    if (!is_owner(c, cur)) {
        cursor_failed(c, cur, "seek");
        cur = &c->cursors[0];
        if (cursor_seek(c, cur, pos)) {
            cursor_touch(c, cur);
            return cur;
        }
        if (mp_cancel_test(s->cancel))
            return NULL;
    }
    if (!cursor_fallback(c, "seek") || !cursor_seek(c, cur, pos))
        return NULL;
    cursor_touch(c, cur);
    return cur;
}

// Serve a miss at pos: position a cursor there and read at least one byte of
// the bucket containing pos. The bucket may begin before pos when reading
// forward produced it. On success returns the inserted bucket and the cursor
// used (NULL if that connection failed right after delivering the data); on
// EOF/error/cancel returns NULL.
//
// The fetch is bounded so that the cached range never crosses a bucket
// boundary: at most `bucket_size - (pos - bidx*bucket_size)` bytes.
static struct bucket_entry *fetch_at(struct stream_lru_cache *c,
                                     struct stream *s,
                                     uint64_t bidx, int64_t pos,
                                     int max_want, int64_t file_size,
                                     struct lru_cursor **used)
{
    int64_t bucket_off = (int64_t)bidx << c->bucket_shift;
    int max_fit = (int)c->bucket_size - (int)(pos - bucket_off);
    if (max_fit <= 0)
        return NULL;
    if (max_want > max_fit)
        max_want = max_fit;
    if (max_want < 1)
        max_want = 1;

    // Bounded fill loop. With min_fetch_size == 0 this degenerates to the
    // legacy single-shot fetch (one fill_buffer per miss). With min_fetch_size
    // > 0 we keep pumping fill_buffer on the SAME backend connection until
    // either we've accumulated at least min_fetch_size bytes, hit the hard
    // iteration cap, the backend returns 0/error/EOF, or the stream is
    // cancelled. All extra iterations share the original seek + TLS
    // handshake -- they are just additional TCP recv() calls on a connection
    // we already paid the latency for.
    int target = c->min_fetch_size ? (int)c->min_fetch_size : 1;
    if (target > max_want)
        target = max_want;

    bool reopened = false;
    // Each retry follows the loss of one additional connection or the single
    // reopen of a dead one, so the bound only guards against surprises.
    for (int attempt = 0; attempt <= STREAM_LRU_MAX_CURSORS + 1; attempt++) {
        struct lru_cursor *cur = cursor_for(c, s, pos, file_size);
        if (!cur)
            return NULL;
        struct stream *backend = cur->backend;

        // Reading forward may already have produced the start of this
        // bucket; continue it when it ends exactly at pos.
        struct bucket_entry *e = table_find(c, bidx);
        if (e && e->start_offset + (int64_t)e->valid_size != pos) {
            cache_drop_bidx(c, bidx);
            e = NULL;
        }
        bool fresh = !e;
        if (fresh) {
            e = bucket_alloc(c);
            e->bucket_idx = bidx;
            e->start_offset = pos;
        }
        uint8_t *dst = e->data + (pos - e->start_offset);

        int got = 0;
        int iters = 0;
        int last_chunk = 0;
        bool stop_short = false;
        while (got < max_want) {
            // Check cancel at the top of every iteration: a single fill_buffer
            // can block on a slow TCP recv, so we want the earliest possible
            // exit when the user requests interrupt.
            if (mp_cancel_test(s->cancel)) {
                stop_short = true;
                break;
            }
            int chunk = backend->fill_buffer(backend, dst + got, max_want - got);
            iters++;
            last_chunk = chunk;
            if (chunk <= 0) {
                stop_short = true;
                break;
            }
            got += chunk;

            if (got >= target)
                break;
            if (iters >= LRU_MAX_FETCH_ITERS)
                break;
        }

        MP_DBG(c, "lru_cache: fetch_at bidx=%" PRIu64 " pos=%" PRId64
                  " connection=%d max_want=%d target=%d got=%d iters=%d"
                  " last_chunk=%d s_err=%d\n",
                  bidx, pos, cur->id, max_want, target, got, iters,
                  last_chunk, backend->error);

        if (got > 0) {
            cur->pos = pos + got;
            c->bytes_read_from_backend += got;
            e->valid_size = (uint32_t)(cur->pos - e->start_offset);

            // EOF / error handling. stream_lavf::fill_buffer collapses
            // AVERROR_EOF to a negative return WITHOUT setting s->error --
            // a clean EOF therefore shows up as (last_chunk < 0 &&
            // s->error == 0). A hard transport error (avio->error or any
            // other non-EOF AVERROR) sets s->error != 0.
            //
            // When the loop stopped because of a hard transport error AFTER
            // having already accumulated got > 0 bytes, the bytes we
            // collected are valid, but the backend connection is no longer
            // trustworthy. The owner's backend gets a fresh seek on its next
            // use; an additional connection is closed. We still insert the
            // bucket so the caller can consume the partial data; the error
            // will resurface on the next miss (where the fresh seek either
            // recovers or finally fails into the consecutive_failures
            // self-disable path).
            bool fill_error = stop_short && backend->error != 0;
            if (fill_error) {
                if (is_owner(c, cur)) {
                    cur->pos = -1;
                    cursor_fallback(c, "read");
                } else {
                    cursor_failed(c, cur, "read");
                    cur = NULL;
                }
            } else if (file_size > 0
                       && e->start_offset + (int64_t)e->valid_size >= file_size)
            {
                e->is_eof_bucket = true;
            }

            if (fresh) {
                cache_insert(c, e);
            } else {
                bucket_touch(c, e);
            }
            *used = cur;
            return e;
        }

        if (fresh)
            talloc_free(e);
        if (mp_cancel_test(s->cancel))
            return NULL;

        if (backend->error != 0) {
            if (is_owner(c, cur)) {
                // The error surfaces through s->error; the stream layer's
                // reopen then runs without competing connections.
                cur->pos = -1;
                cursor_fallback(c, "read");
                return NULL;
            }
            // Report the error unless another connection recovers.
            s->error = backend->error;
            cursor_failed(c, cur, "read");
            continue;
        }

        // Clean EOF. Accept it at or past the end of the file, and when the
        // size is unknown.
        if (file_size <= 0 || pos >= file_size || reopened)
            return NULL;

        // No data before the known end of the file: the connection is dead,
        // typically a keep-alive socket the server closed after a drained
        // response, or a server answered with a shorter range than
        // requested. Restart it at pos once.
        reopened = true;
        if (!is_owner(c, cur)) {
            MP_VERBOSE(c, "lru_cache: connection %d returned no data at pos=%"
                          PRId64 " before the end of the file; closing it\n",
                       cur->id, pos);
            cursor_close(c, cur);
            continue;
        }
        if (!s->reconnect) {
            // A seek restarts the transfer at pos instead (stream_curl sends
            // a new request), as the cache used to do before every miss.
            MP_VERBOSE(c, "lru_cache: connection 0 returned no data at pos=%"
                          PRId64 " before the end of the file; seeking\n",
                       pos);
            if (!cursor_seek(c, cur, pos))
                return NULL;
            continue;
        }
        MP_VERBOSE(c, "lru_cache: connection 0 returned no data at pos=%"
                      PRId64 " before the end of the file; reopening\n", pos);
        bool reconnected = s->reconnect(s, pos) == STREAM_OK;
        if (!reconnected && !mp_cancel_test(s->cancel) &&
            cursor_fallback(c, "reopen"))
            reconnected = s->reconnect(s, pos) == STREAM_OK;
        if (!reconnected) {
            // The backend may be torn down now (stream_lavf priv == NULL).
            // Stop using the cache and refuse further access; recovery needs
            // a fresh stream.
            MP_WARN(c, "lru_cache: reopen at pos=%" PRId64 " failed, disabling"
                       " cache\n", pos);
            cursor_close_extra(c);
            c->disabled = true;
            c->cursors[0].pos = -1;
            s->broken = true;
            // Non-zero sentinel: a failed reopen is not the end of the file.
            if (!s->error)
                s->error = -1;
            return NULL;
        }
        c->backend_reconnects++;
        c->cursors[0].pos = pos;
    }
    return NULL;
}

// ---- public API ------------------------------------------------------------

static void cache_destructor(void *p)
{
    cursor_close_extra(p);
}

struct stream_lru_cache *stream_lru_cache_create(
    struct stream *owner, const struct stream_lru_cache_params *params)
{
    size_t capacity_bytes = params->capacity_bytes;
    uint32_t bucket_size = params->bucket_size;
    uint32_t min_fetch_size = params->min_fetch_size;
    uint32_t tail_prefetch = params->tail_prefetch;

    if (capacity_bytes == 0)
        return NULL;

    if (bucket_size == 0)
        bucket_size = LRU_DEFAULT_BUCKET_SIZE;
    if (bucket_size < LRU_MIN_BUCKET_SIZE)
        bucket_size = LRU_MIN_BUCKET_SIZE;
    if (bucket_size > LRU_MAX_BUCKET_SIZE)
        bucket_size = LRU_MAX_BUCKET_SIZE;

    // Round up to next power of two
    {
        uint32_t pw = 1;
        while (pw < bucket_size)
            pw <<= 1;
        bucket_size = pw;
    }

    // Clamp min_fetch to the bucket size; a fetch cannot legally cross a
    // bucket boundary, so asking for more would be silently capped anyway.
    if (min_fetch_size > bucket_size)
        min_fetch_size = bucket_size;

    if (capacity_bytes < (size_t)bucket_size * 4)
        capacity_bytes = (size_t)bucket_size * 4;

    struct stream_lru_cache *c = talloc_zero(owner, struct stream_lru_cache);
    c->log              = owner->log;
    c->bucket_size      = bucket_size;
    c->bucket_mask      = (uint64_t)bucket_size - 1;
    c->min_fetch_size   = min_fetch_size;
    c->capacity_bytes   = capacity_bytes;
    c->capacity_buckets = capacity_bytes / bucket_size;
    if (c->capacity_buckets < 4)
        c->capacity_buckets = 4;

    // The owner's backend has not been read since it opened, so it sits at
    // the stream position.
    c->cursors[0] = (struct lru_cursor){ .backend = owner, .pos = owner->pos };
    c->num_cursors  = 1;
    c->peak_cursors = 1;
    c->max_cursors  = MPCLAMP(params->max_cursors, 1, STREAM_LRU_MAX_CURSORS);
    c->read_through = params->read_through;
    talloc_set_destructor(c, cache_destructor);

    // Cap tail prefetch to (capacity_buckets - 1) * bucket_size so the
    // prefetch can never evict the originating-miss bucket we are about to
    // return to the caller. With the default 256 MiB cache / 64 KiB bucket
    // this cap is ~256 MiB; the practical default (4 MiB) is well within.
    if (tail_prefetch > 0 && c->capacity_buckets > 1) {
        uint64_t cap = (uint64_t)(c->capacity_buckets - 1)
                       * (uint64_t)bucket_size;
        if (cap > 0xFFFFFFFFu)
            cap = 0xFFFFFFFFu;
        if ((uint64_t)tail_prefetch > cap)
            tail_prefetch = (uint32_t)cap;
    } else {
        tail_prefetch = 0;
    }
    c->tail_prefetch_size = tail_prefetch;
    c->tail_threshold     = params->tail_threshold;

    // log2(bucket_size)
    {
        uint32_t shift = 0;
        while ((1u << shift) < bucket_size)
            shift++;
        c->bucket_shift = shift;
    }

    // Hash table sized to ~2x capacity_buckets, rounded up to power of two.
    {
        size_t target = c->capacity_buckets * 2;
        size_t tcap = 1;
        while (tcap < target)
            tcap <<= 1;
        if (tcap < 16)
            tcap = 16;
        c->table_capacity = tcap;
        c->table = talloc_zero_array(c, struct bucket_entry *, tcap);
    }

    MP_VERBOSE(c,
               "lru_cache: created capacity=%zu bytes (%zu buckets x %u bytes)"
               " hash_slots=%zu min_fetch=%u tail_prefetch=%u"
               " tail_threshold=%" PRIu64 " connections=%d read_through=%u\n",
               c->capacity_bytes, c->capacity_buckets,
               c->bucket_size, c->table_capacity,
               c->min_fetch_size, c->tail_prefetch_size, c->tail_threshold,
               c->max_cursors, c->read_through);

    return c;
}

void stream_lru_cache_destroy(struct stream_lru_cache *c)
{
    if (!c)
        return;
    talloc_free(c);
}

void stream_lru_cache_invalidate_backend_pos(struct stream_lru_cache *c)
{
    if (c)
        c->cursors[0].pos = -1;
}

bool stream_lru_cache_is_disabled(struct stream_lru_cache *c)
{
    return c && c->disabled;
}

void stream_lru_cache_log_stats(struct stream_lru_cache *c)
{
    if (!c)
        return;
    uint64_t total = c->hit_count + c->miss_count;
    double hit_ratio = total ? (100.0 * (double)c->hit_count / (double)total)
                             : 0.0;
    MP_INFO(c,
            "lru_cache: hits=%" PRIu64 " misses=%" PRIu64
            " evictions=%" PRIu64 " hit_ratio=%.2f%%"
            " served=%" PRIu64 "B from_backend=%" PRIu64 "B"
            " backend_seeks=%" PRIu64 " reconnects=%" PRIu64
            " connections_opened=%" PRIu64 " connection_failures=%" PRIu64
            " peak_connections=%d read_throughs=%" PRIu64
            " read_through=%" PRIu64 "B disabled=%d\n",
            c->hit_count, c->miss_count, c->evict_count, hit_ratio,
            c->bytes_served, c->bytes_read_from_backend,
            c->backend_seeks, c->backend_reconnects,
            c->cursor_opens, c->cursor_open_failures, c->peak_cursors,
            c->read_throughs, c->read_through_bytes, (int)c->disabled);
}

int stream_lru_cache_read(struct stream_lru_cache *c, struct stream *s,
                          int64_t pos, void *buf, int len)
{
    if (!c || c->disabled || len <= 0 || pos < 0)
        return 0;

    if (mp_cancel_test(s->cancel))
        return 0;

    int64_t file_size = stream_get_size(s);
    uint64_t bidx = (uint64_t)pos >> c->bucket_shift;

    struct bucket_entry *e = table_find(c, bidx);

    // ---- HIT ----
    // Bucket exists AND the cached range covers `pos`.
    if (e && pos >= e->start_offset
        && pos < e->start_offset + (int64_t)e->valid_size)
    {
        c->hit_count++;
        bucket_touch(c, e);

        int avail = (int)(e->start_offset + (int64_t)e->valid_size - pos);
        int copy = avail < len ? avail : len;
        memcpy(buf, e->data + (pos - e->start_offset), (size_t)copy);
        c->bytes_served += copy;

        // C3: success clears s->error to match the fill_buffer contract.
        s->error = 0;
        return copy;
    }

    // Confirmed-EOF bucket past its valid range -> nothing to read.
    if (e && e->is_eof_bucket
        && pos >= e->start_offset + (int64_t)e->valid_size)
    {
        return 0;
    }

    // ---- MISS ----
    // fetch_at decides what happens to a bucket of this bidx that caches a
    // different sub-range: it is continued when it ends exactly at pos and
    // replaced otherwise.
    c->miss_count++;

    struct lru_cursor *used = NULL;
    struct bucket_entry *fresh = fetch_at(c, s, bidx, pos, len, file_size,
                                          &used);
    if (!fresh) {
        bool is_error  = (s->error != 0);
        bool is_cancel = mp_cancel_test(s->cancel);
        if (is_error || is_cancel) {
            c->consecutive_failures++;
            if (c->consecutive_failures >= LRU_DISABLE_AFTER_FAIL) {
                MP_WARN(c,
                        "lru_cache: disabled after %d consecutive backend"
                        " failures (last fetch at bucket %" PRIu64
                        " pos=%" PRId64 ")\n",
                        c->consecutive_failures, bidx, pos);
                c->disabled = true;
                cursor_close_extra(c);
            }
        } else {
            // Clean EOF: no data, no error.
            c->consecutive_failures = 0;
        }
        return 0;
    }

    c->consecutive_failures = 0;

    int avail = (int)(fresh->start_offset + (int64_t)fresh->valid_size - pos);
    int copy = avail < len ? avail : len;
    memcpy(buf, fresh->data + (pos - fresh->start_offset), (size_t)copy);
    c->bytes_served += copy;
    s->error = 0;

    // Tail prefetch (after data is safely copied to the caller buffer, so
    // any eviction triggered by prefetch inserts cannot corrupt our return).
    // Re-triggered on EVERY miss whose pos falls in the tail region. The
    // natural rate limiter is cache coverage itself: once a 4 MiB window
    // around `pos` has been pulled in, subsequent reads inside it hit and
    // do not re-trigger. Only when the demuxer touches a previously
    // unseen tail position (e.g. after a user seek lands in a new region
    // of the moov atom) does a new prefetch fire.
    //
    // fetch_at left the cursor right behind the fresh data, which is
    // exactly where the prefetch wants to continue: no extra seek, no extra
    // TLS handshake.
    if (c->tail_prefetch_size > 0
        && file_size > 0
        && pos >= file_size - (int64_t)c->tail_threshold
        && used
        && used->pos == fresh->start_offset + (int64_t)fresh->valid_size)
    {
        int64_t pf_target = pos + (int64_t)c->tail_prefetch_size;
        if (pf_target > file_size)
            pf_target = file_size;
        bool done = cursor_pull(c, s, used, pf_target, file_size)
                    || used->pos >= file_size;
        MP_VERBOSE(c, "lru_cache: tail prefetch %s at pos=%" PRId64
                      " (file_size=%" PRId64 ", target=%u bytes)\n",
                   done ? "completed" : "interrupted",
                   pos, file_size, c->tail_prefetch_size);
        if (used->pos < 0 && !is_owner(c, used))
            cursor_failed(c, used, "prefetch");
        // Clear any error/cancel residue from the prefetch path so the
        // caller's successful read doesn't surface as a spurious failure.
        s->error = 0;
    }

    return copy;
}
