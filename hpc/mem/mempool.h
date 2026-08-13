

#ifndef __HPC_MEM_MEMPOOL_H__
#define __HPC_MEM_MEMPOOL_H__

#include <hpc/compiler.h>
#include <hpc/cpu.h>
#include <hpc/log.h>
#include <mem/measure.h>
#include <mem/slab_vm.h>

#include <stdlib.h>
#include <string.h>
#include <assert.h>

__BEGIN_DECLS

#define MEMPOOL_NIL ((u32)~0U)

struct mempool;

struct mempool_policy {
	size_t min;
	size_t max;
	u16 shrink_usage_pct;
	u16 shrink_release_pct;
	timestamp_t shrink_after;
	bool (*check)(struct mempool *pool, void *arg);
	void *arg;
};

#define MEMPOOL_POLICY_STATIC(_max) { \
	.min = (_max), .max = (_max), \
}

#define MEMPOOL_POLICY_GRADUAL(_min, _max, _dwell_ms) { \
	.min = (_min), .max = (_max), \
	.shrink_usage_pct = 25, \
	.shrink_release_pct = 50, \
	.shrink_after = (_dwell_ms), \
}

#define MEMPOOL_POLICY_EAGER(_min, _max, _dwell_ms) { \
	.min = (_min), .max = (_max), \
	.shrink_usage_pct = 25, \
	.shrink_release_pct = 100, \
	.shrink_after = (_dwell_ms), \
}

#ifdef CONFIG_MEM_SLAB_SHRINK_AFTER
#define MEMPOOL_SHRINK_AFTER_DEFAULT ((timestamp_t)CONFIG_MEM_SLAB_SHRINK_AFTER)
#else
#define MEMPOOL_SHRINK_AFTER_DEFAULT ((timestamp_t)30000)
#endif

#if defined(CONFIG_MEM_SLAB_POLICY_STATIC)
# define MEMPOOL_POLICY_DEFAULT_NAME "static"
# define MEMPOOL_POLICY_DEFAULT(_min, _max) MEMPOOL_POLICY_STATIC(_max)
#elif defined(CONFIG_MEM_SLAB_POLICY_EAGER)
# define MEMPOOL_POLICY_DEFAULT_NAME "eager"
# define MEMPOOL_POLICY_DEFAULT(_min, _max) \
	MEMPOOL_POLICY_EAGER(_min, _max, MEMPOOL_SHRINK_AFTER_DEFAULT)
#else
# define MEMPOOL_POLICY_DEFAULT_NAME "gradual"
# define MEMPOOL_POLICY_DEFAULT(_min, _max) \
	MEMPOOL_POLICY_GRADUAL(_min, _max, MEMPOOL_SHRINK_AFTER_DEFAULT)
#endif

static inline int
mempool_policy_preset(struct mempool_policy *p, const char *name,
                      size_t min, size_t max, timestamp_t dwell)
{
	if (!name || !*name)
		name = MEMPOOL_POLICY_DEFAULT_NAME;
	if (!strcmp(name, "static")) {
		struct mempool_policy pol = MEMPOOL_POLICY_STATIC(max);
		*p = pol;
	} else if (!strcmp(name, "gradual")) {
		struct mempool_policy pol = MEMPOOL_POLICY_GRADUAL(min, max, dwell);
		*p = pol;
	} else if (!strcmp(name, "eager")) {
		struct mempool_policy pol = MEMPOOL_POLICY_EAGER(min, max, dwell);
		*p = pol;
	} else {
		return -1;
	}
	return 0;
}

struct mempool {
	u8 *base;
	u64 length;
	u32 total;
	u32 high;
	u32 used;
	u32 idle;
	u32 floor;
	u32 shift;
	u32 cursor;
	timestamp_t idle_since;
	u8 armed;
	struct mempool_policy policy;
	measure_member(mempool)
	u64 *live;
	u64 *dirty;
};


#define __MEMPOOL_WORDS(_n) (((u64)(_n) + 63) / 64)

static inline bool
__mempool_test(const u64 *map, u32 i)
{
	return (map[i >> 6] >> (i & 63)) & 1;
}

static inline void
__mempool_fill(u64 *map, u32 from, u32 to, bool on)
{
	while (from < to) {
		u32 w = from >> 6, lo = from & 63;
		u32 hi = (to - 1) >> 6 == w ? ((to - 1) & 63) + 1 : 64;
		u64 mask = (hi == 64 ? ~(u64)0 : (((u64)1 << hi) - 1)) &
			   ~(((u64)1 << lo) - 1);
		if (on)
			map[w] |= mask;
		else
			map[w] &= ~mask;
		from = (w << 6) + hi;
	}
}

static inline u32
__mempool_count(const u64 *map, u32 from, u32 to)
{
	u32 n = 0;
	while (from < to) {
		u32 w = from >> 6, lo = from & 63;
		u32 hi = (to - 1) >> 6 == w ? ((to - 1) & 63) + 1 : 64;
		u64 mask = (hi == 64 ? ~(u64)0 : (((u64)1 << hi) - 1)) &
			   ~(((u64)1 << lo) - 1);
		n += (u32)__builtin_popcountll(map[w] & mask);
		from = (w << 6) + hi;
	}
	return n;
}

static inline u32
__mempool_find(const u64 *live, u32 from, u32 to, u32 n)
{
	u32 i = from, start = from, run = 0;

	while (i < to) {
		if ((i & 63) == 0 && to - i >= 64) {
			u64 w = live[i >> 6];
			if (w == ~(u64)0) {
				i += 64;
				run = 0;
				start = i;
				continue;
			}
			if (w == 0 && n - run >= 64) {
				i += 64;
				run += 64;
				if (run >= n)
					return start;
				continue;
			}
		}
		if (__mempool_test(live, i)) {
			run = 0;
			start = i + 1;
		} else if (++run >= n) {
			return start;
		}
		i++;
	}
	return MEMPOOL_NIL;
}


static inline u32
__mempool_grains(const struct mempool *p, size_t len)
{
	return (u32)((len + (((size_t)1 << p->shift) - 1)) >> p->shift);
}

static inline u32
__mempool_index(const struct mempool *p, const void *ptr)
{
	return (u32)(((const u8 *)ptr - p->base) >> p->shift);
}

static inline void *
__mempool_at(const struct mempool *p, u32 i)
{
	return p->base + ((size_t)i << p->shift);
}

static inline void
__mempool_claim(struct mempool *p, u32 from, u32 to)
{
	u32 was_idle = __mempool_count(p->dirty, from, to);

	__mempool_fill(p->live, from, to, true);
	__mempool_fill(p->dirty, from, to, false);
	p->idle -= was_idle;
	p->used += to - from;
	if (to > p->high)
		p->high = to;
	if (was_idle != to - from)
		SLAB_VM_POPULATE(__mempool_at(p, from),
				 (size_t)(to - from) << p->shift);
	measure_add(p->measure, used, to - from);
	measure_add(p->measure, resident, (to - from) - was_idle);
}

static inline void
__mempool_yield(struct mempool *p, u32 from, u32 to)
{
	assert(__mempool_count(p->live, from, to) == to - from);
	__mempool_fill(p->live, from, to, false);
	__mempool_fill(p->dirty, from, to, true);
	p->used -= to - from;
	p->idle += to - from;
	if (from < p->cursor)
		p->cursor = from;
	measure_sub(p->measure, used, to - from);
}

static inline u32
__mempool_alloc(struct mempool *p, u32 n)
{
	u32 at;

	if (!n || n > p->total)
		return MEMPOOL_NIL;
	at = __mempool_find(p->live, p->cursor, p->total, n);
	if (at == MEMPOOL_NIL && p->cursor)
		at = __mempool_find(p->live, 0, p->cursor + n - 1 < p->total ?
				    p->cursor + n - 1 : p->total, n);
	if (at == MEMPOOL_NIL)
		return MEMPOOL_NIL;
	__mempool_claim(p, at, at + n);
	p->cursor = at + n < p->total ? at + n : 0;
	return at;
}

static inline u32
__mempool_release(struct mempool *p, u32 want)
{
	u32 released = 0, i = p->high;

	while (i > 0 && released < want) {
		u32 end, start;

		if (!__mempool_test(p->dirty, i - 1)) {
			if ((i & 63) == 0 && p->dirty[(i - 1) >> 6] == 0) {
				i -= 64;
				continue;
			}
			i--;
			continue;
		}
		end = i;
		start = end;
		while (start > 0 && __mempool_test(p->dirty, start - 1) &&
		       end - start < want - released)
			start--;
		SLAB_VM_RELEASE(__mempool_at(p, start),
				(size_t)(end - start) << p->shift);
		__mempool_fill(p->dirty, start, end, false);
		p->idle -= end - start;
		released += end - start;
		i = start;
	}
	if (released) {
		i = p->high;
		while (i > 0) {
			u32 w = (i - 1) >> 6;
			if ((i & 63) == 0 && !p->live[w] && !p->dirty[w]) {
				i -= 64;
				continue;
			}
			if (__mempool_test(p->live, i - 1) ||
			    __mempool_test(p->dirty, i - 1))
				break;
			i--;
		}
		p->high = i;
		measure_inc(p->measure, release);
		measure_add(p->measure, reclaim, released);
		measure_sub(p->measure, resident, released);
		trace1("mempool_release (-%u -> resident %u/%u)",
			released, p->used + p->idle, p->total);
	}
	return released;
}


static inline int
mempool_init_grain(struct mempool *p, size_t grain,
                   const struct mempool_policy *policy)
{
	u32 total;

	if (!grain || (grain & (grain - 1)) || grain < CPU_PAGE_SIZE)
		return -1;
	memset(p, 0, sizeof(*p));
	p->shift = (u32)__builtin_ctzll((unsigned long long)grain);
	p->policy = *policy;
	total = __mempool_grains(p, policy->max);
	if (!total)
		return -1;
	p->total = total;
	p->floor = __mempool_grains(p, policy->min);
	if (p->floor > total)
		p->floor = total;
	p->policy.max = (size_t)total << p->shift;
	p->policy.min = (size_t)p->floor << p->shift;
	p->length = (u64)total << p->shift;

	p->base = (u8 *)SLAB_VM_ALLOC(p->length);
	if (p->base == (u8 *)SLAB_VM_FAILED) {
		p->base = NULL;
		trace4("mempool_init: reserving %lu bytes failed",
			(unsigned long)p->length);
		return -1;
	}
	if (grain >= (size_t)SLAB_HUGE_BYTES)
		SLAB_VM_HUGEPAGE(p->base, p->length);
	p->live = (u64 *)SLAB_MEM_CALLOC(__MEMPOOL_WORDS(total), sizeof(u64));
	p->dirty = (u64 *)SLAB_MEM_CALLOC(__MEMPOOL_WORDS(total), sizeof(u64));
	if (!p->live || !p->dirty) {
		SLAB_MEM_FREE(p->live);
		SLAB_MEM_FREE(p->dirty);
		SLAB_VM_FREE(p->base, p->length);
		p->base = NULL;
		p->live = p->dirty = NULL;
		return -1;
	}
	trace4("mempool_init (grain: %lu, min: %lu, max: %lu): %p",
		(unsigned long)grain, (unsigned long)p->policy.min,
		(unsigned long)p->policy.max, p->base);
	return 0;
}

static inline int
mempool_init(struct mempool *p, const struct mempool_policy *policy)
{
	return mempool_init_grain(p, CPU_PAGE_SIZE, policy);
}

static inline void
mempool_fini(struct mempool *p)
{
	trace4("mempool_fini (total: %u, used: %u, idle: %u): %p",
		p->total, p->used, p->idle, p->base);
	if (p->base)
		SLAB_VM_FREE(p->base, p->length);
	SLAB_MEM_FREE(p->live);
	SLAB_MEM_FREE(p->dirty);
	p->base = NULL;
	p->live = p->dirty = NULL;
}

static inline size_t
mempool_size(const struct mempool *p, size_t len)
{
	return (size_t)__mempool_grains(p, len) << p->shift;
}

static inline void *
mempool_alloc(struct mempool *p, size_t len)
{
	u32 at = __mempool_alloc(p, __mempool_grains(p, len));

	if (at == MEMPOOL_NIL) {
		measure_inc(p->measure, fail);
		return NULL;
	}
	measure_inc(p->measure, alloc);
	return __mempool_at(p, at);
}

static inline void
mempool_free(struct mempool *p, void *ptr, size_t len)
{
	u32 at;

	if (!ptr)
		return;
	at = __mempool_index(p, ptr);
	__mempool_yield(p, at, at + __mempool_grains(p, len));
	measure_inc(p->measure, free);
}

static inline void *
mempool_shrink(struct mempool *p, void *ptr, size_t old, size_t len)
{
	u32 at, no, nn;

	if (!ptr)
		return NULL;
	at = __mempool_index(p, ptr);
	no = __mempool_grains(p, old);
	nn = __mempool_grains(p, len);
	if (nn >= no)
		return ptr;
	__mempool_yield(p, at + nn, at + no);
	measure_inc(p->measure, shrink);
	return nn ? ptr : NULL;
}

static inline void *
mempool_grow(struct mempool *p, void *ptr, size_t old, size_t len)
{
	u32 at, no, nn, moved;
	void *n;

	if (!ptr)
		return mempool_alloc(p, len);
	at = __mempool_index(p, ptr);
	no = __mempool_grains(p, old);
	nn = __mempool_grains(p, len);
	if (nn <= no)
		return mempool_shrink(p, ptr, old, len);

	if ((u64)at + nn <= p->total &&
	    __mempool_find(p->live, at + no, at + nn, nn - no) == at + no) {
		__mempool_claim(p, at + no, at + nn);
		if (p->cursor == at + no)
			p->cursor = at + nn < p->total ? at + nn : 0;
		measure_inc(p->measure, grow);
		return ptr;
	}

	moved = __mempool_alloc(p, nn);
	if (moved == MEMPOOL_NIL) {
		measure_inc(p->measure, fail);
		return NULL;
	}
	n = __mempool_at(p, moved);
	memcpy(n, ptr, old < len ? old : len);
	__mempool_yield(p, at, at + no);
	measure_inc(p->measure, grow);
	measure_inc(p->measure, move);
	return n;
}

static inline bool
__mempool_should_release(const struct mempool *p)
{
	u32 resident = p->used + p->idle;

	if (!p->policy.shrink_release_pct || !p->idle)
		return false;
	if (resident <= p->floor)
		return false;
	return (u64)p->used * 100u <=
	       (u64)p->policy.shrink_usage_pct * resident;
}

static inline int
mempool_gc(struct mempool *p, timestamp_t now)
{
	u32 want, resident;

	if (!__mempool_should_release(p)) {
		p->armed = 0;
		return 0;
	}
	if (!p->armed) {
		p->armed = 1;
		p->idle_since = now;
	} else if (now < p->idle_since) {
		p->idle_since = now;
	}
	if (now - p->idle_since < p->policy.shrink_after)
		return 0;

	want = (u32)(((u64)p->idle * p->policy.shrink_release_pct + 99u) / 100u);
	resident = p->used + p->idle;
	if (resident - want < p->floor)
		want = resident - p->floor;
	if (!want)
		return 0;
	if (p->policy.check && !p->policy.check(p, p->policy.arg))
		return 0;
	p->idle_since = now;
	return -(int)__mempool_release(p, want);
}

static inline void
mempool_set_policy(struct mempool *p, const struct mempool_policy *policy)
{
	size_t max = p->policy.max;

	p->policy = *policy;
	p->policy.max = max;
	p->floor = __mempool_grains(p, policy->min);
	if (p->floor > p->total)
		p->floor = p->total;
	p->policy.min = (size_t)p->floor << p->shift;
}

#ifdef CONFIG_MEASURE
#define mempool_measure_attach(_p, _m) \
	do { (_p)->measure = (_m); } while (0)
#else
#define mempool_measure_attach(_p, _m) ((void)0)
#endif

static inline size_t
mempool_grain_bytes(const struct mempool *p)
{
	return (size_t)1 << p->shift;
}

static inline size_t
mempool_used_bytes(const struct mempool *p)
{
	return (size_t)p->used << p->shift;
}

static inline size_t
mempool_idle_bytes(const struct mempool *p)
{
	return (size_t)p->idle << p->shift;
}

static inline size_t
mempool_resident_bytes(const struct mempool *p)
{
	return (size_t)(p->used + p->idle) << p->shift;
}

static inline size_t
mempool_reserved_bytes(const struct mempool *p)
{
	return (size_t)p->length;
}

static inline size_t
mempool_high_bytes(const struct mempool *p)
{
	return (size_t)p->high << p->shift;
}

__END_DECLS

#endif
