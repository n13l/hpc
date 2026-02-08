/*
 * Shared scaffolding for the RCU container stress units - hashtable_rcu_stress.c
 * and rbtree_rcu_stress.c.
 *
 * What the two units are
 * ---------------------
 * A cache built the way a real one is: the object *is* a slab block from an
 * expiring block cache <mem/slab_cache.h>, the container link lives inside the
 * payload, and lockless readers walk the container (<hpc/hash/table.h>,
 * <hpc/rbtree.h>) while one writer inserts, expires and returns memory to the
 * OS underneath them. The functional units next door say what each piece does in
 * isolation; these two run all of it at once, for long enough to catch what only
 * a race produces.
 *
 * Why a slab may shrink while readers run
 * --------------------------------------
 * slab_gc()'s shrink ends in madvise(MADV_DONTNEED) over the free tail, and for
 * an anonymous mapping that does not leave stale contents behind - the next read
 * of those pages returns *zeroes*. So a reader holding a pointer into a released
 * range does not read a stale object, it reads nothing at all, silently. The
 * discipline that makes the shrink safe here is the one thing every RCU cache
 * has to get right, and it is the writer's, not the allocator's:
 *
 *   unlink from the container  ->  wait out a grace period  ->  free the block
 *
 * Only then is a free block provably unreachable, and only free blocks are what
 * a shrink reclaims. So the writer never frees an object as it unlinks it: it
 * parks it on a retire batch, and one synchronize_rcu() pays for the whole batch
 * (stress_drain) - at least once a round, and again whenever the writer needs
 * memory back before it can insert.
 *
 * Two things follow, and both are asserted rather than assumed:
 *
 *   - slab_cache_reap() and slab_cache_gc() must NEVER be called here. Reaping
 *     walks the arena and frees every expired block without unlinking anything -
 *     it cannot, it has never heard of the container - which would hand a live,
 *     still-linked chain back to the free list. Expiry has to be driven from the
 *     container instead, the way hashtable_cache.c drives it: the walk a lookup
 *     performs anyway is what notices a deadline.
 *   - a retired object stays intact and readable until its grace period ends,
 *     because a reader that started before the unlink may still walk onto it.
 *     That is exactly what the poison below is shaped to prove.
 *
 * The poison
 * ----------
 * Every block, whenever it is handed out, is filled with a deterministic payload
 * derived from its key and its incarnation: a stamp (key, generation, checksum
 * and a magic written last, behind a barrier) followed by a pseudo-random word
 * sequence over the rest of the block. Nothing about the payload is random, so
 * any reader can recompute it from the two words at the front and check it, and
 * a check is a real check - it fails on a torn write, on a byte that moved, and
 * above all on a page that was released underneath the reader, which reads back
 * as zeroes and so fails on the magic.
 *
 * Integrity is checked at three points, because they catch different bugs:
 *
 *   readers    a bounded prefix on every object they dereference (the hot path;
 *              this is the one that catches a release under a reader)
 *   at unlink  the whole payload, by the writer, before the object is retired
 *              (catches a writer that corrupted a live object)
 *   after the  the whole payload again, once the grace period has elapsed and
 *   grace      just before the block returns to the free list (catches memory
 *   period     released or reused while it was still reachable)
 *
 * The stamp starts at offset 4 and the poison at STRESS_POISON_OFF, both for the
 * same reason: the first word of a *free* block is the allocator's free-list
 * link (struct slab_node overlays the payload, see <mem/slab.h>), so a stamp at
 * offset 0 would be corrupted by the free list - or corrupt it.
 *
 * Measurement
 * -----------
 * Both units report through <hpc/measure.h>: a writer namespace and a reader
 * namespace, declared here with DEFINE_MEASURE so the report is a generic walk
 * over the metric table rather than a hand-written printf per number. Reader
 * threads each own one struct - no atomics, no false sharing - and the run
 * aggregates them with measure_aggregate(), which is what makes the ratios come
 * out right (a hit rate has to be recomputed from the summed numerator and
 * denominator, never averaged). With CONFIG_MEASURE the slab's own counters are
 * attached as well and cross-checked against what the test itself observed.
 *
 * The report goes to stdout as TAP comments, so it is legal output for a cmocka
 * TAP run and a bats wrapper can surface it (tools/testing/bats/test_stress.bats).
 *
 * Scale
 * -----
 * The defaults are sized to run in a few hundred milliseconds, because these are
 * units in `make check` and `make check` is also run under a sanitizer and under
 * valgrind. The run is bounded by a wall-clock budget as well as a wave count, so
 * where each grace period costs a hundred times more it does proportionally less
 * soaking rather than taking a hundred times longer - and one wave, the floor,
 * still establishes everything the assertions check. HPC_STRESS_SCALE=<n> in the
 * environment multiplies both the cap and the budget for a real soak run; nothing
 * about the assertions depends on the scale.
 */

#ifndef __HPC_TEST_STRESS_UTIL_H__
#define __HPC_TEST_STRESS_UTIL_H__

/* the helpers assert through cmocka, which wants these ahead of it */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#include <hpc/compiler.h>
#include <hpc/cpu.h>
#include <hpc/measure.h>
#include <hpc/rcu.h>
#include <mem/measure.h>
#include <mem/slab_cache.h>

/* ---- shape of the run ---------------------------------------------------- */

/*
 * A 256 byte block against a 4 KiB page means a grain of 16 blocks: memory
 * comes and goes 16 blocks at a time and min/max/grow_step are rounded to that
 * (see <mem/slab_vm.h>), which is the granularity a real sub-page slab has and
 * the one the report prints. The key space overshoots the block ceiling by a
 * quarter, so the writer regularly meets a genuinely exhausted slab and has to
 * reclaim before it can insert - often enough to exercise the pressure valve
 * hard, not so often that failing is all it does.
 */
#define STRESS_BLOCK      256u     /* payload bytes per object              */
#define STRESS_MAX        1024u    /* reservation, in blocks                */
#define STRESS_MIN        64u      /* committed floor, in blocks            */
#define STRESS_STEP       128u     /* grow step, in blocks                  */
#define STRESS_KEYS       1280u    /* key space - a quarter more than fits  */
#define STRESS_READERS    4u       /* lockless reader threads               */
#define STRESS_OPS        256u     /* writer operations per round           */
#define STRESS_WAVES      5u       /* press/churn/drain cycles              */
#define STRESS_PRESS      12u      /* rounds spent filling to the ceiling   */
#define STRESS_CHURN      16u      /* rounds spent shrinking under readers  */
#define STRESS_DRAIN      4u       /* rounds spent letting everything die   */
#define STRESS_RETIRE_MAX 256u     /* objects awaiting one grace period     */
#define STRESS_TICK_MS    20u      /* virtual clock step, press/churn round */
#define STRESS_TTL_MS     400u     /* base TTL of an object                 */
#define STRESS_IDLE_MS    120u     /* idle timeout where one is set         */
#define STRESS_STEPS      512u     /* reader traversal step cap             */
#define STRESS_HOT        128u     /* blocks below this index stay live     */
#define STRESS_COLD_MS    40u      /* deadline given to the rest            */
#define STRESS_DWELL_EVERY 4096u   /* sections between one that parks       */
#define STRESS_DWELL_NS   50000L   /* how long a parked reader holds on     */
#define STRESS_BUDGET_MS  400u     /* wall-clock soak budget, x the scale   */

/*
 * The three phases of a wave, and why there are three
 * ---------------------------------------------------
 * The two states worth stressing pull in opposite directions, so each gets its
 * own phase rather than being hoped for at the same time.
 *
 * press  Every object gets its full TTL, the arena fills to its ceiling, and the
 *        writer starts meeting a genuinely exhausted slab: allocation failures,
 *        the sweep-wait-retry valve, and the grace period that valve has to pay.
 *        The clock creeps (STRESS_TICK_MS a round) but a press phase covers less
 *        virtual time than a TTL, so almost nothing dies of old age here.
 *
 * churn  A shrink can only reclaim whole free grains off the committed *tail*
 *        (__slab_retire in <mem/slab.h>) and the policy only shrinks at all when
 *        usage has fallen to its low watermark. A slab whose live blocks are
 *        scattered over its whole range therefore never shrinks, and one that
 *        only shrinks once everything has been swept away never shrinks while a
 *        reader is holding anything - which is exactly the race that matters. So
 *        this phase gives the arena a shape: a hot core of STRESS_HOT blocks
 *        whose objects live out their TTL, and a STRESS_COLD_MS deadline for
 *        everything that lands above it (stress_cool). The tail empties, the
 *        shrink releases it, and it fills again - over and over, while the core
 *        stays linked and four readers hammer it. This is the phase in which a
 *        missing grace period is caught, and it was verified to catch one.
 *
 * drain  The clock jumps a whole TTL a round and the container is swept end to
 *        end, so everything dies, every block comes back and the slab shrinks to
 *        its floor. This is what proves the arena returns to a known state rather
 *        than leaking a little on every wave.
 */

/*
 * xorshift: every key stream in the run - the writer's and each reader's - is its
 * own sequence from its own seed, so no thread's traffic pattern depends on
 * shared state or on the timing of the others.
 */
static inline u32
stress_rand(unsigned *seed)
{
	u32 x = *seed ? *seed : 2463534242u;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*seed = x;
	return x;
}

/* Work multiplier from the environment; 1 unless HPC_STRESS_SCALE says more. */
static inline unsigned
stress_scale(void)
{
	const char *s = getenv("HPC_STRESS_SCALE");
	int n = s ? atoi(s) : 1;
	return n > 0 ? (unsigned)n : 1u;
}

static inline unsigned long
stress_ms_since(const struct timespec *start)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (unsigned long)((now.tv_sec - start->tv_sec) * 1000ul +
	                       (now.tv_nsec - start->tv_nsec) / 1000000l);
}

/* ---- the object and its poison ------------------------------------------- */

#define STRESS_MAGIC      0x57BE5551u
#define STRESS_POISON_OFF 64u      /* first poison byte; past head and link  */
#define STRESS_PROBE      8u       /* poison words a reader checks per visit */

/*
 * The head of every object. @overlay is never read: it is where the slab keeps
 * the free-list link while the block is free, so nothing that has to survive a
 * free may live there. Each unit embeds this as its first member and puts its
 * container node next, which is why STRESS_POISON_OFF is asserted against
 * sizeof(the unit's object) rather than against this.
 */
struct stress_head {
	u32 overlay;              /* slab free-list link while free           */
	u32 key;
	u32 gen;                  /* incarnation, so a reuse is distinguishable */
	u32 csum;
	u32 magic;                /* written last, behind a barrier           */
	u32 pad;                  /* keeps the container node aligned         */
};

static inline u32
stress_mix(u32 x)
{
	x *= 2654435761u;
	x ^= x >> 15;
	return x * 2246822519u;
}

/* The poison word at index @i of the object identified by (@key, @gen). */
static inline u32
stress_word(u32 key, u32 gen, unsigned i)
{
	return stress_mix(key ^ stress_mix(gen ^ stress_mix(i * 2654435761u)));
}

static inline u32
stress_csum(u32 key, u32 gen)
{
	return stress_mix(key + 0x9E3779B9u * gen) ^ STRESS_MAGIC;
}

static inline u32 *
stress_poison_at(void *blk)
{
	return (u32 *)((u8 *)blk + STRESS_POISON_OFF);
}

static inline unsigned
stress_poison_words(unsigned block_size)
{
	return (block_size - STRESS_POISON_OFF) / (unsigned)sizeof(u32);
}

/*
 * Stamp and poison a freshly allocated block. The magic goes last, after a write
 * barrier, so a reader that catches this store torn fails on the magic rather
 * than on a half-written payload it would have to interpret.
 */
static inline void
stress_write(void *blk, unsigned block_size, u32 key, u32 gen)
{
	struct stress_head *h = (struct stress_head *)blk;
	u32 *p = stress_poison_at(blk);
	unsigned n = stress_poison_words(block_size), i;

	for (i = 0; i < n; i++)
		p[i] = stress_word(key, gen, i);
	h->key = key;
	h->gen = gen;
	h->csum = stress_csum(key, gen);
	cmm_smp_wmb();
	CMM_STORE_SHARED(h->magic, STRESS_MAGIC);
}

/*
 * Is this a valid object? @words caps how much of the poison is compared: the
 * readers pass STRESS_PROBE to keep their traversals cheap, the writer passes 0
 * for "all of it". A released page reads back as zeroes and so fails on the
 * magic, before any poison is touched.
 */
static inline bool
stress_valid(const void *blk, unsigned block_size, unsigned words)
{
	const struct stress_head *h = (const struct stress_head *)blk;
	const u32 *p = stress_poison_at((void *)blk);
	unsigned n = stress_poison_words(block_size), i;
	u32 key, gen;

	if (CMM_LOAD_SHARED(h->magic) != STRESS_MAGIC)
		return false;
	cmm_smp_rmb();
	key = h->key;
	gen = h->gen;
	if (h->csum != stress_csum(key, gen))
		return false;
	if (words && words < n)
		n = words;
	for (i = 0; i < n; i++)
		if (p[i] != stress_word(key, gen, i))
			return false;
	return true;
}

/*
 * Invalidate a block on its way back to the free list, once its grace period has
 * elapsed and no reader can reach it any more. A reader that somehow still holds
 * it now fails on the magic - which is the point: without this, a use-after-free
 * would keep reading a payload that happens to still be there.
 */
static inline void
stress_kill(void *blk)
{
	struct stress_head *h = (struct stress_head *)blk;
	CMM_STORE_SHARED(h->magic, 0);
}

/*
 * stress_check_detector - prove the integrity check can fail.
 *
 * A stress run whose only evidence is "no corruption was reported" is worth
 * exactly as much as its detector, so each unit runs this first: a valid object
 * passes, and the three ways a real bug shows up are each caught. The last case
 * is the one that divides the labour - a byte that moved late in the payload is
 * invisible to the bounded prefix a reader checks, which is why the writer
 * checks the whole block at unlink and again after the grace period.
 */
static inline void
stress_check_detector(void)
{
	unsigned bs = STRESS_BLOCK;
	u32 *p;
	void *blk = calloc(1, bs);

	assert_non_null(blk);
	stress_write(blk, bs, 42, 7);
	assert_true(stress_valid(blk, bs, 0));
	assert_true(stress_valid(blk, bs, STRESS_PROBE));

	/* a released page reads back as zeroes: caught on the magic */
	memset(blk, 0, bs);
	assert_false(stress_valid(blk, bs, 0));
	assert_false(stress_valid(blk, bs, STRESS_PROBE));

	/* a block on its way back to the free list: same, by construction */
	stress_write(blk, bs, 42, 7);
	stress_kill(blk);
	assert_false(stress_valid(blk, bs, STRESS_PROBE));

	/* a stamp that no longer describes its payload: caught on the checksum */
	stress_write(blk, bs, 42, 7);
	((struct stress_head *)blk)->key = 43;
	assert_false(stress_valid(blk, bs, 0));
	assert_false(stress_valid(blk, bs, STRESS_PROBE));

	/* a byte that moved: caught by the full check, missed by the prefix */
	stress_write(blk, bs, 42, 7);
	p = stress_poison_at(blk);
	p[stress_poison_words(bs) - 1] ^= 1u;
	assert_false(stress_valid(blk, bs, 0));
	assert_true(stress_valid(blk, bs, STRESS_PROBE));

	free(blk);
}

/* ---- metrics ------------------------------------------------------------- */

/*
 * The writer's view: what it did, and what the memory did in response.
 * @linked and @committed are gauges sampled at the end of the run; @usage is the
 * ratio between them, recomputed on read so it is right whatever it is read from.
 */
#define WRITER_METRICS(_ns, C, G, R) \
	C(_ns, waves,     "Press/churn/drain cycles completed") \
	C(_ns, rounds,    "Churn rounds executed") \
	C(_ns, ops,       "Writer operations attempted") \
	C(_ns, inserts,   "Objects allocated, poisoned and published") \
	C(_ns, touches,   "Live objects found and touched") \
	C(_ns, cooled,    "Objects landed above the hot core, deadline shortened") \
	C(_ns, expiries,  "Objects found expired and unlinked") \
	C(_ns, failures,  "Allocations refused - slab at its ceiling") \
	C(_ns, retires,   "Objects freed after a grace period") \
	C(_ns, gps,       "Grace periods waited for") \
	C(_ns, grows,     "slab_gc grow steps") \
	C(_ns, shrinks,   "slab_gc shrink steps - memory returned to the OS") \
	C(_ns, commit,    "Blocks committed across all grows") \
	C(_ns, reclaim,   "Blocks reclaimed across all shrinks") \
	C(_ns, bytes,     "Payload bytes handed out (allocations x block size)") \
	C(_ns, checks,    "Full-payload integrity checks") \
	C(_ns, sweeps,    "Container sweeps looking for expired objects") \
	G(_ns, linked,    "Objects currently linked in the container") \
	G(_ns, committed, "Blocks currently committed") \
	G(_ns, peak,      "High-water committed blocks") \
	G(_ns, peak_live, "High-water blocks in use at once") \
	R(_ns, usage, linked, committed, "Linked objects as percent of committed")

DEFINE_MEASURE(writer, WRITER_METRICS);

/*
 * One per reader thread, summed at the end. Everything here is a count of
 * something a lockless traversal did, so it aggregates by addition; @hit_pct is
 * a ratio and is recomputed from the sums.
 */
#define READER_METRICS(_ns, C, G, R) \
	C(_ns, sections,  "RCU read-side sections entered") \
	C(_ns, lookups,   "Key lookups attempted") \
	C(_ns, hits,      "Lookups that found their key") \
	C(_ns, scans,     "Bounded ordered scans (rbtree only)") \
	C(_ns, dwells,    "Sections that parked holding an object, then re-checked") \
	C(_ns, visits,    "Objects dereferenced inside a read-side section") \
	C(_ns, checks,    "Integrity checks performed") \
	C(_ns, aborts,    "Traversals that hit the step cap - a cycle, not a bound") \
	C(_ns, torn,      "Objects that failed an integrity check - BUGS") \
	G(_ns, threads,   "Reader threads that reported in") \
	R(_ns, hit_pct, hits, lookups, "Lookups that hit, percent")

DEFINE_MEASURE(reader, READER_METRICS);

/* ---- the arena: an expiring block cache, retired through grace periods ---- */

struct stress_arena {
	struct slab_cache cache;
	unsigned block_size;
	void *retire[STRESS_RETIRE_MAX];   /* unlinked, awaiting a grace period */
	unsigned nretire;
	bool cool;                         /* churn phase: cold tail, see below */
	struct writer_measure m;
#ifdef CONFIG_MEASURE
	struct slab_measure slab_m;        /* the slab's own counters           */
#endif
};

static inline struct slab *
stress_slab(struct stress_arena *a)
{
	return &a->cache.slab;
}

static inline int
stress_arena_init(struct stress_arena *a, unsigned block_size,
                  const struct slab_policy *pol, u32 ttl, u32 idle)
{
	memset(a, 0, sizeof(*a));
	a->block_size = block_size;
	if (slab_cache_init(&a->cache, block_size, pol, ttl, idle))
		return -1;
#ifdef CONFIG_MEASURE
	stress_slab(a)->measure = &a->slab_m;
	/* slab_init() committed policy.min before there was anywhere to count
	 * it, so seed the gauge to match; every later move is counted. */
	a->slab_m.committed = slab_committed(stress_slab(a));
#endif
	a->m.committed = slab_committed(stress_slab(a));
	a->m.peak = a->m.committed;
	return 0;
}

/* Fold a change in the committed prefix into the grow/shrink counters. */
static inline void
stress_account(struct stress_arena *a, u32 before)
{
	u32 now = slab_committed(stress_slab(a));

	if (now > before) {
		a->m.grows++;
		a->m.commit += now - before;
	} else if (now < before) {
		a->m.shrinks++;
		a->m.reclaim += before - now;
	}
	a->m.committed = now;
	if (now > a->m.peak)
		a->m.peak = now;
}

/*
 * In the churn phase only: shorten the deadline of a block that landed above the
 * hot core, so the committed tail keeps emptying while the core stays live - the
 * arrangement the shrink needs to be able to race a reader at all (see the phase
 * notes above).
 *
 * The deadline is edited in place through the cache's own per-block metadata,
 * which is the only way to say "this block, not this key": slab_cache_alloc_ex()
 * takes a TTL before the block - and so its index - is known.
 */
static inline void
stress_cool(struct stress_arena *a, void *blk, timestamp_t now)
{
	u32 idx = slab_index(stress_slab(a), blk);

	if (!a->cool || idx < STRESS_HOT)
		return;
	a->cache.ent[idx].ttl_at = now + STRESS_COLD_MS;
	a->m.cooled++;
}

/*
 * Allocate, stamp and poison one object. Returns NULL when the slab is at its
 * ceiling and the policy will not grow - the caller's cue to reclaim and retry.
 *
 * An exhausted slab grows inside slab_alloc() rather than waiting for a gc, so
 * the commit is accounted here too; growth is safe under readers at any time,
 * because it only ever commits blocks nobody could hold a pointer to.
 */
static inline void *
stress_alloc(struct stress_arena *a, timestamp_t now, u32 key, u32 gen,
             u32 ttl, u32 idle)
{
	u32 before = slab_committed(stress_slab(a));
	void *blk = slab_cache_alloc_ex(&a->cache, now, ttl, idle);

	stress_account(a, before);
	if (!blk) {
		a->m.failures++;
		return NULL;
	}
	stress_write(blk, a->block_size, key, gen);
	stress_cool(a, blk, now);
	a->m.inserts++;
	a->m.bytes += a->block_size;
	if (slab_used(stress_slab(a)) > a->m.peak_live)
		a->m.peak_live = slab_used(stress_slab(a));
	return blk;
}

/*
 * Park an unlinked object on the retire batch. The caller must have taken it out
 * of the container already; this verifies the whole payload one last time while
 * the writer still owns it exclusively, so a corruption is attributed to the
 * churn rather than to the grace period.
 *
 * A full batch is drained here, which is the only place the caller does not
 * choose when a grace period happens.
 */
static inline void stress_drain(struct stress_arena *a);

static inline void
stress_retire(struct stress_arena *a, void *blk)
{
	assert_true(stress_valid(blk, a->block_size, 0));
	a->m.checks++;
	if (a->nretire == STRESS_RETIRE_MAX)
		stress_drain(a);
	a->retire[a->nretire++] = blk;
}

/*
 * Wait out one grace period and hand the whole batch back.
 *
 * After synchronize_rcu() every read-side section that could have been holding
 * one of these objects has ended, so each is checked once more (nothing may have
 * touched it while it was reachable-but-unlinked), invalidated, and freed. Only
 * now may the memory under it be released by a shrink.
 */
static inline void
stress_drain(struct stress_arena *a)
{
	unsigned i;

	if (!a->nretire)
		return;
	synchronize_rcu();
	a->m.gps++;
	for (i = 0; i < a->nretire; i++) {
		void *blk = a->retire[i];
		assert_true(stress_valid(blk, a->block_size, 0));
		a->m.checks++;
		stress_kill(blk);
		slab_cache_free(&a->cache, blk);
		a->m.retires++;
	}
	a->nretire = 0;
}

/*
 * Run the slab's grow/shrink policy. This is slab_gc(), NOT slab_cache_gc():
 * the latter reaps expired blocks behind the container's back (see the note at
 * the top of this header), and here expiry is the container's job.
 *
 * Returns the signed change in committed blocks.
 */
static inline int
stress_gc(struct stress_arena *a, timestamp_t now)
{
	u32 before = slab_committed(stress_slab(a));
	int delta = slab_gc(stress_slab(a), now);

	stress_account(a, before);
	return delta;
}

static inline void
stress_arena_fini(struct stress_arena *a)
{
	stress_drain(a);
	slab_cache_fini(&a->cache);
}

/* ---- the churn itself ---------------------------------------------------- */

/*
 * What a unit has to supply for stress_run() to drive it. Everything here is
 * container-specific and nothing else is: the phase structure, the clock, the
 * grace periods and the gc live in stress_run(), so the two units cannot drift
 * apart on the part that defines what is being tested.
 *
 * @op     one writer operation on one key: expire what it walks past, then touch
 *         or insert. Where a container reclaims *is* the difference between the
 *         two units, so this is where each puts it.
 * @reap   sweep the whole container for expired objects. The drain phase always
 *         calls it; @reap_each_round additionally calls it once a round, which is
 *         what an ordered container needs because it has no per-key locality to
 *         reclaim from.
 * @audit  optional, after every round: check whatever the container can check
 *         about itself while nothing is mutating it.
 * @wave   optional, at the end of every wave, when the drain has emptied it.
 */
struct stress_driver {
	void *ctx;
	void (*op)(void *ctx, u32 key, timestamp_t now);
	void (*reap)(void *ctx, timestamp_t now);
	void (*audit)(void *ctx);
	void (*wave)(void *ctx);
	bool reap_each_round;
};

/* One round of traffic, then a grace period and the memory policy. */
static inline void
stress_round(struct stress_arena *a, const struct stress_driver *d,
             unsigned *seed, timestamp_t now)
{
	unsigned op;

	for (op = 0; op < STRESS_OPS; op++)
		d->op(d->ctx, stress_rand(seed) % STRESS_KEYS, now);
	if (d->reap_each_round)
		d->reap(d->ctx, now);
	stress_drain(a);                   /* one grace period a round, at least */
	stress_gc(a, now);
	if (d->audit)
		d->audit(d->ctx);
	a->m.rounds++;
}

/*
 * Drive up to @waves waves of press, churn and drain (see the phase notes at the
 * top). Returns the virtual clock it left off at, so the caller can carry on.
 *
 * Bounded by wall clock as well as by wave count, and that is not a nicety. One
 * wave establishes every property the run asserts - it grows, it exhausts, it
 * expires, it retires through grace periods, it shrinks, and it ends empty - so
 * the waves after the first are soak, and soak is exactly what should give way
 * when each grace period suddenly costs a hundred times more. Under valgrind or a
 * sanitizer that is the difference between a unit that still runs there and one
 * that has to be excluded; the assertions are identical either way.
 *
 * HPC_STRESS_SCALE raises the cap and the budget together, so a soak run is
 * genuinely longer rather than just permitted to be.
 */
static inline timestamp_t
stress_run(struct stress_arena *a, const struct stress_driver *d,
           unsigned waves, timestamp_t now)
{
	unsigned long budget = (unsigned long)STRESS_BUDGET_MS * stress_scale();
	struct timespec t0;
	unsigned wave, round, i;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (wave = 0; wave < waves; wave++) {
		unsigned seed = 0x2545F491u + wave;

		a->cool = false;               /* press: fill to the ceiling */
		for (round = 0; round < STRESS_PRESS; round++) {
			now += STRESS_TICK_MS;
			stress_round(a, d, &seed, now);
		}

		a->cool = true;                /* churn: cold tail, hot core */
		for (round = 0; round < STRESS_CHURN; round++) {
			now += STRESS_TICK_MS;
			stress_round(a, d, &seed, now);
		}

		a->cool = false;               /* drain: everything dies */
		for (round = 0; round < STRESS_DRAIN; round++) {
			now += STRESS_TTL_MS;
			d->reap(d->ctx, now);
			stress_drain(a);
			for (i = 0; i < 32 && stress_gc(a, now) < 0; i++)
				;              /* shrink until it settles */
			if (d->audit)
				d->audit(d->ctx);
			a->m.rounds++;
		}
		if (d->wave)
			d->wave(d->ctx);
		a->m.waves++;
		if (stress_ms_since(&t0) >= budget)
			break;                 /* the soak has had its time */
	}
	return now;
}

/* ---- reader threads ------------------------------------------------------ */

/*
 * A rendezvous, so the churn provably overlaps the readers rather than racing
 * their startup: every reader registers with RCU, arrives, and waits; the writer
 * waits for all of them, then opens the gate and starts working. Built from a
 * mutex and a condition variable rather than pthread_barrier_t, which is
 * optional in POSIX and absent from some libcs this tree builds against.
 */
struct stress_gate {
	pthread_mutex_t lock;
	pthread_cond_t cv;
	unsigned arrived;
	unsigned want;
	int open;
};

static inline void
stress_gate_init(struct stress_gate *g, unsigned want)
{
	memset(g, 0, sizeof(*g));
	g->want = want;
	pthread_mutex_init(&g->lock, NULL);
	pthread_cond_init(&g->cv, NULL);
}

static inline void
stress_gate_fini(struct stress_gate *g)
{
	pthread_cond_destroy(&g->cv);
	pthread_mutex_destroy(&g->lock);
}

/* reader side: count in, then wait to be let go */
static inline void
stress_gate_arrive(struct stress_gate *g)
{
	pthread_mutex_lock(&g->lock);
	g->arrived++;
	pthread_cond_broadcast(&g->cv);
	while (!g->open)
		pthread_cond_wait(&g->cv, &g->lock);
	pthread_mutex_unlock(&g->lock);
}

/* writer side: block until every reader is up, then release them all */
static inline void
stress_gate_open(struct stress_gate *g)
{
	pthread_mutex_lock(&g->lock);
	while (g->arrived < g->want)
		pthread_cond_wait(&g->cv, &g->lock);
	g->open = 1;
	pthread_cond_broadcast(&g->cv);
	pthread_mutex_unlock(&g->lock);
}

/*
 * Per-reader state. Each thread owns its metrics outright and they are summed
 * once it has been joined, so the hot path has no atomics in it; a cache line
 * apiece keeps the writer's stores off them.
 */
struct stress_reader {
	struct reader_measure m;
	struct stress_arena *arena;
	void *container;              /* the table or the tree, unit's choice  */
	struct stress_gate *gate;
	volatile int *stop;
	unsigned seed;
	unsigned id;
} _align(CPU_CACHE_LINE);

/* Under QSBR nothing marks the end of a reader, so it has to say so itself. */
static inline void
stress_quiescent(void)
{
#ifdef CONFIG_RCU_QSBR
	rcu_quiescent_state();
#endif
}

static inline void
stress_reader_register(void)
{
#ifndef CONFIG_RCU_BP
	rcu_register_thread();                  /* bp registers on first use */
#endif
}

static inline void
stress_reader_unregister(void)
{
#ifndef CONFIG_RCU_BP
	rcu_unregister_thread();
#endif
}

/*
 * The integrity check a reader performs on every object it dereferences. Counts
 * rather than asserts, because a cmocka assertion from a non-main thread is a
 * longjmp out of the wrong stack; the run fails on the aggregated count instead.
 */
static inline bool
stress_reader_check(struct stress_reader *r, const void *blk)
{
	r->m.checks++;
	if (stress_valid(blk, r->arena->block_size, STRESS_PROBE))
		return true;
	r->m.torn++;
	return false;
}

/*
 * stress_reader_dwell - park inside the read-side section, still holding @blk,
 * then look at it again.
 *
 * A traversal that dereferences each node and moves straight on holds a pointer
 * for a few nanoseconds, which is far too narrow a window to overlap the moment
 * the writer frees a block and the slab releases its pages. Every so often a
 * reader therefore stops mid-section and waits, exactly like the parked reader in
 * slab_rcu.c but under load, and re-checks what it is holding on the way out.
 * That is the window in which a missing grace period is visible, and widening it
 * on purpose is what makes this test able to fail:
 *
 *   with the retire discipline removed from stress_drain(), these units report
 *   torn objects and fail; with it in place they never do.
 *
 * It also puts the other half of the contract under load: a parked reader holds
 * every grace period back for as long as it is parked, so the writer's
 * synchronize_rcu() really is waiting for readers rather than returning
 * immediately.
 */
static inline void
stress_reader_dwell(struct stress_reader *r, const void *blk)
{
	struct timespec ts = { 0, STRESS_DWELL_NS };

	if (!blk)
		return;
	r->m.dwells++;
	nanosleep(&ts, NULL);
	stress_reader_check(r, blk);
}

/* Is this the section that parks? Cheap, and spread out by the section count. */
static inline bool
stress_reader_should_dwell(const struct stress_reader *r)
{
	return (r->m.sections & (STRESS_DWELL_EVERY - 1)) == 0;
}

/*
 * Start @n readers on @fn and wait until all of them are inside a read-side
 * section; returns with the gate open and the churn free to begin.
 */
static inline void
stress_readers_start(pthread_t *th, struct stress_reader *r, unsigned n,
                     void *(*fn)(void *), struct stress_arena *a,
                     void *container, struct stress_gate *gate,
                     volatile int *stop)
{
	unsigned i;

	stress_gate_init(gate, n);
	for (i = 0; i < n; i++) {
		memset(&r[i], 0, sizeof(r[i]));
		r[i].arena = a;
		r[i].container = container;
		r[i].gate = gate;
		r[i].stop = stop;
		r[i].seed = 0x9E3779B9u * (i + 1);
		r[i].id = i;
		assert_int_equal(pthread_create(&th[i], NULL, fn, &r[i]), 0);
	}
	stress_gate_open(gate);
}

/*
 * Stop the readers, join them, and sum their metrics into @out.
 *
 * Every one of them must have reported in and made progress: a reader that never
 * entered a section, or never saw an object, would make the whole run a
 * single-threaded one that happens to pass. That is the "were the threads really
 * running together" check, and it is why @threads is a metric.
 */
static inline void
stress_readers_join(pthread_t *th, struct stress_reader *r, unsigned n,
                    volatile int *stop, struct stress_gate *gate,
                    struct reader_measure *out)
{
	unsigned i;

	CMM_STORE_SHARED(*stop, 1);
	for (i = 0; i < n; i++) {
		assert_int_equal(pthread_join(th[i], NULL), 0);
		assert_int_equal(r[i].m.threads, 1);
		assert_true(r[i].m.sections > 0);
		assert_true(r[i].m.visits > 0);
		assert_int_equal(r[i].m.torn, 0);
		measure_aggregate(reader, out, &r[i].m);
	}
	assert_int_equal(out->threads, n);
	/* the widened window really opened; without it this test cannot fail */
	assert_true(out->dwells > 0);
	stress_gate_fini(gate);
}

/* ---- the report ---------------------------------------------------------- */

static inline const char *
stress_bytes(u64 b, char *buf, size_t n)
{
	if (b >= (1u << 20))
		snprintf(buf, n, "%.1f MiB", (double)b / (1u << 20));
	else if (b >= (1u << 10))
		snprintf(buf, n, "%.1f KiB", (double)b / (1u << 10));
	else
		snprintf(buf, n, "%llu B", (unsigned long long)b);
	return buf;
}

/*
 * Metric names and descriptions are metadata and are emitted only under
 * CONFIG_MEASURE (see <hpc/measure.h>); the writer and reader counters
 * themselves are kept in every build, because this harness reads them back.
 * So label a row with its name where there is one, and with its index - which
 * the fixed column order makes unambiguous - where the strings were compiled
 * out, rather than printing a column of unnamed numbers.
 */
static inline const char *
stress_label(const char *name, unsigned i, char *buf, size_t n)
{
	if (name && name[0])
		return name;
	snprintf(buf, n, "[%u]", i);
	return buf;
}

/*
 * Print what the run did, as TAP comments. The three metric blocks are generic
 * walks over the metric tables, so a counter added to WRITER_METRICS or
 * READER_METRICS shows up here without touching this function.
 */
static inline void
stress_report(const char *unit, struct stress_arena *a,
              const struct reader_measure *rm, unsigned readers,
              unsigned long ms)
{
	struct slab *s = stress_slab(a);
	unsigned bs = a->block_size;
	char b1[32], b2[32], b3[32], b4[32], b5[32];
	char lb[16];   /* metric label, or its index where names are compiled out */

	printf("#\n# %s stress: %u readers + 1 writer, %lu ms"
	       " (HPC_STRESS_SCALE=%u)\n", unit, readers, ms, stress_scale());

	printf("#   layout: block %u B, grain %u blocks (%s),"
	       " reservation %s, policy min %u max %u blocks\n",
	       bs, slab_grain(s), stress_bytes(slab_grain_bytes(s), b1, sizeof(b1)),
	       stress_bytes((u64)s->total * bs, b2, sizeof(b2)),
	       slab_policy_min(s), slab_policy_max(s));

	/* One buffer per conversion: they are all live at the call. */
	printf("#   memory: committed %s (peak %s), in use %s in %u blocks"
	       " (peak %s), %s handed out over the run\n",
	       stress_bytes(slab_committed_bytes(s), b1, sizeof(b1)),
	       stress_bytes((u64)a->m.peak * bs, b2, sizeof(b2)),
	       stress_bytes((u64)slab_used(s) * bs, b3, sizeof(b3)),
	       slab_used(s),
	       stress_bytes((u64)a->m.peak_live * bs, b4, sizeof(b4)),
	       stress_bytes(a->m.bytes, b5, sizeof(b5)));

	printf("#   writer:\n");
	measure_for_each(writer, i)
		printf("#     %-10s %12llu   %s\n",
		       stress_label(measure_name(writer, i), i, lb, sizeof(lb)),
		       (unsigned long long)measure_at(writer, &a->m, i),
		       measure_desc(writer, i));

	printf("#   readers (%u threads, aggregated):\n", readers);
	measure_for_each(reader, i)
		printf("#     %-10s %12llu   %s\n",
		       stress_label(measure_name(reader, i), i, lb, sizeof(lb)),
		       (unsigned long long)measure_at(reader, rm, i),
		       measure_desc(reader, i));

#ifdef CONFIG_MEASURE
	printf("#   slab (its own counters):\n");
	measure_for_each(slab, i)
		printf("#     %-10s %12llu   %s\n", measure_name(slab, i),
		       (unsigned long long)measure_at(slab, &a->slab_m, i),
		       measure_desc(slab, i));
#endif
	fflush(stdout);
}

/*
 * The state the arena must be in once the readers are gone and the last batch
 * has been drained: every block the slab thinks is in use is an object the
 * container still holds, and every one of those is still a valid object.
 *
 * @linked is the count the caller arrived at by walking its container, which is
 * the whole point - it is derived independently of the allocator's bookkeeping.
 */
static inline void
stress_assert_settled(struct stress_arena *a, u32 linked)
{
	assert_int_equal(a->nretire, 0);
	assert_int_equal(slab_cache_live(&a->cache), linked);
	assert_int_equal(slab_used(stress_slab(a)), linked);
	assert_true(slab_committed(stress_slab(a)) >=
	            slab_policy_min(stress_slab(a)));

#ifdef CONFIG_MEASURE
	/* the slab counted the same events the test did */
	assert_int_equal(a->slab_m.alloc, a->m.inserts);
	assert_int_equal(a->slab_m.free, a->m.retires);
	assert_int_equal(a->slab_m.used, linked);
	assert_int_equal(a->slab_m.committed, slab_committed(stress_slab(a)));
	assert_int_equal(a->slab_m.grow, a->m.grows);
	assert_int_equal(a->slab_m.shrink, a->m.shrinks);
	assert_int_equal(a->slab_m.commit, a->m.commit);
	assert_int_equal(a->slab_m.reclaim, a->m.reclaim);
	assert_int_equal(a->slab_m.fail, a->m.failures);
#endif
}

/*
 * What the churn has to have achieved for the run to mean anything: memory grew,
 * memory was handed back to the OS, objects died of their deadlines, and the
 * ceiling was actually met. A stress test that quietly did none of these would
 * pass on every assertion and prove nothing.
 */
static inline void
stress_assert_exercised(struct stress_arena *a)
{
	assert_true(a->m.grows > 0);
	assert_true(a->m.shrinks > 0);
	assert_true(a->m.reclaim > 0);
	assert_true(a->m.expiries > 0);
	assert_true(a->m.gps > 0);
	assert_true(a->m.retires > 0);
	assert_true(a->m.failures > 0);
	assert_true(a->m.peak > slab_policy_min(stress_slab(a)));
}

#endif/*__HPC_TEST_STRESS_UTIL_H__*/
