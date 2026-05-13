/*
 * Byte-range LRU cache for streaming network sources.
 *
 * See cache_lru.h for the public contract and threading model.
 *
 * Design summary (rev 3):
 *   - At most ONE fill_buffer per stream_lru_cache_read call. The cache is
 *     latency-transparent: a miss costs exactly one backend round-trip, the
 *     same as the no-cache path.
 *   - One bucket per bucket-index. A bucket caches a contiguous byte range
 *     [start_offset, start_offset+valid_size) where start_offset lies inside
 *     [bidx*bucket_size, (bidx+1)*bucket_size). The range never crosses a
 *     bucket boundary (we cap fetches at the boundary).
 *   - A miss within an already-cached bidx REPLACES the bucket entry. We
 *     don't try to merge non-contiguous sub-ranges. Contiguous forward reads
 *     therefore share a single bucket; jumping back into a partially-cached
 *     bucket at a different offset trades the old slice for the new one.
 *
 * Rationale for "no read-ahead, no fill loop":
 *   Earlier revisions tried to be clever and fill the entire bucket on the
 *   first miss. On a slow remote (CDN pull origin with multi-second TTFB
 *   AND per-GET throughput caps) "loop fill_buffer until 64 KiB" inflated
 *   single-call latency from 1-2 s to 30-60 s. By keeping fetches single-
 *   shot we restore the original mpv timing characteristics; yo-yo seek
 *   hit rate is preserved because every byte that crossed the cache stays
 *   cached for next time.
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

// Self-disable after this many consecutive backend failures, so that a dead
// connection or a non-byte-range server doesn't keep generating useless
// per-fetch retries forever. Caller should then fall back to the direct
// backend path.
#define LRU_DISABLE_AFTER_FAIL    8

struct bucket_entry {
    uint64_t bucket_idx;            // (start_offset >> bucket_shift); also hash key
    int64_t  start_offset;          // first cached byte; in [bidx*bsz, (bidx+1)*bsz)
    uint32_t valid_size;            // bytes from start_offset; never crosses bsz boundary
    bool     is_eof_bucket;         // hitting end of cached range = clean EOF
    uint8_t *data;                  // talloc child of this entry, size == bucket_size
    struct bucket_entry *prev_lru, *next_lru;
    struct bucket_entry *next_hash;
};

struct stream_lru_cache {
    struct mp_log *log;

    uint32_t bucket_size;           // power of two
    uint32_t bucket_shift;          // log2(bucket_size)
    uint64_t bucket_mask;           // bucket_size - 1

    size_t   capacity_bytes;
    size_t   capacity_buckets;
    size_t   used_buckets;

    int64_t  backend_pos;           // -1 = unknown, requires fresh seek
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
// At most ONE fill_buffer per call. Latency-transparent.

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

// Fetch from the backend at exactly `pos`, into a freshly-allocated bucket
// for `bidx`. On success returns the inserted entry (caller can read its
// .data and .valid_size); on EOF/error/cancel returns NULL.
//
// The fetch is bounded so that the cached range never crosses a bucket
// boundary: at most `bucket_size - (pos - bidx*bucket_size)` bytes.
static struct bucket_entry *fetch_at(struct stream_lru_cache *c,
                                     struct stream *s,
                                     uint64_t bidx, int64_t pos,
                                     int max_want, int64_t file_size)
{
    int64_t bucket_off = (int64_t)bidx << c->bucket_shift;
    int max_fit = (int)c->bucket_size - (int)(pos - bucket_off);
    if (max_fit <= 0)
        return NULL;
    if (max_want > max_fit)
        max_want = max_fit;
    if (max_want < 1)
        max_want = 1;

    // L2: always issue the seek; avio_seek no-ops on matching pos. This
    // avoids any backend-pos drift introduced by out-of-band seeks
    // (e.g. STREAM_CTRL_AVSEEK).
    if (s->seek(s, pos) <= 0) {
        c->backend_pos = -1;
        return NULL;
    }
    c->backend_pos = pos;
    c->backend_seeks++;

    struct bucket_entry *e = bucket_alloc(c);
    e->bucket_idx   = bidx;
    e->start_offset = pos;
    e->valid_size   = 0;
    e->is_eof_bucket = false;

    int got = s->fill_buffer(s, e->data, max_want);
    if (got <= 0) {
        talloc_free(e);
        return NULL;
    }

    c->bytes_read_from_backend += got;
    c->backend_pos += got;
    e->valid_size = (uint32_t)got;

    // C2: only mark as a "true" EOF bucket when no error AND we have reached
    // the known file size. Transient short reads must not be cached as fake
    // EOF.
    if (s->error == 0 && file_size > 0
        && (pos + (int64_t)got) >= file_size)
    {
        e->is_eof_bucket = true;
    }

    cache_insert(c, e);
    return e;
}

// ---- public API ------------------------------------------------------------

struct stream_lru_cache *stream_lru_cache_create(void *talloc_parent,
                                                 struct mp_log *log,
                                                 size_t capacity_bytes,
                                                 uint32_t bucket_size)
{
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

    if (capacity_bytes < (size_t)bucket_size * 4)
        capacity_bytes = (size_t)bucket_size * 4;

    struct stream_lru_cache *c =
        talloc_zero(talloc_parent, struct stream_lru_cache);
    c->log              = log;
    c->bucket_size      = bucket_size;
    c->bucket_mask      = (uint64_t)bucket_size - 1;
    c->capacity_bytes   = capacity_bytes;
    c->capacity_buckets = capacity_bytes / bucket_size;
    if (c->capacity_buckets < 4)
        c->capacity_buckets = 4;
    c->backend_pos      = -1;

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
               " hash_slots=%zu\n",
               c->capacity_bytes, c->capacity_buckets,
               c->bucket_size, c->table_capacity);

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
        c->backend_pos = -1;
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
            " backend_seeks=%" PRIu64 " disabled=%d\n",
            c->hit_count, c->miss_count, c->evict_count, hit_ratio,
            c->bytes_served, c->bytes_read_from_backend,
            c->backend_seeks, (int)c->disabled);
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
    c->miss_count++;

    // The existing bucket (if any) caches a different sub-range of this bidx.
    // Drop it; we replace with one starting at `pos`.
    if (e)
        cache_drop_bidx(c, bidx);

    struct bucket_entry *fresh = fetch_at(c, s, bidx, pos, len, file_size);
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
            }
        } else {
            // Clean EOF: no data, no error.
            c->consecutive_failures = 0;
        }
        return 0;
    }

    c->consecutive_failures = 0;

    int copy = (int)fresh->valid_size < len ? (int)fresh->valid_size : len;
    memcpy(buf, fresh->data, (size_t)copy);
    c->bytes_served += copy;
    s->error = 0;
    return copy;
}
