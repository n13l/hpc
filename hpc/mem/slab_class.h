/*
 * High performance slab allocator                    Build-time block size
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
 * Slab class - a slab allocator whose block size is fixed at build time.
 *
 * struct slab_class is the build-time sibling of the dynamic struct slab in
 * <mem/slab.h>. The block size is a power-of-two constant baked in at compile
 * time, so every << shift folds to a constant and the class carries no
 * per-instance shift field. Configure it once before including this header:
 *
 *     #define SLAB_CLASS_BLOCK_SIZE 2048   // must be a power of two
 *     #include <mem/slab_class.h>
 *
 * or specify the shift directly:
 *
 *     #define SLAB_CLASS_SHIFT 11          // 1 << 11 == 2048
 *     #include <mem/slab_class.h>
 *
 * The default is a 2048 byte block, which holds a 1500 byte Ethernet MTU - the
 * common case for a TCP reordering buffer on a build with a fixed link. For a
 * link whose MTU is only known at run time, use the dynamic <mem/slab.h>.
 *
 * The reservation model, grow/shrink policy and check() gate are identical to
 * the dynamic variant: the maximum working set is reserved once (stable block
 * addresses), and the committed prefix grows and shrinks under a preconfigured
 * struct slab_class_policy. That includes the reservation backend - the
 * SLAB_VM_* / SLAB_MEM_* hooks in <mem/slab_vm.h>, shared with the dynamic
 * variant, so a custom allocator or CONFIG_MEM_HUGEPAGE / CONFIG_MEM_POPULATE is
 * configured once and applies to both.
 */

#ifndef __HPC_MEM_SLAB_CLASS_H__
#define __HPC_MEM_SLAB_CLASS_H__

#include <hpc/compiler.h>
#include <hpc/cpu.h>
#include <hpc/log.h>
#include <hpc/bitset.h>
/* SLAB_VM_* / SLAB_MEM_* reservation backend, VM_PAGE_*, SLAB_NIL. */
#include <mem/slab_vm.h>

#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

__BEGIN_DECLS

#ifndef SLAB_CLASS_SHIFT
# ifdef SLAB_CLASS_BLOCK_SIZE
#  define SLAB_CLASS_SHIFT ((unsigned)__builtin_ctz(SLAB_CLASS_BLOCK_SIZE))
# else
#  define SLAB_CLASS_SHIFT 11u          /* 2048B block, holds a 1500B MTU */
# endif
#endif

/* Effective, compile-time constant block size in bytes. */
#define SLAB_CLASS_BSIZE (1u << (SLAB_CLASS_SHIFT))

/*
 * Blocks per release grain, as a compile-time constant - the class spelling of
 * slab_grain_for() (see <mem/slab_vm.h>). Every rounding below therefore folds
 * away when a block is a grain or larger.
 */
#define SLAB_CLASS_GRAIN \
	(SLAB_CLASS_BSIZE >= (unsigned)SLAB_GRAIN_BYTES \
		? 1u : (unsigned)(SLAB_GRAIN_BYTES) / SLAB_CLASS_BSIZE)

#ifdef SLAB_CLASS_BLOCK_SIZE
_Static_assert((SLAB_CLASS_BLOCK_SIZE & (SLAB_CLASS_BLOCK_SIZE - 1)) == 0,
	"SLAB_CLASS_BLOCK_SIZE must be a power of two");
_Static_assert(SLAB_CLASS_BSIZE == (unsigned)(SLAB_CLASS_BLOCK_SIZE),
	"SLAB_CLASS_SHIFT does not match SLAB_CLASS_BLOCK_SIZE");
#endif

struct slab_class;

/*
 * See struct slab_policy in <mem/slab.h> for the full semantics. The shrink
 * rule reads: "when live usage stays at/under @shrink_usage_pct for
 * @shrink_after, return @shrink_release_pct of the free blocks to the OS."
 */
struct slab_class_policy {
	u32 min;              /* shrink floor (blocks)                        */
	u32 max;              /* grow ceiling / reservation size (blocks)     */
	u32 grow_step;        /* blocks committed per grow step (0 -> 1)      */
	u16 grow_usage_pct;   /* grow when used >= this % of committed        */
	u16 shrink_usage_pct; /* shrink when used <= this % of committed      */
	u16 shrink_release_pct; /* release this % of free blocks per shrink   */
	timestamp_t shrink_after; /* idle dwell (ms) before releasing memory  */
	bool (*check)(struct slab_class *sc, int grow, void *arg);
	void *arg;
};

struct slab_class_stat {
	u64 grows;
	u64 shrinks;
	u64 fails;
	u32 used;
	u32 peak;
};

struct slab_class {
	u64 length;           /* reserved region length in bytes              */
	u32 list;             /* head index of the free list                  */
	u32 avail;            /* free blocks on the list                      */
	u32 committed;        /* committed blocks [0, committed)               */
	u32 total;            /* reserved (maximum) blocks                    */
	timestamp_t idle_since; /* when usage first dropped to the low mark    */
	u8 idle;              /* idle-shrink timer armed                       */
	struct slab_class_policy policy;
	struct slab_class_stat stat;
	u8 *map;              /* occupancy bitmap, one bit per reserved block */
	void *page;           /* base of the reservation                      */
};

/* Free-list node overlay: valid only while the block is free. */
struct slab_class_node {
	u32 avail;
};

_Static_assert(SLAB_CLASS_BSIZE >= sizeof(struct slab_class_node),
	"SLAB_CLASS block size too small for the free-list node");

static inline void *
slab_class_at(struct slab_class *sc, u32 index)
{
	if (index == SLAB_NIL)
		return NULL;
	return (u8 *)sc->page + ((size_t)index << SLAB_CLASS_SHIFT);
}

static inline u32
slab_class_index(struct slab_class *sc, void *p)
{
	return (u32)(((u8 *)p - (u8 *)sc->page) >> SLAB_CLASS_SHIFT);
}

/*
 * Round the policy to whole grains: max (and so the reservation) and min up, and
 * grow_step up to at least one grain, so the committed prefix always ends on a
 * grain boundary. Same rule and same reasons as __slab_round_policy() in
 * <mem/slab.h>; read the rounded values back with slab_class_policy_min()/max().
 */
static inline void
slab_class_round_policy(struct slab_class *sc)
{
	sc->policy.max = slab_grain_round(sc->policy.max, SLAB_CLASS_GRAIN);
	sc->policy.min = slab_grain_round(sc->policy.min, SLAB_CLASS_GRAIN);
	if (sc->policy.min > sc->policy.max)
		sc->policy.min = sc->policy.max;
	sc->policy.grow_step = slab_grain_round(sc->policy.grow_step,
	                                        SLAB_CLASS_GRAIN);
	if (sc->policy.grow_step < SLAB_CLASS_GRAIN)
		sc->policy.grow_step = SLAB_CLASS_GRAIN;
}

static inline void
slab_class_madvise_tail(struct slab_class *sc, u32 from, u32 to)
{
	uintptr_t pg = CPU_PAGE_SIZE;
	uintptr_t base = (uintptr_t)sc->page;
	uintptr_t start = base + ((size_t)from << SLAB_CLASS_SHIFT);
	uintptr_t end = base + ((size_t)to << SLAB_CLASS_SHIFT);
	uintptr_t a = align_to(start, pg);
	uintptr_t b = end & ~(pg - 1);
	if (b > a)
		SLAB_VM_RELEASE((void *)a, (size_t)(b - a));
}

/*
 * Pre-fault the blocks a grow is about to commit; see __slab_populate() in
 * <mem/slab.h> for why this is confined to sub-page blocks. Here the test is a
 * compile-time constant, so for a block larger than a page the whole call folds
 * away.
 */
static inline void
slab_class_populate(struct slab_class *sc, u32 from, u32 to)
{
	if (SLAB_CLASS_BSIZE > CPU_PAGE_SIZE || to <= from)
		return;
	SLAB_VM_POPULATE((u8 *)sc->page + ((size_t)from << SLAB_CLASS_SHIFT),
			 (size_t)(to - from) << SLAB_CLASS_SHIFT);
}

static inline u32
slab_class_grow(struct slab_class *sc, u32 n)
{
	u32 grew = 0;
	u32 room, plan;

	/* commit whole grains; min/max/grow_step are already rounded */
	n = slab_grain_round(n, SLAB_CLASS_GRAIN);

	/* what the loop will commit, mirrored up front so it can be prefaulted */
	room = sc->policy.max < sc->total ? sc->policy.max : sc->total;
	plan = sc->committed < room ? room - sc->committed : 0;
	if (plan > n)
		plan = n;
	if (plan)
		slab_class_populate(sc, sc->committed, sc->committed + plan);

	while (grew < n && sc->committed < sc->policy.max &&
	       sc->committed < sc->total) {
		u32 idx = sc->committed++;
		struct slab_class_node *node = (struct slab_class_node *)
			slab_class_at(sc, idx);
		node->avail = sc->list;
		sc->list = idx;
		sc->avail++;
		grew++;
	}
	if (grew)
		sc->stat.grows++;
	return grew;
}

static inline u32
slab_class_grow_policy(struct slab_class *sc)
{
	u32 step;
	if (sc->committed >= sc->policy.max)
		return 0;
	if (sc->policy.check && !sc->policy.check(sc, 1, sc->policy.arg))
		return 0;
	step = sc->policy.grow_step ? sc->policy.grow_step : SLAB_CLASS_GRAIN;
	return slab_class_grow(sc, step);
}

/* Is every block of the grain ending at @end free? */
static inline bool
slab_class_grain_free(struct slab_class *sc, u32 end)
{
	u32 i;
	if (end < SLAB_CLASS_GRAIN)
		return false;
	for (i = end - SLAB_CLASS_GRAIN; i < end; i++)
		if (BITSET_TEST(sc->map, i))
			return false;
	return true;
}

static inline u32
slab_class_shrink(struct slab_class *sc, u32 n)
{
	u32 floor = sc->policy.min;
	u32 used = sc->committed - sc->avail;
	u32 old = sc->committed;
	u32 reclaimed = 0, head = SLAB_NIL, cnt = 0, i;

	if (floor < used)
		floor = used;

	/* whole grains only, and only grains in which every block is free */
	while (reclaimed + SLAB_CLASS_GRAIN <= n &&
	       sc->committed >= floor + SLAB_CLASS_GRAIN &&
	       slab_class_grain_free(sc, sc->committed)) {
		sc->committed -= SLAB_CLASS_GRAIN;
		reclaimed += SLAB_CLASS_GRAIN;
	}
	if (!reclaimed)
		return 0;

	for (i = sc->list; i != SLAB_NIL; ) {
		struct slab_class_node *node = (struct slab_class_node *)
			slab_class_at(sc, i);
		u32 next = node->avail;
		if (i < sc->committed) {
			node->avail = head;
			head = i;
			cnt++;
		}
		i = next;
	}
	sc->list = head;
	sc->avail = cnt;

	slab_class_madvise_tail(sc, sc->committed, old);
	sc->stat.shrinks++;
	return reclaimed;
}

static inline bool
slab_class_should_grow(struct slab_class *sc)
{
	u32 c = sc->committed ? sc->committed : 1;
	u32 used = sc->committed - sc->avail;
	if (sc->committed >= sc->policy.max)
		return false;
	if (!sc->policy.grow_usage_pct)
		return false;
	return (u32)used * 100u >= (u32)sc->policy.grow_usage_pct * c;
}

static inline bool
slab_class_should_shrink(struct slab_class *sc)
{
	u32 c = sc->committed ? sc->committed : 1;
	u32 used = sc->committed - sc->avail;
	if (sc->committed <= sc->policy.min)
		return false;
	if (!sc->policy.shrink_release_pct)
		return false;
	return (u32)used * 100u <= (u32)sc->policy.shrink_usage_pct * c;
}

static inline void *
slab_class_alloc(struct slab_class *sc)
{
	struct slab_class_node *node = (struct slab_class_node *)
		slab_class_at(sc, sc->list);
	u32 idx;
	if (!node) {
		if (!slab_class_grow_policy(sc)) {
			sc->stat.fails++;
			return NULL;
		}
		node = (struct slab_class_node *)slab_class_at(sc, sc->list);
		if (!node) {
			sc->stat.fails++;
			return NULL;
		}
	}
	sc->list = node->avail;
	sc->avail--;
	idx = slab_class_index(sc, node);
	BITSET_SET(sc->map, idx);
	if (++sc->stat.used > sc->stat.peak)
		sc->stat.peak = sc->stat.used;
	return node;
}

static inline void
slab_class_free(struct slab_class *sc, void *p)
{
	u32 idx = slab_class_index(sc, p);
	struct slab_class_node *node = (struct slab_class_node *)p;
	BITSET_CLR(sc->map, idx);
	node->avail = sc->list;
	sc->list = idx;
	sc->avail++;
	sc->stat.used--;
}

/*
 * slab_class_gc - apply the grow/shrink policy once.
 *
 * @now is the current monotonic time (milliseconds) driving the idle-shrink
 * dwell; see struct slab_class_policy. Returns the signed change in committed
 * blocks.
 */
static inline int
slab_class_gc(struct slab_class *sc, timestamp_t now)
{
	u32 want;

	if (slab_class_should_grow(sc)) {
		sc->idle = 0;
		return (int)slab_class_grow_policy(sc);
	}
	if (!slab_class_should_shrink(sc)) {
		sc->idle = 0;
		return 0;
	}

	if (!sc->idle) {
		sc->idle = 1;
		sc->idle_since = now;
	} else if (now < sc->idle_since) {
		sc->idle_since = now;
	}
	if (now - sc->idle_since < sc->policy.shrink_after)
		return 0;

	/* Release shrink_release_pct of the free blocks, rounded to a whole grain. */
	want = (u32)((u64)sc->avail * sc->policy.shrink_release_pct / 100u);
	want = slab_grain_round(want, SLAB_CLASS_GRAIN);
	if (want > sc->avail)
		want = sc->avail;
	if (want < SLAB_CLASS_GRAIN)
		return 0;

	if (sc->policy.check && !sc->policy.check(sc, 0, sc->policy.arg))
		return 0;

	sc->idle_since = now;
	return -(int)slab_class_shrink(sc, want);
}

/*
 * slab_class_set_policy - replace the policy while the class is live.
 *
 * See slab_set_policy() in <mem/slab.h>: single-writer, no locking; sequenced
 * after any other call it is safe, leaves allocated blocks and their contents
 * untouched, caps max/min at the fixed reservation and restarts the dwell.
 */
static inline void
slab_class_set_policy(struct slab_class *sc,
                      const struct slab_class_policy *policy)
{
	sc->policy = *policy;
	slab_class_round_policy(sc);
	if (sc->policy.max > sc->total)
		sc->policy.max = sc->total;
	if (sc->policy.min > sc->total)
		sc->policy.min = sc->total;
	sc->idle = 0;
}

/*
 * slab_class_init - reserve a class of SLAB_CLASS_BSIZE byte blocks.
 *
 * Returns 0 on success, -1 on failure. The reservation covers policy->max
 * blocks; policy->min blocks are committed immediately.
 */
static inline int
slab_class_init(struct slab_class *sc, const struct slab_class_policy *policy)
{
	memset(sc, 0, sizeof(*sc));
	sc->policy = *policy;
	slab_class_round_policy(sc);
	sc->total = sc->policy.max;
	sc->list = SLAB_NIL;
	sc->length = (u64)sc->total << SLAB_CLASS_SHIFT;

	sc->page = SLAB_VM_ALLOC(sc->length);
	if (sc->page == SLAB_VM_FAILED) {
		sc->page = NULL;
		return -1;
	}
	SLAB_VM_HUGEPAGE(sc->page, sc->length);
	sc->map = (u8 *)SLAB_MEM_CALLOC(BITSET_SIZE(sc->total), 1);
	if (!sc->map) {
		SLAB_VM_FREE(sc->page, sc->length);
		sc->page = NULL;
		return -1;
	}
	if (policy->min)
		slab_class_grow(sc, policy->min);
	sc->stat.grows = 0;

	trace1("slab_class_init (bsize: %u, min: %u, max: %u): %p",
		(unsigned)SLAB_CLASS_BSIZE, policy->min, policy->max, sc->page);
	return 0;
}

static inline void
slab_class_fini(struct slab_class *sc)
{
	if (sc->page)
		SLAB_VM_FREE(sc->page, sc->length);
	SLAB_MEM_FREE(sc->map);
	sc->page = NULL;
	sc->map = NULL;
}

static inline unsigned
slab_class_block_size(struct slab_class *sc)
{
	(void)sc;
	return SLAB_CLASS_BSIZE;
}

static inline u32
slab_class_grain(struct slab_class *sc)
{
	(void)sc;
	return SLAB_CLASS_GRAIN;
}

/* The policy as enforced, after the grain rounding done at init. */
static inline u32
slab_class_policy_min(struct slab_class *sc)
{
	return sc->policy.min;
}

static inline u32
slab_class_policy_max(struct slab_class *sc)
{
	return sc->policy.max;
}

static inline u32
slab_class_committed(struct slab_class *sc)
{
	return sc->committed;
}

static inline u32
slab_class_avail(struct slab_class *sc)
{
	return sc->avail;
}

static inline u32
slab_class_used(struct slab_class *sc)
{
	return sc->committed - sc->avail;
}

__END_DECLS

#endif/*__HPC_MEM_SLAB_CLASS_H__*/
