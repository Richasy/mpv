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

// Hard cap on fill_buffer iterations per fetch_at() call, regardless of the
// configured min_fetch target. Stops a misbehaving / extremely slow backend
// from inflating any single miss into an unbounded latency hit.
#define LRU_MAX_FETCH_ITERS       8

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
    uint32_t min_fetch_size;        // target bytes per miss before stopping
                                    // the fill_buffer loop (0 = single shot)
    uint32_t tail_prefetch_size;    // bytes to pull forward on first tail-region
                                    // miss (0 = disabled). Already capped at
                                    // (capacity_buckets - 1) * bucket_size.
    uint64_t tail_threshold;        // miss is "tail" if pos >= file_size - this.

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

    // Bounded fill loop. With min_fetch_size == 0 this degenerates to the
    // legacy single-shot fetch (one fill_buffer per miss). With min_fetch_size
    // > 0 we keep pumping fill_buffer on the SAME backend connection until
    // either we've accumulated at least min_fetch_size bytes, hit the hard
    // iteration cap, the backend returns 0/error/EOF, or the stream is
    // cancelled. All extra iterations share the original seek + TLS
    // handshake -- they are just additional TCP recv() calls on a connection
    // we already paid the latency for.
    int  target = c->min_fetch_size ? (int)c->min_fetch_size : 1;
    if (target > max_want)
        target = max_want;

    int  got = 0;
    int  iters = 0;
    int  last_chunk = 0;
    bool stop_short = false;
    while (got < max_want) {
        // Check cancel at the top of every iteration: a single fill_buffer
        // can block on a slow TCP recv, so we want the earliest possible
        // exit when the user requests interrupt.
        if (mp_cancel_test(s->cancel)) {
            stop_short = true;
            break;
        }
        int chunk = s->fill_buffer(s, e->data + got, max_want - got);
        iters++;
        if (chunk <= 0) {
            last_chunk = chunk;
            stop_short = true;
            break;
        }
        last_chunk = chunk;
        got += chunk;
        c->backend_pos += chunk;

        if (got >= target)
            break;
        if (iters >= LRU_MAX_FETCH_ITERS)
            break;
    }

    MP_DBG(c, "lru_cache: fetch_at bidx=%" PRIu64 " pos=%" PRId64
              " max_want=%d target=%d got=%d iters=%d last_chunk=%d"
              " s_err=%d\n",
              bidx, pos, max_want, target, got, iters, last_chunk,
              s->error);

    if (got <= 0) {
        talloc_free(e);
        return NULL;
    }

    c->bytes_read_from_backend += got;
    e->valid_size = (uint32_t)got;

    // EOF / error handling. stream_lavf::fill_buffer collapses AVERROR_EOF
    // to a negative return WITHOUT setting s->error -- a clean EOF therefore
    // shows up as (last_chunk < 0 && s->error == 0). A hard transport error
    // (avio->error or any other non-EOF AVERROR) sets s->error != 0.
    //
    // When the loop stopped because of a hard transport error AFTER having
    // already accumulated got > 0 bytes, the bytes we collected are valid,
    // but the backend connection is no longer trustworthy. Force a fresh
    // seek on the next fetch (c->backend_pos = -1) and don't mark this as
    // an EOF bucket. We still insert the bucket so the caller can consume
    // the partial data; the error will resurface on the next miss (where
    // the fresh seek either recovers or finally fails into the
    // consecutive_failures self-disable path).
    bool fill_error = stop_short && s->error != 0;
    if (fill_error) {
        c->backend_pos = -1;
    } else if (s->error == 0 && file_size > 0
               && (pos + (int64_t)got) >= file_size)
    {
        e->is_eof_bucket = true;
    }

    cache_insert(c, e);
    return e;
}

// ---- tail prefetch ---------------------------------------------------------
//
// Heuristic: a single miss landing in [file_size - tail_threshold, file_size)
// is almost certainly the demuxer probing the moov atom of a non-faststart
// mp4 sitting at the end of the file. Instead of letting the demuxer yo-yo
// (read 1-10 KiB of moov, seek to mdat, read mdat, seek back to moov for the
// next sample table entry, ...) with a fresh TLS handshake per round-trip,
// we pull `tail_prefetch_size` bytes forward in one go on the connection
// already opened by the originating fetch_at(). This converts O(N) handshakes
// into 1 handshake.
//
// IMPORTANT INVARIANTS:
//   - Caller has already memcpy'd the originating-miss data into the user
//     buffer; prefetch eviction of `start_bucket` is therefore safe.
//   - On entry the backend's logical position equals
//     start_bucket->start_offset + start_bucket->valid_size. We MUST NOT
//     issue s->seek anywhere in this function, because the whole point is
//     to reuse the open connection: the next s->fill_buffer just continues
//     on the same TCP recv stream. (ffmpeg avio_seek to an unchanged pos
//     would be a no-op, but explicitly avoiding it makes the intent clear.)
//   - Repeated invocations are safe: a cancelled / partial prior run may
//     leave behind partial buckets, which subsequent prefetches either
//     skip over (table_find hit) or bail out on (alignment check). The
//     caller's contract continues to hold across re-triggers.

// Read into a bucket starting at e->start_offset + e->valid_size, growing
// valid_size by up to `want` bytes (capped at the bucket-boundary limit).
// Assumes the backend is already positioned at
// e->start_offset + e->valid_size. Returns bytes appended (0 on EOF/error/
// cancel, also when already at the bucket boundary).
//
// NOTE 1: a bucket may start mid-bucket (e->start_offset > bucket_off when
// the originating miss landed inside the bucket), so the relevant cap is
// the FILE bucket-end, not the buffer size.
//
// NOTE 2: this helper is used ONLY by the tail-prefetch path (fetch_at has
// its own inline fill loop). Prefetch is an opportunistic background pull
// that needs to complete in one shot to be useful; capping iterations the
// way fetch_at does would defeat the entire purpose -- when fill_buffer
// returns small chunks (e.g. 2-3 KiB per TCP recv on slow CDNs), an
// 8-iter cap would terminate the prefetch after ~20 KiB and force the
// demuxer back into the yo-yo pattern we are trying to avoid. We rely on
// mp_cancel_test() at the top of every iteration to bound worst-case
// latency under user-driven cancellation.
static int extend_bucket(struct stream_lru_cache *c, struct stream *s,
                         struct bucket_entry *e, int want)
{
    int64_t bucket_off = (int64_t)e->bucket_idx << c->bucket_shift;
    int64_t bucket_end = bucket_off + (int64_t)c->bucket_size;
    int64_t cur_end    = e->start_offset + (int64_t)e->valid_size;
    int room = (int)(bucket_end - cur_end);
    if (room <= 0 || want <= 0)
        return 0;
    if (want > room)
        want = room;

    int got = 0;
    int iters = 0;
    int last_chunk = 0;
    while (got < want) {
        if (mp_cancel_test(s->cancel)) {
            last_chunk = -2;
            break;
        }
        int chunk = s->fill_buffer(s, e->data + e->valid_size + got,
                                   want - got);
        iters++;
        last_chunk = chunk;
        if (chunk <= 0)
            break;
        got += chunk;
        c->backend_pos += chunk;
    }

    if (got > 0) {
        e->valid_size += (uint32_t)got;
        c->bytes_read_from_backend += got;
    }

    MP_DBG(c, "lru_cache: extend_bucket bidx=%" PRIu64
              " want=%d got=%d iters=%d last_chunk=%d s_err=%d\n",
              e->bucket_idx, want, got, iters, last_chunk, s->error);

    // Mirror fetch_at's hard-error policy: when the backend hit a transport
    // error mid-fill, the data we already pulled is valid but the connection
    // is no longer trustworthy. Force a fresh seek on the next miss so we
    // don't continue assuming logical/backend positions still line up.
    // (Pure EOF goes through stream_lavf::fill_buffer as chunk == -1 WITHOUT
    // setting s->error, so we leave backend_pos alone in that case.)
    if (s->error != 0)
        c->backend_pos = -1;

    return got;
}

// Allocate, fill and insert a fresh bucket starting at `pos`, drawing up to
// `want` bytes from the already-positioned backend. Assumes c->backend_pos
// already equals `pos` (no seek). Returns the inserted bucket, or NULL if
// no bytes could be read (or pos is past the bucket boundary).
static struct bucket_entry *fill_new_bucket(struct stream_lru_cache *c,
                                            struct stream *s,
                                            uint64_t bidx, int64_t pos,
                                            int want, int64_t file_size)
{
    int64_t bucket_off = (int64_t)bidx << c->bucket_shift;
    int max_fit = (int)c->bucket_size - (int)(pos - bucket_off);
    if (max_fit <= 0)
        return NULL;
    if (want > max_fit)
        want = max_fit;
    if (want < 1)
        want = 1;

    struct bucket_entry *e = bucket_alloc(c);
    e->bucket_idx   = bidx;
    e->start_offset = pos;
    e->valid_size   = 0;
    e->is_eof_bucket = false;

    int got = extend_bucket(c, s, e, want);
    if (got <= 0) {
        talloc_free(e);
        return NULL;
    }

    if (s->error == 0 && file_size > 0
        && (pos + (int64_t)got) >= file_size)
    {
        e->is_eof_bucket = true;
    }

    cache_insert(c, e);
    return e;
}

// Run a tail prefetch. `seed` is the originating-miss bucket the caller is
// about to return; its data is already copied to the user buffer. We extend
// `seed` to its bucket boundary, then allocate and fill consecutive buckets
// forward until we've accumulated `tail_prefetch_size` bytes, hit EOF, or
// the connection short-reads and breaks the "no seek" invariant.
//
// Returns true if the prefetch ran to natural completion (target reached or
// EOF). Returns false on cancel / partial-failure so that a future tail miss
// can retry.
static bool run_tail_prefetch(struct stream_lru_cache *c, struct stream *s,
                              struct bucket_entry *seed, int64_t origin_pos,
                              int64_t file_size)
{
    int64_t pf_target = origin_pos + (int64_t)c->tail_prefetch_size;
    if (file_size > 0 && pf_target > file_size)
        pf_target = file_size;

    // Phase 1: extend `seed` to its bucket boundary if room remains.
    // extend_bucket itself caps at the file bucket-end, so we just pass the
    // remaining byte target; any over-ask is clamped internally.
    int64_t cur_end = seed->start_offset + (int64_t)seed->valid_size;
    if (cur_end < pf_target) {
        int64_t extra64 = pf_target - cur_end;
        // The cap matters only for the practical upper bound (tail_prefetch
        // is M_RANGE-limited to 64 MiB), but be defensive in case future
        // configuration widens that range.
        int extra = (extra64 > (int64_t)c->bucket_size)
                    ? (int)c->bucket_size : (int)extra64;
        if (extra > 0)
            extend_bucket(c, s, seed, extra);
        cur_end = seed->start_offset + (int64_t)seed->valid_size;
    }
    if (mp_cancel_test(s->cancel))
        return false;

    // If phase 1 reached file_size, mark seed as an EOF bucket so future
    // reads at this bidx don't waste a backend roundtrip rediscovering EOF.
    // (fill_new_bucket does the same for phase 2 buckets.)
    if (s->error == 0 && file_size > 0 && cur_end >= file_size)
        seed->is_eof_bucket = true;

    // Phase 2: continue with fresh buckets. Each iteration MUST start at a
    // bucket-aligned offset that equals c->backend_pos -- if extend_bucket
    // (or a previous iteration) short-read, the alignment breaks and we
    // bail out: re-fetching now would require a seek and defeat the whole
    // point of prefetching.
    int64_t pf_pos = cur_end;
    int64_t pf_phase2_start = pf_pos;
    int  buckets_done = 0;
    bool cancelled = false;
    const char *exit_reason = "target reached";
    while (pf_pos < pf_target) {
        if (mp_cancel_test(s->cancel)) {
            cancelled = true;
            exit_reason = "cancel";
            break;
        }
        if (pf_pos != c->backend_pos) {
            exit_reason = "backend pos drift";
            break;
        }
        uint64_t pf_bidx   = (uint64_t)pf_pos >> c->bucket_shift;
        int64_t  bucket_off = (int64_t)pf_bidx << c->bucket_shift;
        if (pf_pos != bucket_off) {
            exit_reason = "short read (mid-bucket)";
            break;
        }

        struct bucket_entry *existing = table_find(c, pf_bidx);
        if (existing) {
            // Bucket already cached. If it covers from bucket_off forward,
            // jump over it; otherwise we have a non-aligned slice and we'd
            // have to drop+refetch which needs a seek -> bail.
            if (existing->start_offset == bucket_off) {
                pf_pos = existing->start_offset
                         + (int64_t)existing->valid_size;
                bucket_touch(c, existing);
                if (pf_pos != c->backend_pos) {
                    exit_reason = "existing bucket short of backend";
                    break;
                }
                continue;
            }
            exit_reason = "non-aligned cached slice";
            break;
        }

        int want = (int)(pf_target - pf_pos);
        struct bucket_entry *e = fill_new_bucket(c, s, pf_bidx, pf_pos,
                                                 want, file_size);
        if (!e) {
            exit_reason = "fill_new_bucket failed";
            break;
        }
        int64_t new_end = e->start_offset + (int64_t)e->valid_size;
        if (new_end <= pf_pos) {
            exit_reason = "no progress";
            break;
        }
        pf_pos = new_end;
        buckets_done++;
        if (e->is_eof_bucket) {
            exit_reason = "eof";
            MP_VERBOSE(c, "lru_cache: tail prefetch phase2 hit EOF after"
                          " %d buckets, %" PRId64 " bytes pulled\n",
                       buckets_done, pf_pos - pf_phase2_start);
            return true;
        }
    }

    MP_VERBOSE(c, "lru_cache: tail prefetch phase2 exit reason=%s"
                  " buckets=%d phase2_pulled=%" PRId64 "B pf_pos=%" PRId64
                  " pf_target=%" PRId64 "\n",
               exit_reason, buckets_done,
               pf_pos - pf_phase2_start, pf_pos, pf_target);

    return !cancelled && pf_pos >= pf_target;
}

// ---- public API ------------------------------------------------------------

struct stream_lru_cache *stream_lru_cache_create(void *talloc_parent,
                                                 struct mp_log *log,
                                                 size_t capacity_bytes,
                                                 uint32_t bucket_size,
                                                 uint32_t min_fetch_size,
                                                 uint32_t tail_prefetch,
                                                 uint64_t tail_threshold)
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

    // Clamp min_fetch to the bucket size; a fetch cannot legally cross a
    // bucket boundary, so asking for more would be silently capped anyway.
    if (min_fetch_size > bucket_size)
        min_fetch_size = bucket_size;

    if (capacity_bytes < (size_t)bucket_size * 4)
        capacity_bytes = (size_t)bucket_size * 4;

    struct stream_lru_cache *c =
        talloc_zero(talloc_parent, struct stream_lru_cache);
    c->log              = log;
    c->bucket_size      = bucket_size;
    c->bucket_mask      = (uint64_t)bucket_size - 1;
    c->min_fetch_size   = min_fetch_size;
    c->capacity_bytes   = capacity_bytes;
    c->capacity_buckets = capacity_bytes / bucket_size;
    if (c->capacity_buckets < 4)
        c->capacity_buckets = 4;
    c->backend_pos      = -1;

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
    c->tail_threshold     = tail_threshold;

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
               " tail_threshold=%" PRIu64 "\n",
               c->capacity_bytes, c->capacity_buckets,
               c->bucket_size, c->table_capacity,
               c->min_fetch_size, c->tail_prefetch_size, c->tail_threshold);

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

    // Tail prefetch (after data is safely copied to the caller buffer, so
    // any eviction triggered by prefetch inserts cannot corrupt our return).
    // Re-triggered on EVERY miss whose pos falls in the tail region. The
    // natural rate limiter is cache coverage itself: once a 4 MiB window
    // around `pos` has been pulled in, subsequent reads inside it hit and
    // do not re-trigger. Only when the demuxer touches a previously
    // unseen tail position (e.g. after a user seek lands in a new region
    // of the moov atom) does a new prefetch fire.
    //
    // fetch_at left the backend pointing at
    //   fresh->start_offset + fresh->valid_size,
    // which is exactly where the prefetch wants to continue: no extra seek,
    // no extra TLS handshake.
    if (c->tail_prefetch_size > 0
        && file_size > 0
        && pos >= file_size - (int64_t)c->tail_threshold
        && c->backend_pos
            == fresh->start_offset + (int64_t)fresh->valid_size)
    {
        bool done = run_tail_prefetch(c, s, fresh, pos, file_size);
        MP_VERBOSE(c, "lru_cache: tail prefetch %s at pos=%" PRId64
                      " (file_size=%" PRId64 ", target=%u bytes)\n",
                   done ? "completed" : "interrupted",
                   pos, file_size, c->tail_prefetch_size);
        // Clear any error/cancel residue from the prefetch path so the
        // caller's successful read doesn't surface as a spurious failure.
        s->error = 0;
    }

    return copy;
}
