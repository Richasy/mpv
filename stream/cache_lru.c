/*
 * Byte-range LRU cache for streaming network sources.
 *
 * See cache_lru.h for the public contract and threading model.
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

// Hard ceiling on how many buckets one fetch_buckets() call may insert.
// Combined with the capacity/4 cap below this prevents H4 (a single fetch
// from evicting its own freshly inserted buckets).
#define LRU_MAX_FETCH_BUCKETS     64

// Self-disable after this many consecutive backend failures, so that a dead
// connection or a non-byte-range server doesn't keep generating useless
// per-fetch retries forever. Caller should then fall back to the direct
// backend path.
#define LRU_DISABLE_AFTER_FAIL    8

struct bucket_entry {
    uint64_t bucket_idx;            // file_offset >> bucket_shift
    int64_t  file_offset;           // bucket_idx << bucket_shift
    uint32_t valid_size;            // <= bucket_size; last bucket may be short
    bool     is_eof_bucket;         // hitting end of this bucket = clean EOF
    uint8_t *data;                  // talloc child of this entry
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

enum fetch_result {
    FETCH_OK_DATA,                  // at least one full bucket inserted
    FETCH_EOF,                      // backend signalled clean EOF
    FETCH_ERROR,                    // backend reported error (s->error set)
    FETCH_CANCELED,                 // mp_cancel fired during the fetch
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

// Decide how many consecutive missing buckets starting at start_idx to fetch
// in one round. Bounded by:
//   - LRU_MAX_FETCH_BUCKETS                : protect demux thread responsiveness
//   - capacity_buckets / 4                 : prevent same-fetch eviction (H4)
//   - first already-cached bucket in run   : avoid useless re-read
//   - wanted (caller's residual byte need) : don't over-read
static int plan_fetch(struct stream_lru_cache *c, uint64_t start_idx, int wanted)
{
    int max = LRU_MAX_FETCH_BUCKETS;
    int cap_max = (int)(c->capacity_buckets / 4);
    if (cap_max < 1)
        cap_max = 1;
    if (max > cap_max)
        max = cap_max;
    if (wanted > 0 && max > wanted)
        max = wanted;
    if (max < 1)
        max = 1;

    for (int i = 1; i < max; i++) {
        if (table_find(c, start_idx + i))
            return i;
    }
    return max;
}

static enum fetch_result fetch_buckets(struct stream_lru_cache *c,
                                       struct stream *s,
                                       uint64_t start_idx,
                                       int n_max,
                                       int64_t file_size)
{
    int64_t off = (int64_t)start_idx << c->bucket_shift;

    // L2: always issue the seek; the backend is expected to no-op for matching
    // pos (avio_seek does this), so this is essentially free when we are
    // already in the right place and avoids the H1 shadow-pos bug.
    if (s->seek(s, off) <= 0) {
        c->backend_pos = -1;
        if (mp_cancel_test(s->cancel))
            return FETCH_CANCELED;
        return FETCH_ERROR;
    }
    c->backend_pos = off;
    c->backend_seeks++;

    int filled = 0;

    for (int i = 0; i < n_max; i++) {
        struct bucket_entry *e = bucket_alloc(c);
        e->bucket_idx  = start_idx + i;
        e->file_offset = off + (int64_t)i * (int64_t)c->bucket_size;

        // Read up to one full bucket. Loop because backends may short-read
        // (e.g. one TCP segment per fill_buffer call) without indicating EOF.
        int got_in_bucket = 0;
        bool short_read = false;
        while (got_in_bucket < (int)c->bucket_size) {
            int got = s->fill_buffer(s, e->data + got_in_bucket,
                                     (int)c->bucket_size - got_in_bucket);
            if (got <= 0) {
                short_read = true;
                break;
            }
            got_in_bucket += got;
            c->backend_pos += got;
            c->bytes_read_from_backend += got;
        }

        if (got_in_bucket == 0) {
            // Couldn't read anything for this bucket. Discard and report.
            talloc_free(e);
            if (filled > 0)
                return FETCH_OK_DATA;       // earlier buckets are valid
            if (mp_cancel_test(s->cancel))
                return FETCH_CANCELED;
            if (s->error)
                return FETCH_ERROR;
            return FETCH_EOF;               // intentionally do NOT cache
                                            // anything for "EOF before any
                                            // data" -- avoids C2 poisoning.
        }

        e->valid_size = (uint32_t)got_in_bucket;

        if (short_read) {
            // C2: only cache as a "real" EOF bucket when we can prove from
            // file_size that this is genuinely the end. Otherwise (transport
            // hiccup, server cut us off mid-body, unknown size) we throw the
            // partial bucket away -- a transient error must NOT poison future
            // reads as fake EOF.
            bool real_eof = (s->error == 0)
                         && (file_size > 0)
                         && (e->file_offset + got_in_bucket >= file_size);

            if (real_eof) {
                e->is_eof_bucket = true;
                cache_insert(c, e);
                return FETCH_EOF;
            }

            talloc_free(e);
            if (s->error)
                return filled > 0 ? FETCH_OK_DATA : FETCH_ERROR;
            return filled > 0 ? FETCH_OK_DATA : FETCH_EOF;
        }

        // Full bucket
        cache_insert(c, e);
        filled++;
    }

    return FETCH_OK_DATA;
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

    int64_t file_size = stream_get_size(s);
    int total = 0;
    int iter = 0;

    while (total < len) {
        // M5: keep cancellation responsive even on long all-hit runs.
        if ((++iter & 0xF) == 0 && mp_cancel_test(s->cancel))
            break;

        int64_t cur = pos + total;
        uint64_t bidx = (uint64_t)cur >> c->bucket_shift;
        uint32_t off  = (uint32_t)((uint64_t)cur & c->bucket_mask);

        struct bucket_entry *e = table_find(c, bidx);
        if (!e) {
            // How many additional bucket-aligned bytes the caller still wants.
            int wanted_bytes = len - total;
            int wanted = (wanted_bytes + (int)c->bucket_mask)
                         >> c->bucket_shift;
            if (wanted < 1)
                wanted = 1;
            int n = plan_fetch(c, bidx, wanted);

            enum fetch_result r = fetch_buckets(c, s, bidx, n, file_size);

            if (r == FETCH_ERROR || r == FETCH_CANCELED) {
                c->consecutive_failures++;
                if (c->consecutive_failures >= LRU_DISABLE_AFTER_FAIL) {
                    MP_WARN(c,
                            "lru_cache: disabled after %d consecutive backend"
                            " failures (last fetch at bucket %" PRIu64 ")\n",
                            c->consecutive_failures, bidx);
                    c->disabled = true;
                }
                if (total == 0) {
                    // Surface to caller via 0-return; s->error is set by
                    // backend (stream_lavf::seek/fill_buffer write s->error
                    // for hard errors; cancel leaves it 0 -> caller treats
                    // as EOF, which is the established mpv contract).
                    return 0;
                }
                break;
            }

            // Any successful fetch (OK_DATA or EOF) is good news.
            c->consecutive_failures = 0;

            e = table_find(c, bidx);
            if (!e) {
                // FETCH_EOF without inserting any bucket at bidx
                // (e.g. EOF reached before reading any bytes for this idx,
                // or partial bucket from a transport hiccup that we refused
                // to cache). Stop reading.
                break;
            }
            c->miss_count++;
        } else {
            c->hit_count++;
            bucket_touch(c, e);
        }

        if (off >= e->valid_size) {
            // Reading past the end of an EOF bucket
            break;
        }

        int from_bucket = (int)e->valid_size - (int)off;
        int remaining = len - total;
        if (from_bucket > remaining)
            from_bucket = remaining;

        memcpy((uint8_t *)buf + total, e->data + off, (size_t)from_bucket);
        total += from_bucket;
        c->bytes_served += from_bucket;

        if (e->is_eof_bucket && (uint32_t)off + (uint32_t)from_bucket
                                >= e->valid_size)
            break;
    }

    // C3: a successful read clears s->error to match the contract that
    // demux_lavf::pending_stream_error relies on. Without this, a transient
    // backend error during one fetch would persist as "fatal stream error"
    // even after the cache served plenty of valid data and the file reached
    // a clean EOF.
    if (total > 0)
        s->error = 0;

    return total;
}
