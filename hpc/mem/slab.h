/*
 * High performance slab allocator                       Runtime block size
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
 * Dynamic slab allocator - fixed-size blocks whose size is chosen at run time.
 *
 * struct slab is the allocator handle; slab_init() sets the block size (a power
 * of two, typically derived from the path MTU - see slab_shift_for()). Every
 * block is addressed by index using a single shift, so allocation and address
 * translation are branch-free bit shifts. For a block size fixed at build time
 * use struct slab_class in <mem/slab_class.h> instead.
 *
 * Reservation model
 * -----------------
 * The maximum working set (policy.max blocks) is reserved up front as a single
 * anonymous MAP_NORESERVE region. Physical pages are faulted lazily and only
 * for the committed prefix [0, committed). Because the region is reserved once
 * and never remapped, *block addresses are stable for the whole lifetime of
 * the slab* - a hard requirement for TCP reordering buffers that keep pointers
 * to out-of-order segments while the slab grows and shrinks underneath them.
 *
 * Reserving once is also what makes growth free: committing blocks is a pointer
 * bump, no syscall. mremap() is deliberately not used - it cannot preserve the
 * base address (MREMAP_FIXED onto the old base is EINVAL, and MREMAP_MAYMOVE
 * relocates the region, invalidating every outstanding block pointer), and
 * growing in place needs the address space above the region to be free, which
 * is only guaranteed by reserving it, which is what happens here. The
 * reservation itself is cheap: page tables are built lazily, so reserving a
 * gigabyte of address space costs about a microsecond.
 *
 * Grow / shrink policy
 * --------------------
 * The committed working set grows and shrinks under a preconfigured
 * struct slab_policy (min/max/step blocks, low/high free-ratio watermarks, an
 * idle dwell before releasing memory) and an optional check() gate that can
 * veto either direction. slab_gc() applies the policy; slab_grow()/slab_shrink()
 * are the unconditional primitives. Shrink returns resident memory to the OS
 * with madvise(MADV_DONTNEED) over the fully-covered pages of the reclaimed
 * tail.
 *
 * Reservation backend
 * -------------------
 * Where the reservation comes from is a build-time choice: the default is
 * mmap()/munmap()/madvise(), -DSLAB_MALLOC_FREE routes it through the libc heap,
 * and the SLAB_VM_* hooks plug in any custom allocator. See <mem/slab_vm.h>,
 * which also carries the CONFIG_MEM_HUGEPAGE / CONFIG_MEM_POPULATE hints.
 */

#ifndef __HPC_MEM_SLAB_H__
#define __HPC_MEM_SLAB_H__

#include <hpc/compiler.h>
#include <hpc/cpu.h>
#include <hpc/log.h>
#include <hpc/bitset.h>
#include <mem/measure.h>
/* SLAB_VM_* / SLAB_MEM_* reservation backend, VM_PAGE_*, SLAB_NIL. */
#include <mem/slab_vm.h>

#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

__BEGIN_DECLS

/* Common path MTUs, handy for picking a block size (see slab_shift_for). */
#define SLAB_MTU_ETHERNET 1500   /* -> 2048 byte block  */
#define SLAB_MTU_JUMBO    9000   /* -> 16384 byte block */

struct slab;

/*
 * Grow/shrink policy. Counts are in blocks; percentages are of committed
 * blocks (for the watermarks) or of the free blocks (for the release).
 *
 * The shrink rule reads directly as: "when live usage stays at/under
 * @shrink_usage_pct for @shrink_after, return @shrink_release_pct of the
 * currently unused memory to the OS." For example
 * { .shrink_usage_pct = 30, .shrink_after = 30000, .shrink_release_pct = 50 }
 * means: usage under 30% for 30 s -> release half of the free blocks.
 *
 * @min        lower bound of the committed working set; shrink never goes below
 * @max        upper bound; grow never goes above (also the reservation size)
 * @grow_step  blocks committed per grow step (0 -> 1)
 * @grow_usage_pct
 *             grow when live usage is >= this percent of committed (0 disables
 *             watermark growth; the slab still auto-grows on exhaustion)
 * @shrink_usage_pct
 *             shrink when live usage is <= this percent of committed
 * @shrink_release_pct
 *             on each shrink, return this percent of the currently free
 *             (unused) blocks to the OS (0 disables shrinking entirely)
 * @shrink_after
 *             idle dwell: usage must stay at/under @shrink_usage_pct for this
 *             long before memory is released, in the same time unit
 *             (milliseconds) as the @now passed to slab_gc(). This is
 *             hysteresis - it stops a transient dip from thrashing the slab.
 *             0 releases immediately once the watermark is met. The timer
 *             re-arms after each release, so at most @shrink_release_pct of the
 *             then-free blocks is returned per @shrink_after interval.
 * @check      optional gate; grow==1 for grow, 0 for shrink; return true to
 *             allow the operation, false to veto it
 * @arg        opaque argument passed to check()
 */
struct slab_policy {
	u32 min;
	u32 max;
	u32 grow_step;
	u16 grow_usage_pct;
	u16 shrink_usage_pct;
	u16 shrink_release_pct;
	timestamp_t shrink_after;
	bool (*check)(struct slab *slab, int grow, void *arg);
	void *arg;
};

/*
 * Event measurement. The slab keeps a pointer to a caller-owned
 * struct slab_measure (declared by measure_member(slab), defined in
 * <mem/measure.h>); events are counted directly through the generic
 * measure_inc()/measure_add() from <hpc/measure.h>, which no-op on a NULL
 * measure and vanish entirely without CONFIG_MEASURE. Attach a struct after
 * slab_init() - many slabs may point at one shared struct to aggregate, or
 * hold their own - and leave it NULL to not measure. The initial commit done
 * inside slab_init() is never counted because no measure is attached yet.
 */

/* Dynamic slab allocator handle. */
struct slab {
	u64 length;           /* reserved region length in bytes              */
	u32 list;             /* head index of the free list (SLAB_NIL end)   */
	u32 avail;            /* number of free blocks on the list            */
	u32 committed;        /* blocks currently committed [0, committed)     */
	u32 total;            /* reserved (maximum) blocks                    */
	u32 shift;            /* block size aligned to power of 2 (log2)       */
	u32 grain;            /* blocks per release unit; see <mem/slab_vm.h>  */
	timestamp_t idle_since; /* when usage first dropped to the low mark    */
	u8 idle;              /* idle-shrink timer armed                       */
	struct slab_policy policy;
	measure_member(slab)  /* caller-owned event counters (CONFIG_MEASURE)   */
	u8 *map;              /* occupancy bitmap, one bit per reserved block */
	void *page;           /* base of the reservation                      */
};

/* Free-list node overlay: valid only while the block sits on the free list. */
struct slab_node {
	u32 avail;            /* next free block index, or SLAB_NIL           */
};

/*
 * slab_shift_for - pick the power-of-2 block shift able to hold @block_size.
 *
 * A 1500 byte Ethernet MTU yields a 2048 byte block (shift 11); a 9000 byte
 * jumbo frame yields 16384 (shift 14). The block never shrinks below the free
 * list node.
 */
static inline unsigned
slab_shift_for(unsigned block_size)
{
	u32 v = (u32)block_size;
	if (v < sizeof(struct slab_node))
		v = (u32)sizeof(struct slab_node);
	/* round v up to the next power of two (identity when already pow2) */
	v--;
	v |= v >> 1;
	v |= v >> 2;
	v |= v >> 4;
	v |= v >> 8;
	v |= v >> 16;
	v++;
	return (unsigned)__builtin_ctz(v);
}

/* ---- internal helpers, all taking an explicit shift ---------------------- */

static inline void *
__slab_at(struct slab *slab, unsigned shift, u32 index)
{
	if (index == SLAB_NIL)
		return NULL;
	return (u8 *)slab->page + ((size_t)index << shift);
}

static inline u32
__slab_index(struct slab *slab, unsigned shift, void *p)
{
	return (u32)(((u8 *)p - (u8 *)slab->page) >> shift);
}

/*
 * Round the policy to whole grains: max (and so the reservation) and min up, and
 * grow_step up to at least one grain. A min that rounds past max is clamped to
 * it. Doing this once here is what lets every later grow and shrink stay
 * grain-aligned.
 */
static inline void
__slab_round_policy(struct slab *slab)
{
	u32 grain = slab->grain ? slab->grain : 1;
	slab->policy.max = slab_grain_round(slab->policy.max, grain);
	slab->policy.min = slab_grain_round(slab->policy.min, grain);
	if (slab->policy.min > slab->policy.max)
		slab->policy.min = slab->policy.max;
	slab->policy.grow_step = slab_grain_round(slab->policy.grow_step, grain);
	if (slab->policy.grow_step < grain)
		slab->policy.grow_step = grain;
}

static inline void
__slab_madvise_tail(struct slab *slab, unsigned shift, u32 from, u32 to)
{
	uintptr_t pg = CPU_PAGE_SIZE;
	uintptr_t base = (uintptr_t)slab->page;
	uintptr_t start = base + ((size_t)from << shift);
	uintptr_t end = base + ((size_t)to << shift);
	uintptr_t a = align_to(start, pg);      /* first whole page in range   */
	uintptr_t b = end & ~(pg - 1);          /* last whole page in range    */
	if (b > a)
		SLAB_VM_RELEASE((void *)a, (size_t)(b - a));
}

/*
 * Pre-fault the blocks a grow is about to commit.
 *
 * Only worth it when a block is no larger than a page: committing writes one
 * free-list node per block, so with a sub-page block that write already faults
 * every page in the range and one madvise() replaces a fault per page (measured
 * ~1.7x cheaper for a 2048 byte block). With a block spanning several pages the
 * node write touches only the first page of each, and populating the rest would
 * fault pages the caller may never use - measurably slower and it commits memory
 * that would otherwise stay untouched. Advisory either way: the range is
 * faulted lazily if this does nothing.
 */
static inline void
__slab_populate(struct slab *slab, unsigned shift, u32 from, u32 to)
{
	if ((1u << shift) > CPU_PAGE_SIZE || to <= from)
		return;
	SLAB_VM_POPULATE((u8 *)slab->page + ((size_t)from << shift),
			 (size_t)(to - from) << shift);
}

static inline u32
__slab_grow(struct slab *slab, unsigned shift, u32 n)
{
	u32 grew = 0;
	u32 room, plan;

	/* Commit whole grains: a partial one would report blocks the slab does
	 * not really have memory boundaries for. min/max/grow_step are already
	 * rounded, so this only matters for a caller-supplied @n. */
	n = slab_grain_round(n, slab->grain);

	/*
	 * What the loop below will actually commit, computed up front so the
	 * range can be pre-faulted in one call. The loop bound is unchanged;
	 * this only mirrors it.
	 */
	room = slab->policy.max < slab->total ? slab->policy.max : slab->total;
	plan = slab->committed < room ? room - slab->committed : 0;
	if (plan > n)
		plan = n;
	if (plan)
		__slab_populate(slab, shift, slab->committed,
		                slab->committed + plan);

	while (grew < n && slab->committed < slab->policy.max &&
	       slab->committed < slab->total) {
		u32 idx = slab->committed++;
		struct slab_node *node = (struct slab_node *)
			__slab_at(slab, shift, idx);
		node->avail = slab->list;
		slab->list = idx;
		slab->avail++;
		grew++;
	}
	if (grew) {
		measure_inc(slab->measure, grow);
		measure_add(slab->measure, commit, grew);
		measure_add(slab->measure, committed, grew);   /* gauge up */
		trace1("slab_grow (+%u -> committed %u/%u)",
			grew, slab->committed, slab->total);
	}
	return grew;
}

static inline u32
__slab_grow_policy(struct slab *slab, unsigned shift)
{
	u32 step;
	if (slab->committed >= slab->policy.max)
		return 0;
	if (slab->policy.check && !slab->policy.check(slab, 1, slab->policy.arg))
		return 0;
	step = slab->policy.grow_step ? slab->policy.grow_step : slab->grain;
	return __slab_grow(slab, shift, step);
}

/* Is every block of the grain ending at @end free? */
static inline bool
__slab_grain_free(struct slab *slab, u32 end, u32 grain)
{
	u32 i;
	if (end < grain)
		return false;
	for (i = end - grain; i < end; i++)
		if (BITSET_TEST(slab->map, i))
			return false;
	return true;
}

/*
 * __slab_retire - take grains out of service without releasing their pages.
 *
 * The accounting half of a shrink: peel whole free grains off the committed
 * tail, rebuild the free list without them, and report the vacated range in
 * [*from, *to). The pages stay mapped with their contents intact, so a pointer
 * into that range is still safe to dereference - which is the property the RCU
 * variant in <mem/slab_rcu.h> needs, because it has to let a grace period pass
 * before the release can happen. __slab_shrink() below is this plus the release.
 */
static inline u32
__slab_retire(struct slab *slab, unsigned shift, u32 n, u32 *from, u32 *to)
{
	u32 grain = slab->grain ? slab->grain : 1;
	u32 floor = slab->policy.min;
	u32 used = slab->committed - slab->avail;
	u32 old = slab->committed;
	u32 reclaimed = 0, head = SLAB_NIL, cnt = 0, i;

	if (floor < used)             /* live blocks are never reclaimable */
		floor = used;

	/*
	 * Peel whole grains off the committed tail, and only grains in which
	 * every block is free. Anything finer would drop the block count
	 * without dropping a page, which is the divergence between accounting
	 * and memory this is here to avoid.
	 */
	while (reclaimed + grain <= n && slab->committed >= floor + grain &&
	       __slab_grain_free(slab, slab->committed, grain)) {
		slab->committed -= grain;
		reclaimed += grain;
	}
	if (!reclaimed)
		return 0;

	/* Rebuild the free list, dropping the reclaimed tail indices. */
	for (i = slab->list; i != SLAB_NIL; ) {
		struct slab_node *node = (struct slab_node *)
			__slab_at(slab, shift, i);
		u32 next = node->avail;
		if (i < slab->committed) {
			node->avail = head;
			head = i;
			cnt++;
		}
		i = next;
	}
	slab->list = head;
	slab->avail = cnt;

	*from = slab->committed;
	*to = old;
	measure_inc(slab->measure, shrink);
	measure_add(slab->measure, reclaim, reclaimed);
	measure_sub(slab->measure, committed, reclaimed); /* gauge down */
	trace1("slab_retire (-%u -> committed %u/%u)",
		reclaimed, slab->committed, slab->total);
	return reclaimed;
}

static inline u32
__slab_shrink(struct slab *slab, unsigned shift, u32 n)
{
	u32 from = 0, to = 0;
	u32 reclaimed = __slab_retire(slab, shift, n, &from, &to);
	if (reclaimed)
		__slab_madvise_tail(slab, shift, from, to);
	return reclaimed;
}

static inline bool
__slab_should_grow(struct slab *slab)
{
	u32 c = slab->committed ? slab->committed : 1;
	u32 used = slab->committed - slab->avail;
	if (slab->committed >= slab->policy.max)
		return false;
	if (!slab->policy.grow_usage_pct)         /* watermark growth off */
		return false;
	return (u32)used * 100u >= (u32)slab->policy.grow_usage_pct * c;
}

static inline bool
__slab_should_shrink(struct slab *slab)
{
	u32 c = slab->committed ? slab->committed : 1;
	u32 used = slab->committed - slab->avail;
	if (slab->committed <= slab->policy.min)
		return false;
	if (!slab->policy.shrink_release_pct)     /* shrinking disabled */
		return false;
	return (u32)used * 100u <= (u32)slab->policy.shrink_usage_pct * c;
}

static inline void *
__slab_alloc(struct slab *slab, unsigned shift)
{
	struct slab_node *node = (struct slab_node *)
		__slab_at(slab, shift, slab->list);
	u32 idx;
	if (!node) {
		/* exhausted - try to grow within policy and the check() gate */
		if (!__slab_grow_policy(slab, shift)) {
			measure_inc(slab->measure, fail);
			return NULL;
		}
		node = (struct slab_node *)__slab_at(slab, shift, slab->list);
		if (!node) {
			measure_inc(slab->measure, fail);
			return NULL;
		}
	}
	slab->list = node->avail;
	slab->avail--;
	idx = __slab_index(slab, shift, node);
	BITSET_SET(slab->map, idx);
	measure_inc(slab->measure, alloc);
	measure_inc(slab->measure, used);              /* gauge up */
	return node;
}

static inline void
__slab_free(struct slab *slab, unsigned shift, void *p)
{
	u32 idx = __slab_index(slab, shift, p);
	struct slab_node *node = (struct slab_node *)p;
	BITSET_CLR(slab->map, idx);
	node->avail = slab->list;
	slab->list = idx;
	slab->avail++;
	measure_inc(slab->measure, free);
	measure_dec(slab->measure, used);              /* gauge down */
}

static inline int
__slab_gc(struct slab *slab, unsigned shift, timestamp_t now)
{
	u32 want;

	if (__slab_should_grow(slab)) {
		slab->idle = 0;                   /* not idle - cancel the timer */
		return (int)__slab_grow_policy(slab, shift);
	}
	if (!__slab_should_shrink(slab)) {
		slab->idle = 0;                   /* usage recovered above mark  */
		return 0;
	}

	/* Usage is at/under the low watermark: run the idle dwell timer. */
	if (!slab->idle) {
		slab->idle = 1;
		slab->idle_since = now;
	} else if (now < slab->idle_since) {      /* clock went backwards   */
		slab->idle_since = now;
	}
	if (now - slab->idle_since < slab->policy.shrink_after)
		return 0;                         /* not idle long enough yet   */

	/*
	 * Release shrink_release_pct of the currently unused (free) blocks,
	 * rounded up to a whole grain - the percentage is a target, the grain is
	 * the unit memory actually comes back in - and never more than is free.
	 */
	want = (u32)((u64)slab->avail * slab->policy.shrink_release_pct / 100u);
	want = slab_grain_round(want, slab->grain);
	if (want > slab->avail)
		want = slab->avail;
	if (want < slab->grain)
		return 0;                         /* not a whole grain to release */

	if (slab->policy.check && !slab->policy.check(slab, 0, slab->policy.arg))
		return 0;                         /* gate vetoes; stay armed    */

	slab->idle_since = now;                   /* rate-limit the next round */
	return -(int)__slab_shrink(slab, shift, want);
}

static inline int
__slab_init(struct slab *slab, unsigned shift, const struct slab_policy *policy)
{
	memset(slab, 0, sizeof(*slab));
	slab->shift = shift;
	slab->grain = slab_grain_for(shift);
	slab->policy = *policy;
	__slab_round_policy(slab);
	slab->total = slab->policy.max;
	slab->list = SLAB_NIL;
	slab->length = (u64)slab->total << shift;

	slab->page = SLAB_VM_ALLOC(slab->length);
	if (slab->page == SLAB_VM_FAILED) {
		slab->page = NULL;
		trace1("slab_init: reserving %lu bytes failed",
			(unsigned long)slab->length);
		return -1;
	}
	SLAB_VM_HUGEPAGE(slab->page, slab->length);
	slab->map = (u8 *)SLAB_MEM_CALLOC(BITSET_SIZE(slab->total), 1);
	if (!slab->map) {
		SLAB_VM_FREE(slab->page, slab->length);
		slab->page = NULL;
		return -1;
	}
	/*
	 * Commit the initial working set. No measure can be attached yet
	 * (memset above cleared the pointer), so this is naturally not counted
	 * as a grow event.
	 */
	if (policy->min)
		__slab_grow(slab, shift, policy->min);

	trace1("slab_init (shift: %u, min: %u, max: %u, length: %lu): %p",
		shift, policy->min, policy->max,
		(unsigned long)slab->length, slab->page);
	return 0;
}

static inline void
__slab_fini(struct slab *slab)
{
	trace1("slab_fini (shift: %u, total: %u, length: %lu): %p",
		slab->shift, slab->total, (unsigned long)slab->length,
		slab->page);
	if (slab->page)
		SLAB_VM_FREE(slab->page, slab->length);
	SLAB_MEM_FREE(slab->map);
	slab->page = NULL;
	slab->map = NULL;
}

/* ---- public API (block size read from the slab) -------------------------- */

/*
 * slab_init - reserve a slab whose block holds @block_size bytes.
 *
 * @block_size is rounded up to the next power of two (e.g. an MTU). The
 * reservation covers policy->max blocks; policy->min blocks are committed
 * immediately. Returns 0 on success, -1 on failure.
 */
static inline int
slab_init(struct slab *slab, unsigned block_size,
          const struct slab_policy *policy)
{
	return __slab_init(slab, slab_shift_for(block_size), policy);
}

static inline void
slab_fini(struct slab *slab)
{
	__slab_fini(slab);
}

static inline void *
slab_alloc(struct slab *slab)
{
	return __slab_alloc(slab, slab->shift);
}

static inline void
slab_free(struct slab *slab, void *p)
{
	__slab_free(slab, slab->shift, p);
}

/* Unconditional grow/shrink primitives; return blocks actually changed. */
static inline u32
slab_grow(struct slab *slab, u32 n)
{
	return __slab_grow(slab, slab->shift, n);
}

static inline u32
slab_shrink(struct slab *slab, u32 n)
{
	return __slab_shrink(slab, slab->shift, n);
}

/*
 * slab_gc - apply the grow/shrink policy once.
 *
 * @now is the current monotonic time in the same unit as policy.shrink_after
 * (milliseconds); it drives the idle-shrink dwell. Callers that do not use a
 * time-based shrink (policy.shrink_after == 0) may pass any value.
 *
 * Returns the signed change in committed blocks: positive if grown, negative
 * if shrunk, zero if the watermarks, the dwell timer or the check() gate left
 * it unchanged.
 */
static inline int
slab_gc(struct slab *slab, timestamp_t now)
{
	int r = __slab_gc(slab, slab->shift, now);
	if (r)
		measure_inc(slab->measure, gc);
	return r;
}

/*
 * slab_set_policy - replace the grow/shrink policy while the slab is live.
 *
 * Safe to call at any point between other operations on the same slab. The slab
 * is single-writer with no internal locking, so the caller must not run this
 * concurrently with another slab_* call on the same slab; sequenced after any
 * API call it is fine. The policy is plain value data held apart from the block
 * region, the free list and the occupancy bitmap, so swapping it never touches
 * allocated blocks - held pointers and their contents are unaffected. The new
 * thresholds take effect on the next slab_alloc()/slab_gc().
 *
 * The reservation is fixed at init, so max/min are capped at the reserved block
 * count, and the idle-shrink dwell is restarted under the new shrink_after.
 */
static inline void
slab_set_policy(struct slab *slab, const struct slab_policy *policy)
{
	slab->policy = *policy;
	__slab_round_policy(slab);
	if (slab->policy.max > slab->total)
		slab->policy.max = slab->total;
	if (slab->policy.min > slab->total)
		slab->policy.min = slab->total;
	slab->idle = 0;                           /* re-arm under new dwell */
}

/* ---- introspection ------------------------------------------------------- */

static inline unsigned
slab_block_size(struct slab *slab)
{
	return 1u << slab->shift;
}

static inline u32
slab_committed(struct slab *slab)
{
	return slab->committed;
}

static inline u32
slab_avail(struct slab *slab)
{
	return slab->avail;
}

static inline u32
slab_used(struct slab *slab)
{
	return slab->committed - slab->avail;
}

/* Release granularity: blocks per grain, and the grain in bytes. */
static inline u32
slab_grain(struct slab *slab)
{
	return slab->grain;
}

static inline size_t
slab_grain_bytes(struct slab *slab)
{
	return (size_t)slab->grain << slab->shift;
}

/*
 * The policy as the slab actually enforces it, after the grain rounding done at
 * init. Read these rather than the struct you passed in.
 */
static inline u32
slab_policy_min(struct slab *slab)
{
	return slab->policy.min;
}

static inline u32
slab_policy_max(struct slab *slab)
{
	return slab->policy.max;
}

/*
 * Bytes the committed prefix covers. Because committed is grain-aligned this is
 * the memory the slab is really holding, not a block count scaled by a size.
 */
static inline u64
slab_committed_bytes(struct slab *slab)
{
	return (u64)slab->committed << slab->shift;
}

static inline void *
slab_at(struct slab *slab, u32 index)
{
	return __slab_at(slab, slab->shift, index);
}

static inline u32
slab_index(struct slab *slab, void *p)
{
	return __slab_index(slab, slab->shift, p);
}

__END_DECLS

#endif/*__HPC_MEM_SLAB_H__*/
