/*
 * Slab allocator with deferred reclaim                       RCU-safe shrink
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
 * A slab whose shrink is safe against concurrent RCU readers.
 *
 * What needs protecting, and what does not
 * ---------------------------------------
 * Growing a slab is already safe: it extends the committed prefix over blocks
 * that were never handed out, moves nothing, and no reader can hold a pointer
 * into the range being committed. It needs no synchronization and this header
 * gives it none.
 *
 * Shrinking is the dangerous half, and worse than an ordinary free. slab_shrink()
 * ends in madvise(MADV_DONTNEED) over the reclaimed tail, and for an anonymous
 * mapping that does not merely make the old contents stale - the next read of
 * those pages returns *zeroes*. So a reader parked in a read-side section, still
 * holding a pointer to a block a writer freed, does not read a stale object: it
 * reads nothing at all, silently.
 *
 * Plan, then execute
 * ------------------
 * So the release is split from the decision to release:
 *
 *   slab_rcu_plan_shrink()  takes whole free grains out of service - drops them
 *                           from the committed prefix and the free list, so no
 *                           new allocation can land there - and asks for a grace
 *                           period. The pages stay mapped and intact, so a
 *                           parked reader still dereferences valid memory.
 *   slab_rcu_tick()         called from a timer or event loop (once a second is
 *                           the default), releases the retired range once the
 *                           grace period has elapsed, and applies any new plan.
 *
 * Nothing about this asks a reader to do anything beyond what RCU already asks:
 * take rcu_read_lock()/rcu_read_unlock() around the section in which it
 * dereferences slab blocks, register the thread once, and - under
 * CONFIG_RCU_QSBR - announce a quiescent state periodically. The grace period
 * *is* the rendezvous; there is no separate barrier for readers to enter, and
 * adding one would only be a slower way to wait for the same thing.
 *
 * What this does and does not guarantee
 * -------------------------------------
 * The guarantee is the kernel's SLAB_TYPESAFE_BY_RCU, not more: while a reader
 * may hold a stale block pointer, the memory behind it stays mapped and
 * type-valid. It does *not* stop slab_rcu_free() from handing that same block
 * out again to the next allocation. A reader that must not observe a recycled
 * block has to say so itself, exactly as in the kernel - re-validate the object
 * under the read-side section (a sequence number, a refcount), or defer the free
 * of the object with its own call_rcu(). This header protects the memory, not the
 * identity of what is in it.
 *
 * Which liburcu flavour supplies the grace period, and what each one asks of a
 * read-side thread, is <hpc/rcu.h>'s business and set by CONFIG_RCU_*.
 *
 * Relation to the polled grace-period API
 * ---------------------------------------
 * A tick that asks "has the grace period elapsed yet?" without blocking is
 * exactly what the Linux kernel's polled API does:
 *
 *   unsigned long c = get_state_synchronize_rcu();  / * or start_poll_...  * /
 *   ...
 *   if (poll_state_synchronize_rcu(c))              / * checked on a tick  * /
 *           release();                              / * safe now          * /
 *
 * (include/linux/rcutree.h). The vendored liburcu is 0.11 and predates that
 * interface, so the same state machine is built from the primitives it does
 * have: call_rcu() with a callback that sets a flag the tick reads. Should the
 * vendored liburcu be updated to 0.14 or newer, __slab_rcu_gp_start() and
 * __slab_rcu_gp_done() are the only two places that need to change.
 *
 * Single writer
 * -------------
 * Like the plain slab, this is single-writer with no internal locking: alloc,
 * free, plan and tick are all writer-side and must not run concurrently with
 * each other. Readers run concurrently with all of them. If several threads
 * allocate, serialise them the way you already serialise writers.
 */

#ifndef __HPC_MEM_SLAB_RCU_H__
#define __HPC_MEM_SLAB_RCU_H__

#include <hpc/compiler.h>
#include <hpc/rcu.h>
#include <mem/slab.h>

/*
 * No non-RCU spelling on purpose, following <hpc/rcu.h>: a deferred reclaim
 * without a grace period is not a slower version of this, it is an unsafe one.
 * Without CONFIG_RCU this header contributes nothing and a caller fails at the
 * line that needs it.
 */
#ifdef CONFIG_RCU

#include <urcu/uatomic.h>

__BEGIN_DECLS

/* Default tick period, milliseconds - the "check once a second" the tick is for. */
#ifndef SLAB_RCU_TICK_MS
#define SLAB_RCU_TICK_MS 1000
#endif

/* Retire state. */
enum slab_rcu_state {
	SLAB_RCU_IDLE = 0,    /* nothing retired, nothing owed              */
	SLAB_RCU_RETIRED,     /* tail out of service, grace period pending  */
};

struct slab_rcu_stat {
	u64 ticks;            /* ticks that were due and ran                */
	u64 plans;            /* plans recorded                             */
	u64 grows;            /* grow plans executed                        */
	u64 retires;          /* shrink plans that took grains out of service */
	u64 releases;         /* retired ranges actually handed back         */
	u64 deferred;         /* ticks that found the grace period unfinished */
	u64 cancels;          /* retires abandoned because the slab grew      */
	u64 blocks_grown;
	u64 blocks_released;
};

struct slab_rcu {
	struct slab slab;         /* the plain slab; writer-side as always    */
	struct rcu_head head;     /* one grace period may be in flight        */
	unsigned long drained;    /* set by the callback, read by the tick    */
	u32 state;                /* enum slab_rcu_state                      */
	u32 retire_from;          /* retired range [from, to), still mapped   */
	u32 retire_to;
	u32 plan_grow;            /* blocks to commit at the next tick        */
	u32 plan_shrink;          /* blocks to retire at the next tick        */
	timestamp_t interval;     /* tick period; 0 runs every call           */
	timestamp_t last_tick;
	u8 ticked;                /* last_tick is valid                       */
	struct slab_rcu_stat stat;
};

/* ---- grace period: the two functions a newer liburcu would replace ------- */

static inline void
__slab_rcu_gp_cb(struct rcu_head *head)
{
	struct slab_rcu *r = caa_container_of(head, struct slab_rcu, head);
	uatomic_set(&r->drained, 1);
}

/* Ask for a grace period. Returns with drained clear; the callback sets it. */
static inline void
__slab_rcu_gp_start(struct slab_rcu *r)
{
	uatomic_set(&r->drained, 0);
	call_rcu(&r->head, __slab_rcu_gp_cb);
}

/* Has the grace period asked for by __slab_rcu_gp_start() elapsed? */
static inline bool
__slab_rcu_gp_done(struct slab_rcu *r)
{
	return uatomic_read(&r->drained) != 0;
}

/* ---- internals ----------------------------------------------------------- */

/* Hand the retired range back. Only ever called once the grace period is in. */
static inline u32
__slab_rcu_release(struct slab_rcu *r)
{
	u32 blocks = r->retire_to - r->retire_from;
	__slab_madvise_tail(&r->slab, r->slab.shift,
	                    r->retire_from, r->retire_to);
	r->state = SLAB_RCU_IDLE;
	r->retire_from = r->retire_to = 0;
	r->stat.releases++;
	r->stat.blocks_released += blocks;
	trace1("slab_rcu_release (%u blocks -> committed %u)",
		blocks, slab_committed(&r->slab));
	return blocks;
}

/*
 * Abandon a pending retire.
 *
 * Nothing was released yet, so the retired blocks are still mapped and still
 * hold whatever they held: putting them back into service costs nothing and
 * faults nothing. Any growth does this, because growth re-commits from exactly
 * where the retire left off - and releasing pages a grow has just handed out
 * would be the one way this could corrupt a live block.
 *
 * The outstanding grace period is left to complete on its own; its callback only
 * sets a flag, and the next retire clears that flag before asking again.
 */
static inline void
slab_rcu_cancel(struct slab_rcu *r)
{
	if (r->state != SLAB_RCU_RETIRED)
		return;
	trace1("slab_rcu_cancel (%u blocks back in service)",
		r->retire_to - r->retire_from);
	r->state = SLAB_RCU_IDLE;
	r->retire_from = r->retire_to = 0;
	r->stat.cancels++;
}

/* ---- lifetime ------------------------------------------------------------ */

/*
 * slab_rcu_init - as slab_init(), plus the tick period in milliseconds.
 *
 * Pass 0 for @tick_ms to take SLAB_RCU_TICK_MS; the tick then rate-limits itself
 * to that period, so slab_rcu_tick() can be called as often as an event loop
 * likes. A negative-free build that wants no rate limiting sets it explicitly
 * with slab_rcu_set_interval(&r, 0).
 */
static inline int
slab_rcu_init(struct slab_rcu *r, unsigned block_size,
              const struct slab_policy *policy, timestamp_t tick_ms)
{
	memset(r, 0, sizeof(*r));
	r->interval = tick_ms ? tick_ms : (timestamp_t)SLAB_RCU_TICK_MS;
	r->state = SLAB_RCU_IDLE;
	return slab_init(&r->slab, block_size, policy);
}

/*
 * slab_rcu_fini - release the slab.
 *
 * rcu_barrier() first: a grace period may still be in flight with a callback
 * that would write into this struct. The kernel does the same thing in
 * kmem_cache_destroy() for a SLAB_TYPESAFE_BY_RCU cache. This blocks, and is
 * meant to - it is teardown.
 *
 * Callers still holding block pointers must be gone by now; this frees the whole
 * reservation and no grace period will save a reader from that.
 */
static inline void
slab_rcu_fini(struct slab_rcu *r)
{
	rcu_barrier();
	r->state = SLAB_RCU_IDLE;
	slab_fini(&r->slab);
}

/* ---- writer side --------------------------------------------------------- */

/*
 * slab_rcu_alloc - allocate a block.
 *
 * An exhausted slab grows inside slab_alloc(), which would re-commit into a
 * pending retire, so the retire is abandoned first. That is why this is not just
 * slab_alloc(): growth and a pending release cannot both be allowed to touch the
 * same range.
 */
static inline void *
slab_rcu_alloc(struct slab_rcu *r)
{
	if (!slab_avail(&r->slab))
		slab_rcu_cancel(r);
	return slab_alloc(&r->slab);
}

/*
 * slab_rcu_free - return a block to the free list.
 *
 * The block may be handed out again immediately; see the note on
 * SLAB_TYPESAFE_BY_RCU semantics at the top of this header. What is guaranteed
 * is that the memory stays mapped, so a reader holding this pointer keeps
 * dereferencing something valid.
 */
static inline void
slab_rcu_free(struct slab_rcu *r, void *p)
{
	slab_free(&r->slab, p);
}

/* ---- planning ------------------------------------------------------------ */

/*
 * slab_rcu_plan_grow / slab_rcu_plan_shrink - record intent, change nothing.
 *
 * The two are mutually exclusive: recording one clears the other, because a
 * tick that was told to do both has been told nothing. Returns the blocks
 * recorded, rounded up to a whole grain.
 */
static inline u32
slab_rcu_plan_grow(struct slab_rcu *r, u32 blocks)
{
	r->plan_shrink = 0;
	r->plan_grow = slab_grain_round(blocks, slab_grain(&r->slab));
	r->stat.plans++;
	return r->plan_grow;
}

static inline u32
slab_rcu_plan_shrink(struct slab_rcu *r, u32 blocks)
{
	r->plan_grow = 0;
	r->plan_shrink = slab_grain_round(blocks, slab_grain(&r->slab));
	r->stat.plans++;
	return r->plan_shrink;
}

static inline bool
slab_rcu_planned(struct slab_rcu *r)
{
	return r->plan_grow || r->plan_shrink;
}

/*
 * slab_rcu_plan_gc - let the slab's own policy decide the plan.
 *
 * Reads the watermarks and the idle dwell exactly as slab_gc() does, but records
 * the outcome as a plan instead of performing it, so the memory movement still
 * happens on a tick under a grace period. Returns the signed block count
 * planned.
 */
static inline int
slab_rcu_plan_gc(struct slab_rcu *r, timestamp_t now)
{
	struct slab *s = &r->slab;
	u32 want;

	if (__slab_should_grow(s)) {
		s->idle = 0;
		if (s->committed >= s->policy.max)
			return 0;
		if (s->policy.check && !s->policy.check(s, 1, s->policy.arg))
			return 0;
		return (int)slab_rcu_plan_grow(r, s->policy.grow_step);
	}
	if (!__slab_should_shrink(s)) {
		s->idle = 0;
		return 0;
	}
	if (!s->idle) {
		s->idle = 1;
		s->idle_since = now;
	} else if (now < s->idle_since) {
		s->idle_since = now;
	}
	if (now - s->idle_since < s->policy.shrink_after)
		return 0;

	want = (u32)((u64)s->avail * s->policy.shrink_release_pct / 100u);
	want = slab_grain_round(want, s->grain);
	if (want > s->avail)
		want = s->avail;
	if (want < s->grain)
		return 0;
	if (s->policy.check && !s->policy.check(s, 0, s->policy.arg))
		return 0;

	s->idle_since = now;                      /* rate-limit the next round */
	return -(int)slab_rcu_plan_shrink(r, want);
}

/* ---- the tick ------------------------------------------------------------ */

/*
 * slab_rcu_tick - execute what is due. Never blocks.
 *
 * Call it from a timer or the idle path of an event loop; it rate-limits itself
 * to the configured interval, so calling it every time round the loop is fine.
 * In order, one tick:
 *
 *   1. applies a planned grow, first. Growth is why someone is waiting, it is
 *      always safe, and it abandons a pending retire rather than waiting for it -
 *      so a reclaim that has not finished its grace period must not be allowed to
 *      starve it;
 *   2. otherwise finishes a retire whose grace period has elapsed, releasing the
 *      pages - and if the grace period has not elapsed, leaves it for the next
 *      tick (counted in stat.deferred) rather than waiting;
 *   3. otherwise applies a planned shrink: retires grains and starts a grace
 *      period, for a later tick to release.
 *
 * Returns the signed change in committed blocks: positive grown, negative
 * retired, zero if nothing was due. Note a release is not a change in committed
 * - the blocks left the committed prefix when they were retired - so a tick that
 * only releases pages returns 0. stat.releases and stat.blocks_released are
 * where that shows up.
 */
static inline int
slab_rcu_tick(struct slab_rcu *r, timestamp_t now)
{
	int delta = 0;

	if (r->ticked && r->interval && now >= r->last_tick &&
	    (now - r->last_tick) < r->interval)
		return 0;                         /* not due yet */
	r->last_tick = now;
	r->ticked = 1;
	r->stat.ticks++;

	/* 1. growth first, and it takes the retired range back if there is one */
	if (r->plan_grow) {
		u32 n = r->plan_grow;
		u32 grew;
		r->plan_grow = 0;
		slab_rcu_cancel(r);               /* growth reclaims the range */
		grew = slab_grow(&r->slab, n);
		if (grew) {
			r->stat.grows++;
			r->stat.blocks_grown += grew;
		}
		return (int)grew;
	}

	/* 2. finish a retire whose grace period is in */
	if (r->state == SLAB_RCU_RETIRED) {
		if (!__slab_rcu_gp_done(r)) {
			r->stat.deferred++;
			return 0;                 /* readers still parked */
		}
		__slab_rcu_release(r);
	}

	/* 3. start a new retire */
	if (r->plan_shrink) {
		u32 n = r->plan_shrink, from = 0, to = 0, retired;
		r->plan_shrink = 0;
		if (r->state != SLAB_RCU_IDLE)
			return delta;             /* one retire at a time */
		retired = __slab_retire(&r->slab, r->slab.shift, n, &from, &to);
		if (retired) {
			r->retire_from = from;
			r->retire_to = to;
			r->state = SLAB_RCU_RETIRED;
			r->stat.retires++;
			__slab_rcu_gp_start(r);
			trace1("slab_rcu_retire (%u blocks awaiting a grace period)",
				retired);
		}
		delta = -(int)retired;
	}
	return delta;
}

/*
 * slab_rcu_sync - finish everything now, blocking.
 *
 * rcu_barrier() waits for the outstanding callback, so the pending retire is
 * releasable when the tick that follows runs. For teardown paths and tests -
 * anywhere the deferral itself is what is in the way. The interval is bypassed.
 *
 * Returns the blocks released.
 */
static inline u32
slab_rcu_sync(struct slab_rcu *r)
{
	if (r->state != SLAB_RCU_RETIRED)
		return 0;
	rcu_barrier();
	if (!__slab_rcu_gp_done(r))             /* callback ran, flag is set */
		return 0;
	return __slab_rcu_release(r);
}

/* ---- reader side --------------------------------------------------------- */

/*
 * Registration and read-side sections. These are liburcu's own primitives under
 * names that say what they are for; the flavour decides what a reader owes (see
 * <hpc/rcu.h>). Every thread that dereferences slab blocks calls
 * slab_rcu_reader_register() once before its first read-side section and
 * slab_rcu_reader_unregister() before it exits, and brackets each traversal in
 * slab_rcu_read_lock()/slab_rcu_read_unlock(). Under CONFIG_RCU_QSBR nothing
 * marks the end of a reader, so such a thread also calls
 * slab_rcu_quiescent() periodically or a planned shrink never becomes
 * releasable.
 */
static inline void
slab_rcu_reader_register(void)
{
#ifndef CONFIG_RCU_BP
	rcu_register_thread();                  /* bp registers on first use */
#endif
}

static inline void
slab_rcu_reader_unregister(void)
{
#ifndef CONFIG_RCU_BP
	rcu_unregister_thread();
#endif
}

static inline void
slab_rcu_read_lock(void)
{
	rcu_read_lock();
}

static inline void
slab_rcu_read_unlock(void)
{
	rcu_read_unlock();
}

static inline void
slab_rcu_quiescent(void)
{
#ifdef CONFIG_RCU_QSBR
	rcu_quiescent_state();
#endif
}

/* ---- introspection ------------------------------------------------------- */

static inline void
slab_rcu_set_interval(struct slab_rcu *r, timestamp_t ms)
{
	r->interval = ms;
}

static inline u32
slab_rcu_state(struct slab_rcu *r)
{
	return r->state;
}

/* Blocks retired but not yet released: out of service, still resident. */
static inline u32
slab_rcu_retiring(struct slab_rcu *r)
{
	return r->state == SLAB_RCU_RETIRED ? r->retire_to - r->retire_from : 0;
}

/*
 * Blocks the slab is actually holding memory for: the committed prefix plus
 * anything retired and awaiting its grace period. This is the number to watch
 * against RSS, while slab_committed() is the working set.
 */
static inline u32
slab_rcu_resident(struct slab_rcu *r)
{
	return slab_committed(&r->slab) + slab_rcu_retiring(r);
}

static inline struct slab *
slab_rcu_slab(struct slab_rcu *r)
{
	return &r->slab;
}

static inline const struct slab_rcu_stat *
slab_rcu_stat(struct slab_rcu *r)
{
	return &r->stat;
}

__END_DECLS

#endif/*CONFIG_RCU*/

#endif/*__HPC_MEM_SLAB_RCU_H__*/
