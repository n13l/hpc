/*
 * Expiring block cache over the dynamic slab            Runtime block size
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2012-2026                          Daniel Kubec <niel@rtfm.cz>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"),to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/*
 * Expiring block cache built on top of the dynamic slab (<mem/slab.h>).
 *
 * A cache block expires when either deadline passes:
 *   - TTL:  it has lived longer than @ttl since it was allocated;
 *   - idle: it has not been touched for @idle.
 * slab_cache_touch() resets the idle window (but not the absolute TTL). Expired
 * blocks are reclaimed by slab_cache_reap()/slab_cache_gc(), which return them
 * to the slab free list to be handed out again - "marked as free block again".
 *
 * Per-block expiry metadata is kept out-of-band in an array indexed by block
 * index (like the slab's occupancy bitmap), so an allocated block stays fully
 * usable as payload. All times are in the same unit (milliseconds) as the @now
 * passed in, matching slab_gc(). For a build-time block size use struct
 * slab_cache_class in <mem/slab_cache_class.h>.
 */

#ifndef __HPC_MEM_SLAB_CACHE_H__
#define __HPC_MEM_SLAB_CACHE_H__

#include <hpc/compiler.h>
#include <mem/slab.h>

#include <stdlib.h>
#include <string.h>

__BEGIN_DECLS

/*
 * Per-block expiry metadata (shared with struct slab_cache_class). Kept
 * out-of-band, indexed by block index.
 *
 * @ttl_at absolute TTL deadline; 0 disables the TTL
 * @atime  last touch time; the idle window is [atime, atime + idle)
 * @idle   idle timeout; 0 disables idle expiry
 * @used   the slot holds a live cache block
 */
#ifndef HPC_SLAB_CACHE_ENTRY_DEFINED
#define HPC_SLAB_CACHE_ENTRY_DEFINED
struct slab_cache_entry {
	timestamp_t ttl_at;
	timestamp_t atime;
	u32 idle;
	u32 used;
};

static inline bool
slab_cache_entry_expired(const struct slab_cache_entry *e, timestamp_t now)
{
	if (!e->used)
		return false;
	if (e->ttl_at && now >= e->ttl_at)
		return true;
	if (e->idle && now >= e->atime && (now - e->atime) >= e->idle)
		return true;
	return false;
}
#endif /* HPC_SLAB_CACHE_ENTRY_DEFINED */

struct slab_cache {
	struct slab slab;              /* backing block allocator            */
	struct slab_cache_entry *ent;  /* per-block metadata [slab.total]    */
	u32 ttl;                       /* default TTL (ms), 0 = none          */
	u32 idle;                      /* default idle timeout (ms), 0 = none */
	u32 live;                      /* live (unexpired) cache blocks       */
	u64 reaps;                     /* blocks expired and reclaimed        */
};

/*
 * slab_cache_init - build an expiring cache with @ttl / @idle defaults (ms).
 *
 * @block_size and @policy are handed to slab_init(). Returns 0 on success,
 * -1 on failure.
 */
static inline int
slab_cache_init(struct slab_cache *c, unsigned block_size,
                const struct slab_policy *policy, u32 ttl, u32 idle)
{
	memset(c, 0, sizeof(*c));
	if (slab_init(&c->slab, block_size, policy))
		return -1;
	c->ent = (struct slab_cache_entry *)
		SLAB_MEM_CALLOC(c->slab.total, sizeof(struct slab_cache_entry));
	if (!c->ent) {
		slab_fini(&c->slab);
		return -1;
	}
	c->ttl = ttl;
	c->idle = idle;
	return 0;
}

static inline void
slab_cache_fini(struct slab_cache *c)
{
	SLAB_MEM_FREE(c->ent);
	c->ent = NULL;
	slab_fini(&c->slab);
}

/*
 * slab_cache_alloc_ex - allocate a block with explicit TTL and idle timeout.
 *
 * Returns the block (full block_size bytes of payload) or NULL when the slab
 * is exhausted and the policy will not grow.
 */
static inline void *
slab_cache_alloc_ex(struct slab_cache *c, timestamp_t now, u32 ttl, u32 idle)
{
	struct slab_cache_entry *e;
	void *p = slab_alloc(&c->slab);
	if (!p)
		return NULL;
	e = &c->ent[slab_index(&c->slab, p)];
	e->ttl_at = ttl ? now + ttl : 0;
	e->atime = now;
	e->idle = idle;
	e->used = 1;
	c->live++;
	return p;
}

/* slab_cache_alloc - allocate a block using the cache's default TTL / idle. */
static inline void *
slab_cache_alloc(struct slab_cache *c, timestamp_t now)
{
	return slab_cache_alloc_ex(c, now, c->ttl, c->idle);
}

/*
 * slab_cache_touch - mark a block used at @now, resetting its idle window.
 *
 * Returns false if the block has already expired (the caller should treat it
 * as gone and let the next reap collect it); true if it is still live.
 */
static inline bool
slab_cache_touch(struct slab_cache *c, void *p, timestamp_t now)
{
	struct slab_cache_entry *e = &c->ent[slab_index(&c->slab, p)];
	if (slab_cache_entry_expired(e, now))
		return false;
	e->atime = now;
	return true;
}

/* slab_cache_expired - has this block passed its TTL or idle deadline? */
static inline bool
slab_cache_expired(struct slab_cache *c, void *p, timestamp_t now)
{
	return slab_cache_entry_expired(&c->ent[slab_index(&c->slab, p)], now);
}

/* slab_cache_free - explicitly release a block back to the slab. */
static inline void
slab_cache_free(struct slab_cache *c, void *p)
{
	struct slab_cache_entry *e = &c->ent[slab_index(&c->slab, p)];
	if (e->used) {
		e->used = 0;
		c->live--;
	}
	slab_free(&c->slab, p);
}

/*
 * slab_cache_reap - free every block that has expired at @now.
 *
 * Reclaimed blocks return to the slab free list. Returns the number reaped.
 */
static inline u32
slab_cache_reap(struct slab_cache *c, timestamp_t now)
{
	u32 reaped = 0, i, committed = slab_committed(&c->slab);
	for (i = 0; i < committed; i++) {
		struct slab_cache_entry *e = &c->ent[i];
		if (!e->used || !slab_cache_entry_expired(e, now))
			continue;
		e->used = 0;
		c->live--;
		slab_free(&c->slab, slab_at(&c->slab, i));
		reaped++;
	}
	c->reaps += reaped;
	return reaped;
}

/*
 * slab_cache_gc - reap expired blocks, then apply the slab grow/shrink policy.
 *
 * Returns the signed change in committed blocks from the slab_gc() step (the
 * reap itself only moves blocks onto the free list). Reaping lowers usage, so
 * this is where freed memory is actually returned to the OS.
 */
static inline int
slab_cache_gc(struct slab_cache *c, timestamp_t now)
{
	slab_cache_reap(c, now);
	return slab_gc(&c->slab, now);
}

static inline u32
slab_cache_live(struct slab_cache *c)
{
	return c->live;
}

static inline unsigned
slab_cache_block_size(struct slab_cache *c)
{
	return slab_block_size(&c->slab);
}

__END_DECLS

#endif/*__HPC_MEM_SLAB_CACHE_H__*/
