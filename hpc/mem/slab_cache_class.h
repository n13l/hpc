/*
 * Expiring block cache over the slab class            Build-time block size
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
 * Expiring block cache built on top of the slab class (<mem/slab_class.h>).
 *
 * This is the build-time-block-size sibling of struct slab_cache in
 * <mem/slab_cache.h>; the expiry model (TTL + idle timeout, reaped back to the
 * free list) is identical and it reuses struct slab_cache_entry. Configure the
 * block size before including, e.g.:
 *
 *     #define SLAB_CLASS_BLOCK_SIZE 2048
 *     #include <mem/slab_cache_class.h>
 */

#ifndef __HPC_MEM_SLAB_CACHE_CLASS_H__
#define __HPC_MEM_SLAB_CACHE_CLASS_H__

#include <hpc/compiler.h>
#include <mem/slab_class.h>
#include <mem/slab_cache.h>       /* struct slab_cache_entry + expiry test */

#include <stdlib.h>
#include <string.h>

__BEGIN_DECLS

struct slab_cache_class {
	struct slab_class cls;         /* backing block allocator            */
	struct slab_cache_entry *ent;  /* per-block metadata [cls.total]     */
	u32 ttl;                       /* default TTL (ms), 0 = none          */
	u32 idle;                      /* default idle timeout (ms), 0 = none */
	u32 live;                      /* live (unexpired) cache blocks       */
	u64 reaps;                     /* blocks expired and reclaimed        */
};

static inline int
slab_cache_class_init(struct slab_cache_class *c,
                      const struct slab_class_policy *policy,
                      u32 ttl, u32 idle)
{
	memset(c, 0, sizeof(*c));
	if (slab_class_init(&c->cls, policy))
		return -1;
	c->ent = (struct slab_cache_entry *)
		SLAB_MEM_CALLOC(c->cls.total, sizeof(struct slab_cache_entry));
	if (!c->ent) {
		slab_class_fini(&c->cls);
		return -1;
	}
	c->ttl = ttl;
	c->idle = idle;
	return 0;
}

static inline void
slab_cache_class_fini(struct slab_cache_class *c)
{
	SLAB_MEM_FREE(c->ent);
	c->ent = NULL;
	slab_class_fini(&c->cls);
}

static inline void *
slab_cache_class_alloc_ex(struct slab_cache_class *c, timestamp_t now,
                          u32 ttl, u32 idle)
{
	struct slab_cache_entry *e;
	void *p = slab_class_alloc(&c->cls);
	if (!p)
		return NULL;
	e = &c->ent[slab_class_index(&c->cls, p)];
	e->ttl_at = ttl ? now + ttl : 0;
	e->atime = now;
	e->idle = idle;
	e->used = 1;
	c->live++;
	return p;
}

static inline void *
slab_cache_class_alloc(struct slab_cache_class *c, timestamp_t now)
{
	return slab_cache_class_alloc_ex(c, now, c->ttl, c->idle);
}

static inline bool
slab_cache_class_touch(struct slab_cache_class *c, void *p, timestamp_t now)
{
	struct slab_cache_entry *e = &c->ent[slab_class_index(&c->cls, p)];
	if (slab_cache_entry_expired(e, now))
		return false;
	e->atime = now;
	return true;
}

static inline bool
slab_cache_class_expired(struct slab_cache_class *c, void *p, timestamp_t now)
{
	return slab_cache_entry_expired(&c->ent[slab_class_index(&c->cls, p)],
	                                now);
}

static inline void
slab_cache_class_free(struct slab_cache_class *c, void *p)
{
	struct slab_cache_entry *e = &c->ent[slab_class_index(&c->cls, p)];
	if (e->used) {
		e->used = 0;
		c->live--;
	}
	slab_class_free(&c->cls, p);
}

static inline u32
slab_cache_class_reap(struct slab_cache_class *c, timestamp_t now)
{
	u32 reaped = 0, i, committed = slab_class_committed(&c->cls);
	for (i = 0; i < committed; i++) {
		struct slab_cache_entry *e = &c->ent[i];
		if (!e->used || !slab_cache_entry_expired(e, now))
			continue;
		e->used = 0;
		c->live--;
		slab_class_free(&c->cls, slab_class_at(&c->cls, i));
		reaped++;
	}
	c->reaps += reaped;
	return reaped;
}

static inline int
slab_cache_class_gc(struct slab_cache_class *c, timestamp_t now)
{
	slab_cache_class_reap(c, now);
	return slab_class_gc(&c->cls, now);
}

static inline u32
slab_cache_class_live(struct slab_cache_class *c)
{
	return c->live;
}

static inline unsigned
slab_cache_class_block_size(struct slab_cache_class *c)
{
	return slab_class_block_size(&c->cls);
}

__END_DECLS

#endif/*__HPC_MEM_SLAB_CACHE_CLASS_H__*/
