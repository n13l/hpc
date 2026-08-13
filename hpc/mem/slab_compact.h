/*
 * Compacting slab allocator                        Defragment, then shrink
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
 * A slab whose shrink can move live blocks out of the way.
 *
 * The problem this solves
 * -----------------------
 * The plain slab in <mem/slab.h> releases memory only from the committed tail,
 * and only whole grains in which every block is free. A long-lived allocation
 * pattern - a session table where most sessions end quickly but a few linger -
 * leaves stragglers scattered across the tail, and each one pins a whole grain
 * resident. Usage drops to 5%, the shrink watermark fires, and nothing comes
 * back, because no tail grain is ever entirely free. That divergence between
 * "the slab is nearly empty" and "the slab releases nothing" is fragmentation,
 * and the only cure is moving the stragglers.
 *
 * What a tick does
 * ----------------
 * slab_compact_tick() applies the same policy as slab_gc() - the watermarks,
 * the idle dwell, the check() gate - but where slab_gc() gives up on a pinned
 * tail, this starts a *compaction round*: the release target is computed once
 * (shrink_release_pct of the free blocks, as ever), and then each tick takes
 * one budgeted slice of the work until the target is met or nothing more can
 * move. A slice
 *
 *   1. moves up to @budget live blocks from the committed tail into the lowest
 *      free blocks - in bulk: a contiguous run of live tail blocks lands in a
 *      contiguous run of free blocks with a single copy - announcing every
 *      relocation through the move callback;
 *   2. rebuilds the free list in ascending block order, so the allocations
 *      that race the round land in the low half rather than re-pinning the
 *      tail this is trying to vacate;
 *   3. releases whatever whole free grains the tail can now give, up to what
 *      the round still owes.
 *
 * The budget is what makes the round incremental: an event loop that ticks
 * with budget 32 never stalls on a big slab, it just takes more ticks to
 * finish. A round in flight is abandoned the moment new demand shows up -
 * the live count rising above where the round started, or the grow watermark
 * tripping - because moving objects toward a front that is filling up is pure
 * waste. Its own releases do not cancel it: releasing raises the usage ratio
 * by design, and a round that stopped at the watermark would finish or not
 * depending on how finely it was sliced.
 *
 * The contract change: pointers move
 * ----------------------------------
 * The plain slab promises block addresses stable for the lifetime of the slab.
 * This one deliberately does not: a block address is stable only *between*
 * ticks. The move callback is where the caller keeps up:
 *
 *   void moved(struct slab *slab, void *from, void *to, void *arg);
 *
 * It runs after the object's bytes are already at @to, once per relocated
 * block, from inside the tick. The caller repoints whatever referenced @from -
 * which is O(1) when the object carries a back-reference to its owner (an
 * index into the table that points at it), and that is the pattern to use.
 * @from is dead the moment the callback returns: the slab reuses its first
 * bytes for free-list linkage before the tick is over. Read identity from
 * @to, never from @from.
 *
 * Code that stashes a block pointer anywhere the callback cannot reach - a
 * local across a tick, another thread, an I/O ring - must either pin the
 * round (the policy check() gate vetoes the shrink direction) or hold indices
 * (slab_index()/slab_at()) instead of pointers. Indices survive compaction
 * only for blocks that did not move; the callback is the one source of truth
 * for those that did.
 *
 * Like the plain slab, single writer, no internal locking: alloc, free, run
 * and tick must not race each other. The RCU variant in <mem/slab_rcu.h>
 * exists because *releasing* is unsafe under concurrent readers; moving is
 * worse, so a compacting slab with concurrent readers is not a thing this
 * header offers.
 */

#ifndef __HPC_MEM_SLAB_COMPACT_H__
#define __HPC_MEM_SLAB_COMPACT_H__

#include <hpc/compiler.h>
#include <mem/slab.h>

__BEGIN_DECLS

/*
 * Relocation callback: the object formerly at @from now lives at @to, both
 * of slab_block_size() bytes. Runs inside slab_compact_tick()/_run(), after
 * the copy. @from stays readable until the callback returns and not a moment
 * longer.
 */
typedef void (*slab_move_fn)(struct slab *slab, void *from, void *to,
			     void *arg);

struct slab_compact_stat {
	u64 ticks;            /* ticks that ran the policy                   */
	u64 rounds;           /* compaction rounds started                   */
	u64 moves;            /* blocks relocated                            */
	u64 copies;           /* bulk copies (each moved >= 1 block)         */
	u64 releases;         /* slices that handed memory back              */
	u64 blocks_released;
	u64 cancels;          /* rounds abandoned: usage recovered           */
};

struct slab_compact {
	struct slab slab;     /* the plain slab; writer-side as always       */
	slab_move_fn move;    /* how the caller learns a block relocated     */
	void *arg;            /* opaque, passed through to the callback      */
	u32 goal;             /* blocks the running round still owes         */
	u32 round_used;       /* live blocks when the round started          */
	u8 compacting;        /* a round is in flight                        */
	struct slab_compact_stat stat;
};

/* ---- the mover ------------------------------------------------------------ */

/*
 * __slab_compact_step - one budgeted defragmentation slice.
 *
 * Two cursors walk the occupancy bitmap toward each other: @dst ascends over
 * free blocks, @src descends over live ones. Each iteration pairs the longest
 * run it can afford - contiguous free at @dst, contiguous live ending at @src,
 * the remaining budget - and moves the whole run with one copy, capped so the
 * destination stays strictly below the source (the copy must not overlap and
 * a move that does not lower the block belongs to a later pairing anyway).
 * The step is over when no free block remains below a live one: the live set
 * is then a dense prefix and the tail is as releasable as it will ever get.
 *
 * A source block freed here is above every future @dst, so a slice never
 * reads a block it already vacated. The free list is rebuilt once at the end,
 * in ascending order - the LIFO list the moves invalidated would hand the
 * next allocation whatever freed last, which is exactly the tail block a
 * compaction wants left alone.
 *
 * @budget caps the blocks moved; 0 means no cap. Returns the blocks moved.
 */
static inline u32
__slab_compact_step(struct slab_compact *c, u32 budget)
{
	struct slab *s = &c->slab;
	unsigned shift = s->shift;
	u32 dst = 0;              /* lowest candidate destination             */
	u32 src = s->committed;   /* blocks [src, committed) already handled  */
	u32 moved = 0, head, cnt, dn, sn, n, i;

	if (!budget)
		budget = (u32)~0U;
	if (!s->avail)
		return 0;

	while (moved < budget) {
		while (dst < s->committed && BITSET_TEST(s->map, dst))
			dst++;
		while (src > dst && !BITSET_TEST(s->map, src - 1))
			src--;
		if (src <= dst)   /* no live block above a free one: packed  */
			break;
		/* here dst is free, src-1 is live, and dst <= src - 2 */

		/* the free run at dst, capped by the budget ... */
		dn = 1;
		while (moved + dn < budget && dst + dn < src &&
		       !BITSET_TEST(s->map, dst + dn))
			dn++;
		/* ... paired with the live run ending at src */
		sn = 1;
		while (sn < dn && src - sn > dst &&
		       BITSET_TEST(s->map, src - sn - 1))
			sn++;
		n = sn < dn ? sn : dn;
		/* keep [dst, dst+n) strictly below [src-n, src): no overlap */
		if (n > (src - dst) / 2)
			n = (src - dst) / 2;

		memcpy(__slab_at(s, shift, dst),
		       __slab_at(s, shift, src - n),
		       (size_t)n << shift);
		c->stat.copies++;
		for (i = 0; i < n; i++) {
			BITSET_CLR(s->map, src - n + i);
			BITSET_SET(s->map, dst + i);
			if (c->move)
				c->move(s, __slab_at(s, shift, src - n + i),
					__slab_at(s, shift, dst + i), c->arg);
		}
		moved += n;
		dst += n;
		src -= n;
	}

	if (moved) {
		/* Rebuild the free list ascending; the count cannot change -
		 * every move consumed one free block and produced one. */
		head = SLAB_NIL;
		cnt = 0;
		for (i = s->committed; i > 0; i--) {
			struct slab_node *node;
			if (BITSET_TEST(s->map, i - 1))
				continue;
			node = (struct slab_node *)
				__slab_at(s, shift, i - 1);
			node->avail = head;
			head = i - 1;
			cnt++;
		}
		s->list = head;
		s->avail = cnt;
		c->stat.moves += moved;
		trace1("slab_compact (%u blocks moved -> tail at %u/%u)",
			moved, src, s->committed);
	}
	return moved;
}

/* ---- lifetime -------------------------------------------------------------- */

/*
 * slab_compact_init - as slab_init(), plus how relocations are announced.
 *
 * @move may be NULL only when nothing outside the slab holds block pointers
 * (index-only access); otherwise every compaction would silently orphan the
 * caller's references.
 */
static inline int
slab_compact_init(struct slab_compact *c, unsigned block_size,
		  const struct slab_policy *policy,
		  slab_move_fn move, void *arg)
{
	memset(c, 0, sizeof(*c));
	c->move = move;
	c->arg = arg;
	return slab_init(&c->slab, block_size, policy);
}

static inline void
slab_compact_fini(struct slab_compact *c)
{
	slab_fini(&c->slab);
}

/* ---- writer side ------------------------------------------------------------ */

static inline void *
slab_compact_alloc(struct slab_compact *c)
{
	return slab_alloc(&c->slab);
}

static inline void
slab_compact_free(struct slab_compact *c, void *p)
{
	slab_free(&c->slab, p);
}

/* ---- the tick ---------------------------------------------------------------- */

/*
 * slab_compact_packed - is there nothing left for a compaction to do?
 *
 * True when no free block sits below a live one: the live set is a dense
 * prefix, so every free grain the policy floor allows is already releasable
 * without moving anything.
 */
static inline bool
slab_compact_packed(struct slab_compact *c)
{
	struct slab *s = &c->slab;
	u32 dst = 0, src = s->committed;

	while (dst < s->committed && BITSET_TEST(s->map, dst))
		dst++;
	while (src > dst && !BITSET_TEST(s->map, src - 1))
		src--;
	return src <= dst;
}

/*
 * slab_compact_run - defragment now, unconditionally.
 *
 * The mover without the policy: moves up to @budget blocks (0 for no cap)
 * toward the front and returns how many moved. Releases nothing - pair it
 * with slab_shrink() when the point was memory, or leave the dense prefix
 * for a later shrink to harvest. slab_compact_tick() is this plus the
 * policy; a caller with its own idea of when belongs here.
 */
static inline u32
slab_compact_run(struct slab_compact *c, u32 budget)
{
	return __slab_compact_step(c, budget);
}

/*
 * slab_compact_tick - apply the policy, compacting where slab_gc() would stall.
 *
 * Call it where slab_gc() would be called: a timer, the idle path of an event
 * loop. @now is milliseconds as ever; @budget caps the blocks moved this tick
 * (0 for no cap) and is what keeps a tick's latency flat regardless of slab
 * size.
 *
 * Grow behaves exactly as in slab_gc(). Shrink differs only once the dwell
 * elapses: the release target becomes a round (stat.rounds), and each tick
 * moves a budget's worth of tail blocks down before releasing what the tail
 * can then give. The round ends when the target is met, or when a slice
 * neither moved nor released anything - the floor of policy.min and the live
 * count is as far as any amount of moving gets. The idle timer was re-armed
 * when the round started, so rounds repeat at most once per shrink_after,
 * same as plain shrinks.
 *
 * A round is cancelled (stat.cancels) when demand returns: the live count
 * rises above where the round started, or the grow watermark trips. The
 * blocks already moved stay moved - they are packed toward the front, which
 * never hurts - and the blocks already released stay released.
 *
 * Returns the signed change in committed blocks, exactly as slab_gc():
 * positive grown, negative released; a tick that only moved blocks returns 0
 * and shows up in stat.moves.
 */
static inline int
slab_compact_tick(struct slab_compact *c, timestamp_t now, u32 budget)
{
	struct slab *s = &c->slab;
	u32 moved, released = 0, want;

	c->stat.ticks++;

	if (__slab_should_grow(s)) {
		s->idle = 0;
		if (c->compacting) {
			c->compacting = 0;
			c->goal = 0;
			c->stat.cancels++;
		}
		return (int)__slab_grow_policy(s, s->shift);
	}

	/*
	 * A round in flight is measured against demand, not against the usage
	 * ratio: every release raises the ratio - that is what releasing does -
	 * and a round that stopped at the watermark would finish or not
	 * depending on the budget it was sliced under. New demand is the live
	 * count climbing above where the round started; that is what cancels.
	 */
	if (c->compacting && slab_used(s) > c->round_used) {
		c->compacting = 0;
		c->goal = 0;
		c->stat.cancels++;
	}

	if (!c->compacting) {
		if (!__slab_should_shrink(s)) {
			s->idle = 0;
			return 0;
		}
		/* the idle dwell, exactly as slab_gc() runs it */
		if (!s->idle) {
			s->idle = 1;
			s->idle_since = now;
		} else if (now < s->idle_since) {  /* clock went backwards */
			s->idle_since = now;
		}
		if (now - s->idle_since < s->policy.shrink_after)
			return 0;

		/* the round's target, once: the same release a plain shrink
		 * would aim for, it just gets to move blocks to reach it */
		want = (u32)((u64)s->avail * s->policy.shrink_release_pct / 100u);
		want = slab_grain_round(want, s->grain);
		if (want > s->avail)
			want = s->avail;
		if (want < s->grain)
			return 0;              /* not a whole grain to release */
		if (s->policy.check && !s->policy.check(s, 0, s->policy.arg))
			return 0;              /* gate vetoes; stay armed      */

		s->idle_since = now;           /* rate-limit the next round    */
		c->compacting = 1;
		c->goal = want;
		c->round_used = slab_used(s);
		c->stat.rounds++;
	}

	/* one slice: move a budget's worth, then release what the tail gives */
	moved = __slab_compact_step(c, budget);
	if (c->goal >= s->grain)
		released = __slab_shrink(s, s->shift, c->goal);
	if (released) {
		c->goal -= released;
		c->stat.releases++;
		c->stat.blocks_released += released;
	}

	/*
	 * Nothing moved means the live set is a dense prefix already, so this
	 * slice's release was the last one possible; a met target ends the
	 * round the satisfied way. Either way the dwell timer owns what
	 * happens next.
	 */
	if (!moved || c->goal < s->grain) {
		c->compacting = 0;
		c->goal = 0;
	}
	return -(int)released;
}

/* ---- introspection ----------------------------------------------------------- */

static inline bool
slab_compacting(struct slab_compact *c)
{
	return c->compacting != 0;
}

/* Blocks the running round still intends to release; 0 outside a round. */
static inline u32
slab_compact_goal(struct slab_compact *c)
{
	return c->goal;
}

static inline struct slab *
slab_compact_slab(struct slab_compact *c)
{
	return &c->slab;
}

static inline const struct slab_compact_stat *
slab_compact_stat(struct slab_compact *c)
{
	return &c->stat;
}

__END_DECLS

#endif/*__HPC_MEM_SLAB_COMPACT_H__*/
