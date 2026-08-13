/*
 * Unit tests for the compacting slab <mem/slab_compact.h>: the shrink that
 * moves live blocks out of the way instead of stalling on them.
 *
 * The one contract change against the plain slab is that pointers move, so the
 * harness here is the pattern real callers should copy: every object carries a
 * back-reference (the slot of the one table entry that points at it), and the
 * move callback repoints that single entry in O(1). Every unit then verifies
 * the objects through the table - if a relocation were ever missed or
 * misdelivered, the checksums would read garbage.
 *
 * As in the plain slab suite, the policy units use a block of exactly one
 * grain so a step of n blocks is a step of n under any grain, including the
 * 2 MB one CONFIG_MEM_HUGEPAGE selects; the sub-grain geometry has units of
 * its own.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>

#include <hpc/compiler.h>

#include <mem/slab.h>
#include <mem/slab_compact.h>

/* ---- the caller side: a table of pointers the callback keeps honest ------- */

#define APP_SLOTS 128

struct obj {
	u32 id;               /* the table slot that points at this object   */
	u32 gen;              /* varies the payload between reuses of a slot */
	u8  data[56];
};

struct app {
	struct slab_compact sc;
	struct obj *tab[APP_SLOTS];
	u32 relocations;      /* callback invocations                        */
	u32 mismatches;       /* callback found a table it could not fix     */
};

/*
 * The relocation callback, and the whole trick of surviving compaction:
 * identity is read from @to (the object already lives there; @from is dying),
 * and the back-reference makes the fix a single store.
 */
static void
on_move(struct slab *vm, void *from, void *to, void *arg)
{
	struct app *a = arg;
	struct obj *o = to;

	(void)vm;
	a->relocations++;
	if (o->id >= APP_SLOTS || a->tab[o->id] != from) {
		a->mismatches++;
		return;
	}
	a->tab[o->id] = to;
}

static void
fill_obj(struct obj *o, u32 id, u32 gen)
{
	unsigned j;
	o->id = id;
	o->gen = gen;
	for (j = 0; j < sizeof(o->data); j++)
		o->data[j] = (u8)(id * 31u + gen * 7u + j);
}

static int
check_obj(const struct obj *o, u32 id, u32 gen)
{
	unsigned j;
	if (o->id != id || o->gen != gen)
		return 0;
	for (j = 0; j < sizeof(o->data); j++)
		if (o->data[j] != (u8)(id * 31u + gen * 7u + j))
			return 0;
	return 1;
}

static void
app_init(struct app *a, unsigned block_size, const struct slab_policy *pol)
{
	memset(a->tab, 0, sizeof(a->tab));
	a->relocations = 0;
	a->mismatches = 0;
	assert_int_equal(slab_compact_init(&a->sc, block_size, pol,
					   on_move, a), 0);
	assert_true(slab_block_size(&a->sc.slab) >= sizeof(struct obj));
}

static struct obj *
app_alloc(struct app *a, u32 id, u32 gen)
{
	struct obj *o = slab_compact_alloc(&a->sc);
	if (!o)
		return NULL;
	fill_obj(o, id, gen);
	a->tab[id] = o;
	return o;
}

static void
app_free(struct app *a, u32 id)
{
	slab_compact_free(&a->sc, a->tab[id]);
	a->tab[id] = NULL;
}

/* Every live object, through the table - the pointers compaction must keep. */
static void
app_verify(struct app *a, u32 gen)
{
	u32 id;
	for (id = 0; id < APP_SLOTS; id++) {
		if (!a->tab[id])
			continue;
		assert_true(check_obj(a->tab[id], id, gen));
	}
	assert_int_equal(a->mismatches, 0);
}

/*
 * Structural invariants a compaction step must preserve: the free list holds
 * exactly the avail count, only unoccupied committed blocks, no cycle; the
 * occupancy bitmap agrees with the used count.
 */
static void
assert_slab_consistent(struct slab *vm)
{
	u32 i, on_list = 0, live = 0;

	for (i = vm->list; i != SLAB_NIL; ) {
		struct slab_node *node = slab_at(vm, i);
		assert_true(i < vm->committed);
		assert_false(BITSET_TEST(vm->map, i));
		on_list++;
		assert_true(on_list <= vm->committed);   /* cycle guard */
		i = node->avail;
	}
	assert_int_equal(on_list, slab_avail(vm));
	for (i = 0; i < vm->committed; i++)
		if (BITSET_TEST(vm->map, i))
			live++;
	assert_int_equal(live, slab_used(vm));
}

/* Allocate @n objects into slots 0..n-1 - alloc order is not index order
 * (the free list is LIFO), which is exactly why everything keys by table
 * slot and the tests reason about block indices via slab_index(). */
static void
app_fill(struct app *a, u32 n, u32 gen)
{
	u32 id;
	for (id = 0; id < n; id++)
		assert_non_null(app_alloc(a, id, gen));
}

/* Free every object living in a block below @index, keeping the tail live -
 * the worst case for a plain shrink: stragglers pinning the highest grains. */
static u32
app_free_below(struct app *a, u32 index)
{
	u32 id, freed = 0;
	for (id = 0; id < APP_SLOTS; id++) {
		if (!a->tab[id])
			continue;
		if (slab_index(&a->sc.slab, a->tab[id]) < index) {
			app_free(a, id);
			freed++;
		}
	}
	return freed;
}

/* ---- the mover ------------------------------------------------------------ */

/*
 * A contiguous live tail over a contiguous free front is one copy: four
 * blocks move as one memcpy, each announced separately, and the survivors
 * land in index order at the bottom.
 */
static void
test_bulk_move_single_copy(void **state)
{
	(void)state;
	struct slab_policy pol = { .min = 0, .max = 8, .grow_step = 8 };
	struct app a;
	u32 id;

	app_init(&a, SLAB_GRAIN_BYTES, &pol);
	assert_int_equal(slab_grow(&a.sc.slab, 8), 8);
	app_fill(&a, 8, 1);
	assert_true(slab_compact_packed(&a.sc));

	/* free the bottom half: live blocks 4..7 over free blocks 0..3 */
	assert_int_equal(app_free_below(&a, 4), 4);
	assert_false(slab_compact_packed(&a.sc));

	assert_int_equal(slab_compact_run(&a.sc, 0), 4);
	assert_int_equal(a.sc.stat.copies, 1);       /* one bulk memcpy      */
	assert_int_equal(a.sc.stat.moves, 4);
	assert_int_equal(a.relocations, 4);
	assert_true(slab_compact_packed(&a.sc));
	assert_slab_consistent(&a.sc.slab);
	app_verify(&a, 1);

	/* every survivor now sits in the first half of the space */
	for (id = 0; id < APP_SLOTS; id++)
		if (a.tab[id])
			assert_true(slab_index(&a.sc.slab, a.tab[id]) < 4);

	/* run moved blocks but released nothing; the tail is now harvestable
	 * by the plain primitive */
	assert_int_equal(slab_committed(&a.sc.slab), 8);
	assert_int_equal(slab_shrink(&a.sc.slab, 8), 4);
	assert_int_equal(slab_committed(&a.sc.slab), 4);
	app_verify(&a, 1);
	slab_compact_fini(&a.sc);
}

/* ---- the tick: dwell, defragment, release --------------------------------- */

static void
test_tick_defrags_then_releases(void **state)
{
	(void)state;
	struct slab_policy pol = {
		.min = 0, .max = 16, .grow_step = 16,
		.shrink_usage_pct = 30, .shrink_release_pct = 50,
		.shrink_after = 1000,
	};
	struct app a;
	timestamp_t t = 5000;
	u32 id;

	app_init(&a, SLAB_GRAIN_BYTES, &pol);
	assert_int_equal(slab_grow(&a.sc.slab, 16), 16);
	app_fill(&a, 16, 2);

	/* 4 live blocks at the very top, 12 free below: 25% usage and a tail
	 * a plain shrink cannot touch */
	assert_int_equal(app_free_below(&a, 12), 12);
	assert_int_equal(slab_shrink(&a.sc.slab, 16), 0);   /* pinned */
	assert_int_equal(slab_committed(&a.sc.slab), 16);

	/* the dwell runs exactly as in slab_gc: arm, wait, then act */
	assert_int_equal(slab_compact_tick(&a.sc, t, 0), 0);
	assert_int_equal(slab_compact_tick(&a.sc, t + 999, 0), 0);
	assert_int_equal(a.sc.stat.rounds, 0);
	assert_int_equal(a.sc.stat.moves, 0);

	/* dwell met: one tick moves the 4 stragglers down and releases 50%
	 * of the 12 free blocks - the release a plain gc could only wish for */
	assert_int_equal(slab_compact_tick(&a.sc, t + 1000, 0), -6);
	assert_int_equal(slab_committed(&a.sc.slab), 10);
	assert_int_equal(a.sc.stat.rounds, 1);
	assert_int_equal(a.sc.stat.moves, 4);
	assert_int_equal(a.sc.stat.releases, 1);
	assert_int_equal(a.sc.stat.blocks_released, 6);
	assert_false(slab_compacting(&a.sc));
	assert_slab_consistent(&a.sc.slab);
	app_verify(&a, 2);
	for (id = 0; id < APP_SLOTS; id++)
		if (a.tab[id])
			assert_true(slab_index(&a.sc.slab, a.tab[id]) < 4);

	/* usage is back over the watermark (4/10): the next tick is a no-op */
	assert_int_equal(slab_compact_tick(&a.sc, t + 1000, 0), 0);
	assert_int_equal(slab_committed(&a.sc.slab), 10);
	slab_compact_fini(&a.sc);
}

/*
 * The budget is the latency knob: with budget 1 the same round takes a tick
 * per block, every intermediate state is consistent, and the caller's
 * pointers are valid between every pair of ticks - which is where a real
 * event loop lives.
 */
static void
test_budget_slices_the_round(void **state)
{
	(void)state;
	struct slab_policy pol = {
		.min = 0, .max = 16, .grow_step = 16,
		.shrink_usage_pct = 30, .shrink_release_pct = 50,
		.shrink_after = 1000,
	};
	struct app a;
	timestamp_t t = 1000;
	u64 moved_before;
	int ticks = 0, r;

	app_init(&a, SLAB_GRAIN_BYTES, &pol);
	assert_int_equal(slab_grow(&a.sc.slab, 16), 16);
	app_fill(&a, 16, 3);
	assert_int_equal(app_free_below(&a, 12), 12);

	assert_int_equal(slab_compact_tick(&a.sc, t, 1), 0);      /* arm */

	/* drive the round tick by tick; the dwell gates only its start */
	do {
		moved_before = a.sc.stat.moves;
		r = slab_compact_tick(&a.sc, t + 1000, 1);
		assert_true(r <= 0);
		assert_true(a.sc.stat.moves - moved_before <= 1);
		assert_slab_consistent(&a.sc.slab);
		app_verify(&a, 3);           /* pointers hold between slices */
		ticks++;
		assert_true(ticks < 32);     /* the round must terminate     */
	} while (slab_compacting(&a.sc));

	/* same end state as the unbudgeted round, just in more ticks */
	assert_int_equal(slab_committed(&a.sc.slab), 10);
	assert_int_equal(a.sc.stat.moves, 4);
	assert_int_equal(a.sc.stat.blocks_released, 6);
	assert_int_equal(a.sc.stat.rounds, 1);
	assert_true(ticks > 1);
	slab_compact_fini(&a.sc);
}

/*
 * A round in flight is abandoned the moment usage recovers: the blocks
 * already moved stay moved, nothing more relocates, and - because the step
 * left the free list in ascending order - the allocations that lifted the
 * usage landed at the front rather than re-pinning the tail.
 */
static void
test_pressure_cancels_the_round(void **state)
{
	(void)state;
	struct slab_policy pol = {
		.min = 0, .max = 16, .grow_step = 16,
		.grow_usage_pct = 90,
		.shrink_usage_pct = 30, .shrink_release_pct = 50,
		.shrink_after = 1000,
	};
	struct app a;
	timestamp_t t = 1000;
	u64 moves;
	u32 id;

	app_init(&a, SLAB_GRAIN_BYTES, &pol);
	assert_int_equal(slab_grow(&a.sc.slab, 16), 16);
	app_fill(&a, 16, 4);
	assert_int_equal(app_free_below(&a, 12), 12);

	assert_int_equal(slab_compact_tick(&a.sc, t, 1), 0);       /* arm  */
	assert_int_equal(slab_compact_tick(&a.sc, t + 1000, 1), -1);
	assert_true(slab_compacting(&a.sc));
	moves = a.sc.stat.moves;

	/* the burst: five new sessions lift usage to 60% ... */
	for (id = 100; id < 105; id++) {
		assert_non_null(app_alloc(&a, id, 4));
		/* ... into the front of the space, thanks to the ordered list */
		assert_true(slab_index(&a.sc.slab, a.tab[id]) < 8);
	}

	/* ... and the next tick walks away from the round */
	assert_int_equal(slab_compact_tick(&a.sc, t + 1000, 1), 0);
	assert_false(slab_compacting(&a.sc));
	assert_int_equal(a.sc.stat.cancels, 1);
	assert_int_equal(a.sc.stat.moves, moves);      /* nothing else moved */
	assert_slab_consistent(&a.sc.slab);
	app_verify(&a, 4);
	slab_compact_fini(&a.sc);
}

/* ---- the memory is really given back --------------------------------------- */

#if SLAB_VM_RELEASES
static u32
resident_pages(void *base, size_t len)
{
	size_t pg = (size_t)sysconf(_SC_PAGESIZE);
	size_t i, n = (len + pg - 1) / pg;
	unsigned char *vec = malloc(n);
	u32 count = 0;

	assert_non_null(vec);
	assert_int_equal(mincore(base, len, vec), 0);
	for (i = 0; i < n; i++)
		count += vec[i] & 1;
	free(vec);
	return count;
}

/*
 * "Actually shrinks" measured, not inferred: after the round the reclaimed
 * tail has zero resident pages (madvise really dropped them), the survivors'
 * blocks are still resident, and a later allocation faults the released range
 * back in as usable memory.
 */
static void
test_release_returns_pages_to_the_os(void **state)
{
	(void)state;
	struct slab_policy pol = {
		.min = 0, .max = 8, .grow_step = 8,
		.shrink_usage_pct = 30, .shrink_release_pct = 100,
		.shrink_after = 0,
	};
	struct app a;
	struct slab *vm;
	u8 *base;
	struct obj *o;
	u32 id;

	app_init(&a, SLAB_GRAIN_BYTES, &pol);
	vm = slab_compact_slab(&a.sc);
	assert_int_equal(slab_grow(vm, 8), 8);
	app_fill(&a, 8, 5);
	base = vm->page;

	/* touch every page of every block, so residency reads all-or-nothing */
	for (id = 0; id < 8; id++)
		memset((u8 *)a.tab[id] + sizeof(struct obj), 0xee,
		       slab_block_size(vm) - sizeof(struct obj));
	assert_true(resident_pages(base, slab_committed_bytes(vm)) >=
		    8 * (SLAB_GRAIN_BYTES / CPU_PAGE_SIZE));

	/* two stragglers at the top; 100% of the free blocks may go */
	assert_int_equal(app_free_below(&a, 6), 6);
	assert_int_equal(slab_compact_tick(&a.sc, 0, 0), -6);
	assert_int_equal(slab_committed(vm), 2);
	app_verify(&a, 5);

	/* the reclaimed tail is gone from RSS; the survivors are not */
	assert_int_equal(resident_pages(base + slab_committed_bytes(vm),
					6ull * SLAB_GRAIN_BYTES), 0);
	assert_true(resident_pages(base, slab_committed_bytes(vm)) > 0);

	/* released memory comes back on demand, zero-filled and writable:
	 * the exhausted slab re-grows into the range it just returned */
	o = app_alloc(&a, 42, 6);
	assert_non_null(o);
	assert_true(slab_index(vm, o) >= 2);          /* in the released range */
	assert_true(check_obj(o, 42, 6));
	assert_slab_consistent(vm);
	slab_compact_fini(&a.sc);
}
#endif /* SLAB_VM_RELEASES */

/* ---- sub-grain geometry ------------------------------------------------------ */

/*
 * Blocks a sixteenth of a grain: eight stragglers spread over four grains pin
 * every one of them for the plain shrink, and the round still ends with the
 * live set dense at the front and whole grains released - in whole-grain
 * counts, because that is the unit memory comes back in.
 */
static void
test_grain_defrag(void **state)
{
	(void)state;
	const unsigned bsize = SLAB_GRAIN_BYTES / 16;
	struct slab_policy pol = {
		.min = 0, .max = 64, .grow_step = 64,
		.shrink_usage_pct = 30, .shrink_release_pct = 50,
		.shrink_after = 0,
	};
	struct app a;
	struct slab *vm;
	u32 id, g;

	app_init(&a, bsize, &pol);
	vm = slab_compact_slab(&a.sc);
	g = slab_grain(vm);
	assert_int_equal(g, 16);
	assert_int_equal(slab_grow(vm, 64), 64);
	app_fill(&a, 64, 7);

	/* keep two stragglers per grain: indices 3, 7, 19, 23, 35, 39, 51, 55 */
	for (id = 0; id < 64; id++) {
		u32 ix = slab_index(vm, a.tab[id]);
		if ((ix % 16) != 3 && (ix % 16) != 7)
			app_free(&a, id);
	}
	assert_int_equal(slab_used(vm), 8);
	assert_int_equal(slab_shrink(vm, 64), 0);     /* every grain pinned */

	/* round 1: pack the eight into the first half-grain, release 50% of
	 * the 56 free blocks rounded to whole grains: 32 blocks, two grains */
	assert_int_equal(slab_compact_tick(&a.sc, 100, 0), -32);
	assert_int_equal(slab_committed(vm), 32);
	assert_int_equal(a.sc.stat.blocks_released % g, 0);
	app_verify(&a, 7);
	for (id = 0; id < 64; id++)
		if (a.tab[id])
			assert_true(slab_index(vm, a.tab[id]) < 8);

	/* the rounds converge grain by grain, exactly like the plain gc */
	assert_int_equal(slab_compact_tick(&a.sc, 200, 0), -16);
	assert_int_equal(slab_committed(vm), 16);
	/* eight live in a sixteen-block grain: nothing left to release */
	assert_int_equal(slab_compact_tick(&a.sc, 300, 0), 0);
	assert_int_equal(slab_committed(vm), 16);
	assert_slab_consistent(vm);
	app_verify(&a, 7);
	slab_compact_fini(&a.sc);
}

/* ---- the real use case: an event loop over a session table ------------------- */

/*
 * A session table living its life: sessions arrive until the slab grows to
 * its peak, most of them end, the loop keeps ticking with a small budget, and
 * the slab defragments and returns memory while the survivors keep being
 * read and written between every tick. This is the loop a daemon runs; the
 * assertions are the ones its correctness rests on - the table never points
 * at stale memory, every object's bytes survive every move, and committed
 * memory really falls from its peak.
 */
static void
test_event_loop_churn(void **state)
{
	(void)state;
	const unsigned bsize = SLAB_GRAIN_BYTES / 16;
	struct slab_policy pol = {
		.min = 16, .max = 128, .grow_step = 16,
		.grow_usage_pct = 75,
		.shrink_usage_pct = 40, .shrink_release_pct = 50,
		.shrink_after = 100,
	};
	struct app a;
	struct slab *vm;
	timestamp_t t = 0;
	u32 seed = 0x5eed, peak = 0, id, i;
	int settle;

	app_init(&a, bsize, &pol);
	vm = slab_compact_slab(&a.sc);

	/* ramp: sessions arrive; the policy grows the slab under the loop */
	for (i = 0; i < 600; i++) {
		seed = seed * 1664525u + 1013904223u;
		id = (seed >> 16) % APP_SLOTS;
		if (!a.tab[id])
			app_alloc(&a, id, 8);        /* NULL at max is fine */
		if ((i & 3) == 0) {
			t += 5;
			slab_compact_tick(&a.sc, t, 8);
		}
		if (slab_committed(vm) > peak)
			peak = slab_committed(vm);
	}
	assert_true(peak > slab_policy_min(vm));
	app_verify(&a, 8);

	/* drain: all but the long-lived sessions end, the loop keeps ticking
	 * with a small budget, and every object is re-verified between ticks
	 * while the compactor is moving its neighbours */
	for (id = 0; id < APP_SLOTS; id++) {
		if (a.tab[id] && (id % 13) != 0)
			app_free(&a, id);
		if ((id & 7) == 0) {
			t += 20;
			slab_compact_tick(&a.sc, t, 4);
			assert_slab_consistent(vm);
			app_verify(&a, 8);
		}
	}

	/* settle: tick until the policy has nothing left to do */
	for (settle = 0; settle < 64; settle++) {
		t += 100;
		if (!slab_compact_tick(&a.sc, t, 4) && !slab_compacting(&a.sc)
		    && settle > 8)
			break;
		assert_slab_consistent(vm);
		app_verify(&a, 8);
	}

	/* the point of it all: memory came back while every pointer held */
	assert_true(slab_committed(vm) < peak);
	assert_int_equal(slab_committed(vm) % slab_grain(vm), 0);
	assert_true(a.sc.stat.rounds >= 1);
	assert_true(a.sc.stat.moves >= 1);
	assert_int_equal(a.relocations, a.sc.stat.moves);
	assert_int_equal(a.mismatches, 0);
	app_verify(&a, 8);

	/* and the slab is still a slab: new sessions land and read back */
	for (id = 0; id < APP_SLOTS && slab_avail(vm); id++)
		if (!a.tab[id])
			assert_non_null(app_alloc(&a, id, 9));
	for (id = 0; id < APP_SLOTS; id++)
		if (a.tab[id])
			assert_true(check_obj(a.tab[id], id,
					      a.tab[id]->gen));
	assert_slab_consistent(vm);
	slab_compact_fini(&a.sc);
}

/* ---- nothing to do behaves like the plain gc --------------------------------- */

static void
test_packed_slab_needs_no_moves(void **state)
{
	(void)state;
	struct slab_policy pol = {
		.min = 0, .max = 8, .grow_step = 8,
		.shrink_usage_pct = 30, .shrink_release_pct = 50,
		.shrink_after = 0,
	};
	struct app a;
	u32 id;

	app_init(&a, SLAB_GRAIN_BYTES, &pol);
	assert_int_equal(slab_grow(&a.sc.slab, 8), 8);
	app_fill(&a, 8, 10);

	/* free the tail: live blocks already dense at the front */
	for (id = 0; id < APP_SLOTS; id++)
		if (a.tab[id] && slab_index(&a.sc.slab, a.tab[id]) >= 2)
			app_free(&a, id);
	assert_true(slab_compact_packed(&a.sc));

	/* the tick releases what slab_gc would - 50% of 6 free - moving
	 * nothing and announcing nothing */
	assert_int_equal(slab_compact_tick(&a.sc, 0, 0), -3);
	assert_int_equal(slab_committed(&a.sc.slab), 5);
	assert_int_equal(a.sc.stat.moves, 0);
	assert_int_equal(a.relocations, 0);
	assert_slab_consistent(&a.sc.slab);
	app_verify(&a, 10);
	slab_compact_fini(&a.sc);
}

int
main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_bulk_move_single_copy),
		cmocka_unit_test(test_tick_defrags_then_releases),
		cmocka_unit_test(test_budget_slices_the_round),
		cmocka_unit_test(test_pressure_cancels_the_round),
#if SLAB_VM_RELEASES
		cmocka_unit_test(test_release_returns_pages_to_the_os),
#endif
		cmocka_unit_test(test_grain_defrag),
		cmocka_unit_test(test_event_loop_churn),
		cmocka_unit_test(test_packed_slab_needs_no_moves),
	};
	return cmocka_run_group_tests_name("slab_compact", tests, NULL, NULL);
}
