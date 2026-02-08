/*
 * Unit tests for the RCU-safe slab <mem/slab_rcu.h>: planning a grow or shrink,
 * executing it on a tick, and above all not releasing a retired range while a
 * reader is still parked inside a read-side section holding a pointer into it.
 *
 * This unit exists only when CONFIG_RCU is enabled (see the Kbuild), so nothing
 * here is conditional. Unlike the other _rcu units it is genuinely threaded: the
 * property under test is a race, and a single-threaded test cannot establish that
 * a grace period held a release back - only that the state machine has the right
 * shape. Both are checked, the state machine first.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <setjmp.h>
#include <cmocka.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#include <hpc/compiler.h>
#include <mem/slab_rcu.h>

/*
 * Every block, whenever it is handed out, is stamped with a self-describing
 * record. A reader asserts only that what it reads is *a* valid record - not that
 * it is still the record it first saw, because a freed block may legitimately be
 * handed out again within a grace period (the SLAB_TYPESAFE_BY_RCU contract in
 * <mem/slab_rcu.h>). What must never happen is reading a block that has had its
 * pages released underneath it: madvise(MADV_DONTNEED) makes an anonymous page
 * read back as zeroes, so the magic would be 0 and the checksum would not match.
 * That is the failure this stamp is shaped to catch.
 *
 * The stamp sits at STAMP_OFF, not at offset 0, because the first word of a *free*
 * block is the allocator's free-list link (struct slab_node overlays the payload -
 * see <mem/slab.h>). Writing a stamp at offset 0 into a block that is on the free
 * list corrupts that list. Past the link, a stamp survives a free, a reuse and a
 * retire, and is destroyed by exactly one thing: the pages going away. Which is
 * the only thing being tested for.
 *
 * It is a single aligned 64-bit word, and that is the whole trick. A stamp made of
 * separate fields published magic-last is only safe the first time a block is
 * stamped: on a *re-stamp* the block already carries a valid magic from its
 * previous incarnation, so there is no point at which the record reads as invalid
 * while the other fields are being rewritten, and a reader parked on the old
 * pointer can pass the magic gate and then read the new seq against the old
 * checksum. That is a legal reuse - exactly the one the header permits within a
 * grace period - reported as if the pages had been released. One word, one store,
 * one load: a reader sees either incarnation whole, and never a mixture.
 */
#define STAMP_MAGIC 0x5AB57A11u
#define STAMP_OFF   64u

struct stamp {
	u64 word;                     /* seq in the high half, checksum in the low */
};

static struct stamp *
stamp_of(void *p)
{
	return (struct stamp *)((u8 *)p + STAMP_OFF);
}

static u32
stamp_csum(u32 seq)
{
	return (seq * 2654435761u) ^ STAMP_MAGIC;
}

/*
 * A released page reads back as zeroes, and zero is not a stamp: it decodes to
 * seq 0, whose checksum is STAMP_MAGIC and so cannot be the zero it was read
 * with. Which is what makes the checksum alone enough here, with no separate
 * magic to gate it.
 */
static u64
stamp_encode(u32 seq)
{
	return ((u64)seq << 32) | stamp_csum(seq);
}

static void
stamp_write(void *p, u32 seq)
{
	CMM_STORE_SHARED(stamp_of(p)->word, stamp_encode(seq));
}

static bool
stamp_valid(const void *p)
{
	u64 w = CMM_LOAD_SHARED(stamp_of((void *)p)->word);
	return (u32)w == stamp_csum((u32)(w >> 32));
}

/* ---- state machine, single threaded --------------------------------------- */

/*
 * A planned grow needs no grace period: it commits blocks nobody could hold a
 * pointer to, so the tick performs it there and then and the slab never enters
 * the retired state.
 */
static void
test_grow_needs_no_grace_period(void **state)
{
	(void)state;
	struct slab_policy pol = { .min = 1, .max = 64, .grow_step = 1 };
	struct slab_rcu r;
	u32 before;

	assert_int_equal(slab_rcu_init(&r, CPU_PAGE_SIZE, &pol, 0), 0);
	before = slab_committed(slab_rcu_slab(&r));

	assert_int_equal(slab_rcu_plan_grow(&r, 4), 4);
	assert_true(slab_rcu_planned(&r));

	/* first tick is always due */
	assert_int_equal(slab_rcu_tick(&r, 1000), 4);
	assert_int_equal(slab_committed(slab_rcu_slab(&r)), before + 4);
	assert_int_equal(slab_rcu_state(&r), SLAB_RCU_IDLE);
	assert_int_equal(slab_rcu_retiring(&r), 0);
	assert_false(slab_rcu_planned(&r));
	assert_int_equal(slab_rcu_stat(&r)->grows, 1);
	assert_int_equal(slab_rcu_stat(&r)->retires, 0);

	slab_rcu_fini(&r);
}

/*
 * A planned shrink retires the tail on one tick and releases it on a later one.
 * With no reader anywhere the grace period is immediate, but it is still a
 * separate step: committed drops at the retire, the pages go back at the release.
 */
static void
test_shrink_retires_then_releases(void **state)
{
	(void)state;
	struct slab_policy pol = { .min = 0, .max = 64, .grow_step = 4 };
	struct slab_rcu r;
	u32 i;

	assert_int_equal(slab_rcu_init(&r, CPU_PAGE_SIZE, &pol, 0), 0);
	slab_rcu_set_interval(&r, 0);                  /* every call is due */
	assert_int_equal(slab_grow(slab_rcu_slab(&r), 8), 8);

	/* touch all eight so they are resident, then leave them free */
	for (i = 0; i < 8; i++)
		stamp_write(slab_at(slab_rcu_slab(&r), i), i);

	assert_int_equal(slab_rcu_plan_shrink(&r, 4), 4);
	assert_int_equal(slab_rcu_tick(&r, 1000), -4);

	/* retired: out of the committed prefix, still resident and readable */
	assert_int_equal(slab_rcu_state(&r), SLAB_RCU_RETIRED);
	assert_int_equal(slab_committed(slab_rcu_slab(&r)), 4);
	assert_int_equal(slab_rcu_retiring(&r), 4);
	assert_int_equal(slab_rcu_resident(&r), 8);
	for (i = 4; i < 8; i++)
		assert_true(stamp_valid(slab_at(slab_rcu_slab(&r), i)));

	/* the release itself is not a change in committed, so the tick returns 0 */
	assert_int_equal(slab_rcu_sync(&r), 4);
	assert_int_equal(slab_rcu_state(&r), SLAB_RCU_IDLE);
	assert_int_equal(slab_rcu_retiring(&r), 0);
	assert_int_equal(slab_rcu_resident(&r), 4);
	assert_int_equal(slab_rcu_stat(&r)->releases, 1);
	assert_int_equal(slab_rcu_stat(&r)->blocks_released, 4);

	/*
	 * And now the pages really are gone: they read back as zeroes. That is
	 * a property of a backend whose release releases (SLAB_VM_RELEASES) -
	 * madvise(MADV_DONTNEED) over an anonymous mapping. On the libc heap
	 * SLAB_VM_RELEASE is a no-op, so the assertion to make is the opposite
	 * one: the bookkeeping above happened all the same, and the memory is
	 * still there with its stamps intact.
	 */
	for (i = 4; i < 8; i++) {
		if (SLAB_VM_RELEASES)
			assert_false(stamp_valid(slab_at(slab_rcu_slab(&r), i)));
		else
			assert_true(stamp_valid(slab_at(slab_rcu_slab(&r), i)));
	}

	slab_rcu_fini(&r);
}

/*
 * Growth reclaims a retired range instead of racing the release for it. Nothing
 * was released, so the blocks come back with their contents intact and without
 * faulting - which is also how we can tell the cancel really did reuse them.
 */
static void
test_grow_cancels_a_pending_retire(void **state)
{
	(void)state;
	struct slab_policy pol = { .min = 0, .max = 64, .grow_step = 4 };
	struct slab_rcu r;
	u32 i;

	assert_int_equal(slab_rcu_init(&r, CPU_PAGE_SIZE, &pol, 0), 0);
	slab_rcu_set_interval(&r, 0);
	assert_int_equal(slab_grow(slab_rcu_slab(&r), 8), 8);
	for (i = 0; i < 8; i++)
		stamp_write(slab_at(slab_rcu_slab(&r), i), 100 + i);

	slab_rcu_plan_shrink(&r, 4);
	assert_int_equal(slab_rcu_tick(&r, 1000), -4);
	assert_int_equal(slab_rcu_state(&r), SLAB_RCU_RETIRED);

	/* plan a grow instead; the tick cancels the retire and commits again */
	slab_rcu_plan_grow(&r, 4);
	assert_int_equal(slab_rcu_tick(&r, 2000), 4);
	assert_int_equal(slab_rcu_state(&r), SLAB_RCU_IDLE);
	assert_int_equal(slab_committed(slab_rcu_slab(&r)), 8);
	assert_int_equal(slab_rcu_stat(&r)->cancels, 1);
	assert_int_equal(slab_rcu_stat(&r)->releases, 0);

	/* never released, so the old contents survived */
	for (i = 4; i < 8; i++)
		assert_true(stamp_valid(slab_at(slab_rcu_slab(&r), i)));

	slab_rcu_fini(&r);
}

/* An allocation that has to grow does the same cancelling. */
static void
test_alloc_cancels_a_pending_retire(void **state)
{
	(void)state;
	struct slab_policy pol = { .min = 0, .max = 64, .grow_step = 4 };
	struct slab_rcu r;
	void *p;
	u32 i;

	assert_int_equal(slab_rcu_init(&r, CPU_PAGE_SIZE, &pol, 0), 0);
	slab_rcu_set_interval(&r, 0);
	assert_int_equal(slab_grow(slab_rcu_slab(&r), 4), 4);

	/* hold every committed block so the next alloc must grow */
	for (i = 0; i < 4; i++)
		assert_non_null(slab_rcu_alloc(&r));
	assert_int_equal(slab_avail(slab_rcu_slab(&r)), 0);

	/* retire is impossible with everything live, so free one grain first */
	for (i = 0; i < 4; i++)
		slab_rcu_free(&r, slab_at(slab_rcu_slab(&r), i));
	slab_rcu_plan_shrink(&r, 4);
	assert_int_equal(slab_rcu_tick(&r, 1000), -4);
	assert_int_equal(slab_rcu_state(&r), SLAB_RCU_RETIRED);
	assert_int_equal(slab_committed(slab_rcu_slab(&r)), 0);

	/* an exhausted alloc grows, which must abandon the retire */
	p = slab_rcu_alloc(&r);
	assert_non_null(p);
	assert_int_equal(slab_rcu_state(&r), SLAB_RCU_IDLE);
	assert_int_equal(slab_rcu_stat(&r)->cancels, 1);
	assert_int_equal(slab_rcu_stat(&r)->releases, 0);

	slab_rcu_fini(&r);
}

/* Only one retire is in flight at a time; a second plan waits its turn. */
static void
test_one_retire_at_a_time(void **state)
{
	(void)state;
	struct slab_policy pol = { .min = 0, .max = 64, .grow_step = 4 };
	struct slab_rcu r;

	assert_int_equal(slab_rcu_init(&r, CPU_PAGE_SIZE, &pol, 0), 0);
	slab_rcu_set_interval(&r, 0);
	assert_int_equal(slab_grow(slab_rcu_slab(&r), 8), 8);

	slab_rcu_plan_shrink(&r, 2);
	assert_int_equal(slab_rcu_tick(&r, 1000), -2);
	assert_int_equal(slab_rcu_retiring(&r), 2);

	/*
	 * A second shrink planned while the first is still retired is dropped:
	 * the tick has one rcu_head, so one grace period can be in flight. Note
	 * the tick that drops it may not release the first one either - the
	 * callback runs on liburcu's thread and need not have fired yet, even
	 * with no readers at all. Non-blocking means non-blocking.
	 */
	slab_rcu_plan_shrink(&r, 2);
	assert_int_equal(slab_rcu_tick(&r, 2000), 0);
	assert_int_equal(slab_rcu_retiring(&r), 2);      /* still the first one */

	/* sync is the blocking way to get there */
	assert_int_equal(slab_rcu_sync(&r), 2);
	assert_int_equal(slab_rcu_stat(&r)->releases, 1);
	assert_int_equal(slab_rcu_state(&r), SLAB_RCU_IDLE);

	/* with the slot free, the next plan retires normally */
	slab_rcu_plan_shrink(&r, 2);
	assert_int_equal(slab_rcu_tick(&r, 3000), -2);
	assert_int_equal(slab_rcu_retiring(&r), 2);

	slab_rcu_sync(&r);
	slab_rcu_fini(&r);
}

/* The tick rate-limits itself, so an event loop can call it every time round. */
static void
test_tick_interval(void **state)
{
	(void)state;
	struct slab_policy pol = { .min = 0, .max = 64, .grow_step = 4 };
	struct slab_rcu r;

	assert_int_equal(slab_rcu_init(&r, CPU_PAGE_SIZE, &pol, 1000), 0);

	slab_rcu_plan_grow(&r, 4);
	assert_int_equal(slab_rcu_tick(&r, 5000), 4);      /* first is due */

	slab_rcu_plan_grow(&r, 4);
	assert_int_equal(slab_rcu_tick(&r, 5001), 0);      /* inside the period */
	assert_int_equal(slab_rcu_tick(&r, 5999), 0);
	assert_true(slab_rcu_planned(&r));                 /* plan still pending */
	assert_int_equal(slab_rcu_tick(&r, 6000), 4);      /* period elapsed */
	assert_false(slab_rcu_planned(&r));

	slab_rcu_fini(&r);
}

/* slab_rcu_plan_gc records what slab_gc would have done, rather than doing it. */
static void
test_plan_gc_records_instead_of_acting(void **state)
{
	(void)state;
	struct slab_policy pol = {
		.min = 0, .max = 64, .grow_step = 4,
		.grow_usage_pct = 50, .shrink_usage_pct = 25,
		.shrink_release_pct = 50, .shrink_after = 0,
	};
	struct slab_rcu r;

	assert_int_equal(slab_rcu_init(&r, CPU_PAGE_SIZE, &pol, 0), 0);
	assert_int_equal(slab_grow(slab_rcu_slab(&r), 8), 8);

	/* all free -> usage 0% <= 25% -> a shrink is planned, not performed */
	assert_true(slab_rcu_plan_gc(&r, 1000) < 0);
	assert_int_equal(slab_committed(slab_rcu_slab(&r)), 8);   /* untouched */
	assert_true(slab_rcu_planned(&r));

	assert_true(slab_rcu_tick(&r, 2000) < 0);                 /* now it acts */
	assert_true(slab_rcu_retiring(&r) > 0);
	slab_rcu_sync(&r);

	/* drain the free list so usage reaches the grow watermark */
	while (slab_rcu_alloc(&r))
		;
	assert_int_equal(slab_avail(slab_rcu_slab(&r)), 0);
	assert_true(slab_rcu_plan_gc(&r, 3000) >= 0);
	slab_rcu_fini(&r);
}

/* ---- the race: a parked reader holds the release back -------------------- */

struct parked {
	struct slab_rcu *r;
	void *block;                  /* block inside the doomed tail        */
	pthread_mutex_t lock;
	pthread_cond_t cv;
	int in_section;               /* reader is inside rcu_read_lock()    */
	int may_leave;                /* main says: leave the section        */
	int saw_valid_on_entry;
	int saw_valid_before_leaving;
};

static void *
parked_reader(void *arg)
{
	struct parked *p = (struct parked *)arg;

	slab_rcu_reader_register();
	slab_rcu_read_lock();

	p->saw_valid_on_entry = stamp_valid(p->block) ? 1 : 0;

	pthread_mutex_lock(&p->lock);
	p->in_section = 1;
	pthread_cond_broadcast(&p->cv);
	while (!p->may_leave)
		pthread_cond_wait(&p->cv, &p->lock);
	pthread_mutex_unlock(&p->lock);

	/*
	 * The assertion that matters: still inside the same read-side section,
	 * after the writer has freed this block, planned a shrink over it and
	 * ticked. If the release were allowed to happen here, this page would
	 * read back as zeroes.
	 */
	p->saw_valid_before_leaving = stamp_valid(p->block) ? 1 : 0;

	slab_rcu_read_unlock();
	slab_rcu_reader_unregister();
	return NULL;
}

static void
test_parked_reader_holds_release_back(void **state)
{
	(void)state;
	struct slab_policy pol = { .min = 0, .max = 64, .grow_step = 4 };
	struct slab_rcu r;
	struct parked p;
	pthread_t th;
	void *blk[8];
	u32 i, doomed;

	assert_int_equal(slab_rcu_init(&r, CPU_PAGE_SIZE, &pol, 0), 0);
	slab_rcu_set_interval(&r, 0);

	assert_int_equal(slab_grow(slab_rcu_slab(&r), 8), 8);
	for (i = 0; i < 8; i++) {
		blk[i] = slab_rcu_alloc(&r);
		assert_non_null(blk[i]);
		stamp_write(blk[i], i);
	}

	/* the reader will hold the highest-index block, which the shrink targets */
	doomed = 7;
	memset(&p, 0, sizeof(p));
	p.r = &r;
	p.block = slab_at(slab_rcu_slab(&r), doomed);
	pthread_mutex_init(&p.lock, NULL);
	pthread_cond_init(&p.cv, NULL);

	assert_int_equal(pthread_create(&th, NULL, parked_reader, &p), 0);

	/* wait until the reader is inside its read-side section */
	pthread_mutex_lock(&p.lock);
	while (!p.in_section)
		pthread_cond_wait(&p.cv, &p.lock);
	pthread_mutex_unlock(&p.lock);
	assert_int_equal(p.saw_valid_on_entry, 1);

	/* writer frees the tail and plans a shrink over it */
	for (i = 4; i < 8; i++)
		slab_rcu_free(&r, slab_at(slab_rcu_slab(&r), i));
	slab_rcu_plan_shrink(&r, 4);
	assert_int_equal(slab_rcu_tick(&r, 1000), -4);
	assert_int_equal(slab_rcu_state(&r), SLAB_RCU_RETIRED);

	/*
	 * Tick repeatedly while the reader is parked. The grace period cannot
	 * complete, so not one of these may release. This is the whole point.
	 */
	for (i = 0; i < 50; i++) {
		assert_int_equal(slab_rcu_tick(&r, 2000 + i), 0);
		assert_int_equal(slab_rcu_state(&r), SLAB_RCU_RETIRED);
		assert_int_equal(slab_rcu_stat(&r)->releases, 0);
	}
	assert_true(slab_rcu_stat(&r)->deferred >= 50);
	assert_int_equal(slab_rcu_retiring(&r), 4);

	/* let the reader finish; it re-reads its block first */
	pthread_mutex_lock(&p.lock);
	p.may_leave = 1;
	pthread_cond_broadcast(&p.cv);
	pthread_mutex_unlock(&p.lock);
	assert_int_equal(pthread_join(th, NULL), 0);

	/* it saw valid data on the way out, with the shrink already retired */
	assert_int_equal(p.saw_valid_before_leaving, 1);

	/* now the grace period can complete and the release goes through */
	assert_int_equal(slab_rcu_sync(&r), 4);
	assert_int_equal(slab_rcu_stat(&r)->releases, 1);
	assert_int_equal(slab_rcu_retiring(&r), 0);

	pthread_cond_destroy(&p.cv);
	pthread_mutex_destroy(&p.lock);
	slab_rcu_fini(&r);
}

/* ---- the race, under load ------------------------------------------------ */

#define STRESS_SLOTS    32
#define STRESS_READERS  4
#define STRESS_RELEASES 8      /* releases the run must at least reach */

struct stress {
	struct slab_rcu *r;
	void **slot;                  /* published blocks, NULL when retired  */
	volatile int stop;
	unsigned long reads;          /* records validated                    */
	unsigned long invalid;        /* records that were NOT valid: bugs     */
};

static void *
stress_reader(void *arg)
{
	struct stress *s = (struct stress *)arg;
	unsigned long seed = 1;

	slab_rcu_reader_register();
	while (!CMM_LOAD_SHARED(s->stop)) {
		unsigned i;
		for (i = 0; i < 64; i++) {
			void *p;
			seed = seed * 1103515245u + 12345u;
			slab_rcu_read_lock();
			/*
			 * Read the slot, and keep using the pointer for the rest
			 * of this section - which is exactly the window a shrink
			 * must not release under.
			 */
			p = rcu_dereference(s->slot[(seed >> 8) % STRESS_SLOTS]);
			if (p) {
				if (stamp_valid(p))
					s->reads++;
				else
					s->invalid++;
				/* hold it a little longer, then look again */
				if (!stamp_valid(p))
					s->invalid++;
				else
					s->reads++;
			}
			slab_rcu_read_unlock();
		}
		slab_rcu_quiescent();
	}
	slab_rcu_reader_unregister();
	return NULL;
}

/*
 * Writer churns: publishes blocks, unpublishes and frees them, plans shrinks and
 * grows and ticks, while four readers dereference published blocks inside
 * read-side sections. Any release that happened under a reader would show up as
 * an invalid record - a page of zeroes where a stamp should be.
 */
static void
test_stress_readers_never_see_released_memory(void **state)
{
	(void)state;
	struct slab_policy pol = { .min = 0, .max = 256, .grow_step = 8 };
	struct slab_rcu r;
	struct stress s;
	void *slot[STRESS_SLOTS];
	pthread_t th[STRESS_READERS];
	unsigned long round;
	int i;

	assert_int_equal(slab_rcu_init(&r, CPU_PAGE_SIZE, &pol, 0), 0);
	slab_rcu_set_interval(&r, 0);
	assert_int_equal(slab_grow(slab_rcu_slab(&r), 128), 128);

	memset(slot, 0, sizeof(slot));
	memset(&s, 0, sizeof(s));
	s.r = &r;
	s.slot = slot;

	/*
	 * Publish the lowest-index blocks and leave the tail free, so a shrink has
	 * something to reclaim. The free list is LIFO, so a plain run of allocations
	 * hands out the *highest* indices first - which would park live blocks
	 * exactly where the tail needs to be free and nothing would ever be
	 * reclaimable. So take everything, then keep the low ones.
	 */
	{
		void *all[128];
		int n;
		for (n = 0; n < 128; n++) {
			all[n] = slab_rcu_alloc(&r);
			assert_non_null(all[n]);
		}
		for (n = 0; n < 128; n++) {
			u32 ix = slab_index(slab_rcu_slab(&r), all[n]);
			if (ix < STRESS_SLOTS) {
				stamp_write(all[n], ix);
				rcu_assign_pointer(slot[ix], all[n]);
			} else {
				slab_rcu_free(&r, all[n]);
			}
		}
	}

	for (i = 0; i < STRESS_READERS; i++)
		assert_int_equal(pthread_create(&th[i], NULL, stress_reader, &s), 0);

	/*
	 * Churn the published set while the working set oscillates, and force each
	 * reclaim through to its release with slab_rcu_sync() - which blocks on the
	 * grace period - so the release lands at a known point *while four readers
	 * are running*. That is the window under test.
	 *
	 * The non-blocking tick is deliberately not what this test waits on. Release
	 * latency there is liburcu's call_rcu worker getting scheduled, and a writer
	 * spinning in a loop like this one starves it: measured here, a release took
	 * some five hundred rounds to land, and occasionally far longer. The deferred
	 * path has its own test (test_parked_reader_holds_release_back); making this
	 * one wait on a worker thread only bought flakiness.
	 */
	for (round = 0; round < 64 * 40; round++) {
		unsigned k = (unsigned)(round % STRESS_SLOTS);
		void *p;

		/* unpublish and free one slot, then republish into it */
		p = slot[k];
		if (p) {
			rcu_assign_pointer(slot[k], NULL);
			slab_rcu_free(&r, p);
		}
		p = slab_rcu_alloc(&r);
		if (p) {
			stamp_write(p, (u32)(round + STRESS_SLOTS));
			rcu_assign_pointer(slot[k], p);
		}

		if (round % 64 != 0) {
			slab_rcu_tick(&r, (timestamp_t)round);
			continue;
		}
		if (slab_committed(slab_rcu_slab(&r)) > 64) {
			slab_rcu_plan_shrink(&r, 8);
			slab_rcu_tick(&r, (timestamp_t)round);   /* retires */
			slab_rcu_sync(&r);                       /* releases, blocking */
		} else {
			slab_rcu_plan_grow(&r, 8);
			slab_rcu_tick(&r, (timestamp_t)round);
		}
	}

	CMM_STORE_SHARED(s.stop, 1);
	for (i = 0; i < STRESS_READERS; i++)
		assert_int_equal(pthread_join(th[i], NULL), 0);

	/* the assertion: not one reader ever read a block whose pages had gone */
	assert_int_equal(s.invalid, 0);
	assert_true(s.reads > 0);

	/* and the churn really did exercise the deferred path */
	assert_true(slab_rcu_stat(&r)->retires >= STRESS_RELEASES);
	assert_true(slab_rcu_stat(&r)->releases >= STRESS_RELEASES);

	slab_rcu_sync(&r);
	slab_rcu_fini(&r);
}

int
main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_grow_needs_no_grace_period),
		cmocka_unit_test(test_shrink_retires_then_releases),
		cmocka_unit_test(test_grow_cancels_a_pending_retire),
		cmocka_unit_test(test_alloc_cancels_a_pending_retire),
		cmocka_unit_test(test_one_retire_at_a_time),
		cmocka_unit_test(test_tick_interval),
		cmocka_unit_test(test_plan_gc_records_instead_of_acting),
		cmocka_unit_test(test_parked_reader_holds_release_back),
		cmocka_unit_test(test_stress_readers_never_see_released_memory),
	};
	return cmocka_run_group_tests_name("slab_rcu", tests, NULL, NULL);
}
