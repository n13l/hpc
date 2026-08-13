/*
 * Unit tests for the page-run memory pool (<mem/mempool.h>): runs of whole
 * grains out of one reservation, grown in place or by moving, shrunk in place,
 * and released to the host under the slab-shaped policy.
 *
 * The pool's grain is a page, so the counts below are in pages; where a test
 * needs to see memory really leave the process it asks mincore(), the way the
 * slab tests do.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h> /* cmocka.h needs it first */
#include <cmocka.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>

#include <hpc/compiler.h>
#include <mem/mempool.h>

#define PG ((size_t)CPU_PAGE_SIZE)

static u32
resident_pages(void *base, size_t len)
{
	size_t n = (len + PG - 1) / PG, i;
	unsigned char *vec = malloc(n);
	u32 r = 0;

	assert_non_null(vec);
	assert_int_equal(mincore(base, len, vec), 0);
	for (i = 0; i < n; i++)
		r += vec[i] & 1;
	free(vec);
	return r;
}

/* ---- sizing ---------------------------------------------------------------- */

static void
test_init_rounds_to_grains(void **state)
{
	(void)state;
	struct mempool_policy pol = MEMPOOL_POLICY_STATIC(10 * PG + 1);
	struct mempool p;

	assert_int_equal(mempool_init(&p, &pol), 0);
	assert_int_equal(mempool_grain_bytes(&p), PG);
	assert_int_equal(mempool_reserved_bytes(&p), 11 * PG);
	assert_int_equal(p.policy.max, 11 * PG);
	assert_int_equal(p.policy.min, 11 * PG);          /* static: floor = max */
	assert_int_equal(mempool_size(&p, 1), PG);
	assert_int_equal(mempool_size(&p, PG), PG);
	assert_int_equal(mempool_size(&p, PG + 1), 2 * PG);
	assert_int_equal(mempool_used_bytes(&p), 0);
	assert_int_equal(mempool_resident_bytes(&p), 0);
	mempool_fini(&p);
}

static void
test_init_refuses_bad_grain(void **state)
{
	(void)state;
	struct mempool_policy pol = MEMPOOL_POLICY_STATIC(16 * PG);
	struct mempool p;

	assert_int_equal(mempool_init_grain(&p, 3 * PG, &pol), -1);   /* not pow2 */
	assert_int_equal(mempool_init_grain(&p, PG / 2, &pol), -1);   /* sub-page */
	assert_int_equal(mempool_init_grain(&p, 4 * PG, &pol), 0);
	assert_int_equal(mempool_grain_bytes(&p), 4 * PG);
	assert_int_equal(mempool_size(&p, 1), 4 * PG);
	mempool_fini(&p);
}

/* ---- alloc / free ---------------------------------------------------------- */

static void
test_alloc_is_contiguous_and_aligned(void **state)
{
	(void)state;
	struct mempool_policy pol = MEMPOOL_POLICY_STATIC(16 * PG);
	struct mempool p;
	u8 *a, *b;

	assert_int_equal(mempool_init(&p, &pol), 0);
	a = mempool_alloc(&p, 3 * PG - 100);
	b = mempool_alloc(&p, 1);
	assert_non_null(a);
	assert_non_null(b);
	assert_true(((uintptr_t)a & (PG - 1)) == 0);
	assert_ptr_equal(b, a + 3 * PG);                  /* next-fit: right after */
	assert_int_equal(p.used, 4);
	assert_int_equal(mempool_used_bytes(&p), 4 * PG);
	/* every byte of the rounded run is the owner's */
	memset(a, 0xa5, 3 * PG);
	memset(b, 0x5a, PG);
	assert_int_equal(a[3 * PG - 1], 0xa5);
	assert_int_equal(b[0], 0x5a);

	mempool_free(&p, a, 3 * PG - 100);
	assert_int_equal(p.used, 1);
	assert_int_equal(p.idle, 3);
	assert_int_equal(mempool_resident_bytes(&p), 4 * PG);
	mempool_free(&p, b, 1);
	assert_int_equal(p.used, 0);
	assert_int_equal(p.idle, 4);
	mempool_fini(&p);
}

static void
test_alloc_fails_past_the_reservation(void **state)
{
	(void)state;
	struct mempool_policy pol = MEMPOOL_POLICY_STATIC(8 * PG);
	struct mempool p;
	void *a, *b, *c;

	assert_int_equal(mempool_init(&p, &pol), 0);
	a = mempool_alloc(&p, 5 * PG);
	assert_non_null(a);
	assert_null(mempool_alloc(&p, 4 * PG));           /* 3 left, not 4 */
	assert_null(mempool_alloc(&p, 9 * PG));           /* larger than all */
	assert_null(mempool_alloc(&p, 0));
	b = mempool_alloc(&p, 3 * PG);
	assert_non_null(b);
	assert_null(mempool_alloc(&p, 1));                /* full */
	mempool_free(&p, a, 5 * PG);
	c = mempool_alloc(&p, 5 * PG);                    /* the hole is reused */
	assert_ptr_equal(c, a);
	mempool_fini(&p);
}

/* A freed hole in the middle is found once the cursor wraps. */
static void
test_alloc_wraps_to_a_hole(void **state)
{
	(void)state;
	struct mempool_policy pol = MEMPOOL_POLICY_STATIC(8 * PG);
	struct mempool p;
	u8 *r[8];
	int i;

	assert_int_equal(mempool_init(&p, &pol), 0);
	for (i = 0; i < 8; i++) {
		r[i] = mempool_alloc(&p, PG);
		assert_non_null(r[i]);
	}
	mempool_free(&p, r[2], PG);
	mempool_free(&p, r[3], PG);
	assert_null(mempool_alloc(&p, 3 * PG));
	assert_ptr_equal(mempool_alloc(&p, 2 * PG), r[2]);
	mempool_fini(&p);
}

/* ---- grow / shrink --------------------------------------------------------- */

static void
test_grow_in_place_when_the_tail_is_free(void **state)
{
	(void)state;
	struct mempool_policy pol = MEMPOOL_POLICY_STATIC(16 * PG);
	struct mempool p;
	u8 *a, *b;

	assert_int_equal(mempool_init(&p, &pol), 0);
	a = mempool_alloc(&p, PG);
	assert_non_null(a);
	memset(a, 0x11, PG);
	b = mempool_grow(&p, a, PG, 4 * PG);
	assert_ptr_equal(b, a);                           /* extended, not moved */
	assert_int_equal(p.used, 4);
	assert_int_equal(b[PG - 1], 0x11);
	/* the grains taken by the grow are live: the next alloc lands after */
	assert_ptr_equal(mempool_alloc(&p, PG), a + 4 * PG);
	mempool_fini(&p);
}

static void
test_grow_moves_when_blocked(void **state)
{
	(void)state;
	struct mempool_policy pol = MEMPOOL_POLICY_STATIC(16 * PG);
	struct mempool p;
	u8 *a, *b, *c;
	size_t i;

	assert_int_equal(mempool_init(&p, &pol), 0);
	a = mempool_alloc(&p, 2 * PG);
	b = mempool_alloc(&p, PG);                        /* sits right after a */
	assert_non_null(a);
	assert_non_null(b);
	for (i = 0; i < 2 * PG; i++)
		a[i] = (u8)i;
	c = mempool_grow(&p, a, 2 * PG, 3 * PG);
	assert_non_null(c);
	assert_ptr_not_equal(c, a);                       /* had to move */
	assert_ptr_equal(c, b + PG);                      /* to the next free run */
	for (i = 0; i < 2 * PG; i++)                      /* the bytes came along */
		assert_int_equal(c[i], (u8)i);
	assert_int_equal(p.used, 4);                      /* 3 + b */
	assert_int_equal(p.idle, 2);                      /* the old a */
	/* and the old run is free for the taking */
	assert_ptr_equal(mempool_alloc(&p, 2 * PG), a);
	mempool_fini(&p);
}

static void
test_grow_fails_intact(void **state)
{
	(void)state;
	struct mempool_policy pol = MEMPOOL_POLICY_STATIC(4 * PG);
	struct mempool p;
	u8 *a;

	assert_int_equal(mempool_init(&p, &pol), 0);
	a = mempool_alloc(&p, 3 * PG);
	assert_non_null(a);
	memset(a, 0x77, 3 * PG);
	assert_null(mempool_grow(&p, a, 3 * PG, 5 * PG));  /* beyond the pool */
	assert_int_equal(p.used, 3);                      /* nothing changed */
	assert_int_equal(a[3 * PG - 1], 0x77);
	mempool_fini(&p);
}

static void
test_grow_from_nothing_allocates(void **state)
{
	(void)state;
	struct mempool_policy pol = MEMPOOL_POLICY_STATIC(4 * PG);
	struct mempool p;
	u8 *a;

	assert_int_equal(mempool_init(&p, &pol), 0);
	a = mempool_grow(&p, NULL, 0, 2 * PG);
	assert_non_null(a);
	assert_int_equal(p.used, 2);
	/* a grow to no more grains is a no-op that keeps the pointer */
	assert_ptr_equal(mempool_grow(&p, a, 2 * PG, 2 * PG - 10), a);
	assert_int_equal(p.used, 2);
	mempool_fini(&p);
}

static void
test_shrink_in_place(void **state)
{
	(void)state;
	struct mempool_policy pol = MEMPOOL_POLICY_STATIC(8 * PG);
	struct mempool p;
	u8 *a;

	assert_int_equal(mempool_init(&p, &pol), 0);
	a = mempool_alloc(&p, 6 * PG);
	assert_non_null(a);
	memset(a, 0x42, 6 * PG);
	assert_ptr_equal(mempool_shrink(&p, a, 6 * PG, 2 * PG), a);
	assert_int_equal(p.used, 2);
	assert_int_equal(p.idle, 4);
	assert_int_equal(a[2 * PG - 1], 0x42);            /* kept bytes untouched */
	/* the cut tail is free again */
	assert_ptr_equal(mempool_alloc(&p, 4 * PG), a + 2 * PG);
	/* shrinking to nothing frees and says so */
	assert_null(mempool_shrink(&p, a, 2 * PG, 0));
	assert_int_equal(p.used, 4);
	/* a grow with a smaller size is a shrink */
	mempool_fini(&p);
}

/* grow-shrink-grow on one buffer: bitmap work only, the address never moves */
static void
test_grow_shrink_cycle_stays_put(void **state)
{
	(void)state;
	struct mempool_policy pol = MEMPOOL_POLICY_STATIC(64 * PG);
	struct mempool p;
	u8 *a, *b;
	int i;

	assert_int_equal(mempool_init(&p, &pol), 0);
	a = mempool_alloc(&p, PG);
	assert_non_null(a);
	for (i = 0; i < 100; i++) {
		b = mempool_grow(&p, a, PG, 16 * PG);
		assert_ptr_equal(b, a);
		assert_ptr_equal(mempool_shrink(&p, a, 16 * PG, PG), a);
	}
	assert_int_equal(p.used, 1);
	assert_int_equal(p.idle, 15);
	assert_int_equal(p.high, 16);
	mempool_fini(&p);
}

/* ---- gc / release policy --------------------------------------------------- */

static void
test_static_never_releases(void **state)
{
	(void)state;
	struct mempool_policy pol = MEMPOOL_POLICY_STATIC(8 * PG);
	struct mempool p;
	void *a;

	assert_int_equal(mempool_init(&p, &pol), 0);
	a = mempool_alloc(&p, 8 * PG);
	assert_non_null(a);
	memset(a, 1, 8 * PG);
	mempool_free(&p, a, 8 * PG);
	assert_int_equal(mempool_gc(&p, 0), 0);
	assert_int_equal(mempool_gc(&p, 1000000), 0);
	assert_int_equal(p.idle, 8);
	assert_int_equal(resident_pages(p.base, 8 * PG), 8);
	mempool_fini(&p);
}

static void
test_eager_releases_idle_after_dwell(void **state)
{
	(void)state;
	struct mempool_policy pol = MEMPOOL_POLICY_EAGER(0, 8 * PG, 100);
	struct mempool p;
	u8 *live, *gone;

	assert_int_equal(mempool_init(&p, &pol), 0);
	live = mempool_alloc(&p, PG);
	gone = mempool_alloc(&p, 7 * PG);
	assert_non_null(live);
	assert_non_null(gone);
	memset(live, 0x33, PG);
	memset(gone, 0x44, 7 * PG);
	assert_int_equal(resident_pages(p.base, 8 * PG), 8);

	mempool_free(&p, gone, 7 * PG);
	/* 1 of 8 resident is live: at the watermark, but the dwell has to pass */
	assert_int_equal(mempool_gc(&p, 1000), 0);
	assert_int_equal(mempool_gc(&p, 1050), 0);
	assert_int_equal(p.idle, 7);
	/* dwell served: eager returns every idle grain at once */
	assert_int_equal(mempool_gc(&p, 1100), -7);
	assert_int_equal(p.idle, 0);
	assert_int_equal(p.used, 1);
	assert_int_equal(p.high, 1);                      /* the tail is untouched */
	assert_int_equal(mempool_resident_bytes(&p), PG);
	/* gone from RSS, not just from the accounting; the survivor intact */
	assert_int_equal(resident_pages(gone, 7 * PG), 0);
	assert_int_equal(live[PG - 1], 0x33);
	/* nothing idle left: a pass is a no-op */
	assert_int_equal(mempool_gc(&p, 2000), 0);
	mempool_fini(&p);
}

static void
test_gradual_releases_half_per_interval(void **state)
{
	(void)state;
	struct mempool_policy pol = MEMPOOL_POLICY_GRADUAL(0, 16 * PG, 100);
	struct mempool p;
	u8 *a;

	assert_int_equal(mempool_init(&p, &pol), 0);
	a = mempool_alloc(&p, 16 * PG);
	assert_non_null(a);
	memset(a, 1, 16 * PG);
	/* keep one page, idle fifteen: usage 6% */
	assert_ptr_equal(mempool_shrink(&p, a, 16 * PG, PG), a);
	assert_int_equal(mempool_gc(&p, 0), 0);           /* arms the timer */
	assert_int_equal(mempool_gc(&p, 100), -8);        /* half of 15, rounded up */
	assert_int_equal(p.idle, 7);
	assert_int_equal(mempool_gc(&p, 150), 0);         /* rate limited */
	assert_int_equal(mempool_gc(&p, 200), -4);        /* half of 7: 1 of 4 = 25% */
	assert_int_equal(mempool_gc(&p, 300), -2);        /* half of 3: 1 of 2 = 50% */
	/* releasing raised usage past the watermark: the round is over, by
	 * design - what is left is a working set, not waste */
	assert_int_equal(mempool_gc(&p, 400), 0);
	assert_false(p.armed);
	assert_int_equal(p.idle, 1);
	assert_int_equal(p.high, 2);
	/* the highest pages went first: the live prefix is what stayed resident */
	assert_int_equal(resident_pages(a, PG), 1);
	assert_int_equal(resident_pages(a + 2 * PG, 14 * PG), 0);
	mempool_fini(&p);
}

static void
test_release_stops_at_the_floor(void **state)
{
	(void)state;
	struct mempool_policy pol = MEMPOOL_POLICY_EAGER(6 * PG, 16 * PG, 0);
	struct mempool p;
	u8 *a;

	assert_int_equal(mempool_init(&p, &pol), 0);
	a = mempool_alloc(&p, 16 * PG);
	assert_non_null(a);
	memset(a, 1, 16 * PG);
	assert_ptr_equal(mempool_shrink(&p, a, 16 * PG, PG), a);
	/* 1 live + 15 idle; the floor keeps 6 resident: 10 may go */
	assert_int_equal(mempool_gc(&p, 0), -10);
	assert_int_equal(p.used + p.idle, 6);
	assert_int_equal(mempool_gc(&p, 1), 0);           /* at the floor */
	mempool_fini(&p);
}

static void
test_usage_above_watermark_disarms(void **state)
{
	(void)state;
	struct mempool_policy pol = MEMPOOL_POLICY_EAGER(0, 8 * PG, 100);
	struct mempool p;
	u8 *a, *b;

	assert_int_equal(mempool_init(&p, &pol), 0);
	a = mempool_alloc(&p, 4 * PG);
	b = mempool_alloc(&p, 4 * PG);
	assert_non_null(a);
	assert_non_null(b);
	mempool_free(&p, b, 4 * PG);                      /* 50% usage: no */
	assert_int_equal(mempool_gc(&p, 0), 0);
	assert_int_equal(mempool_gc(&p, 1000), 0);
	assert_false(p.armed);
	mempool_free(&p, a, 4 * PG);                      /* 0%: arm */
	assert_int_equal(mempool_gc(&p, 2000), 0);
	assert_true(p.armed);
	/* demand comes back before the dwell: timer cancelled, nothing released */
	a = mempool_alloc(&p, 8 * PG);
	assert_non_null(a);
	assert_int_equal(mempool_gc(&p, 2100), 0);
	assert_false(p.armed);
	assert_int_equal(p.idle, 0);
	mempool_fini(&p);
}

static bool
veto(struct mempool *p, void *arg)
{
	(void)p;
	return *(int *)arg != 0;
}

static void
test_check_gate_vetoes(void **state)
{
	(void)state;
	int allow = 0;
	struct mempool_policy pol = MEMPOOL_POLICY_EAGER(0, 8 * PG, 0);
	struct mempool p;
	void *a;

	pol.check = veto;
	pol.arg = &allow;
	assert_int_equal(mempool_init(&p, &pol), 0);
	a = mempool_alloc(&p, 8 * PG);
	assert_non_null(a);
	mempool_free(&p, a, 8 * PG);
	assert_int_equal(mempool_gc(&p, 0), 0);           /* vetoed */
	assert_int_equal(p.idle, 8);
	allow = 1;
	assert_int_equal(mempool_gc(&p, 1), -8);
	mempool_fini(&p);
}

/* a released grain reads as zero again when it is next handed out */
static void
test_released_grains_come_back_usable(void **state)
{
	(void)state;
	struct mempool_policy pol = MEMPOOL_POLICY_EAGER(0, 8 * PG, 0);
	struct mempool p;
	u8 *a;

	assert_int_equal(mempool_init(&p, &pol), 0);
	a = mempool_alloc(&p, 8 * PG);
	assert_non_null(a);
	memset(a, 0xee, 8 * PG);
	mempool_free(&p, a, 8 * PG);
	assert_int_equal(mempool_gc(&p, 0), -8);
	a = mempool_alloc(&p, 8 * PG);
	assert_non_null(a);
#if SLAB_VM_RELEASES
	assert_int_equal(a[0], 0);
	assert_int_equal(a[8 * PG - 1], 0);
#endif
	memset(a, 0x01, 8 * PG);
	assert_int_equal(a[8 * PG - 1], 0x01);
	mempool_fini(&p);
}

static void
test_set_policy_keeps_the_reservation(void **state)
{
	(void)state;
	struct mempool_policy pol = MEMPOOL_POLICY_STATIC(8 * PG);
	struct mempool_policy eager = MEMPOOL_POLICY_EAGER(2 * PG, 1, 0);
	struct mempool p;
	void *a;

	assert_int_equal(mempool_init(&p, &pol), 0);
	a = mempool_alloc(&p, 8 * PG);
	assert_non_null(a);
	mempool_free(&p, a, 8 * PG);
	assert_int_equal(mempool_gc(&p, 0), 0);
	mempool_set_policy(&p, &eager);
	assert_int_equal(p.policy.max, 8 * PG);           /* max is the mapping */
	assert_int_equal(p.policy.min, 2 * PG);
	assert_int_equal(mempool_gc(&p, 0), -6);          /* down to the floor */
	mempool_fini(&p);
}

static void
test_policy_preset_by_name(void **state)
{
	(void)state;
	struct mempool_policy p;

	assert_int_equal(mempool_policy_preset(&p, "static", 0, 8 * PG, 5), 0);
	assert_int_equal(p.shrink_release_pct, 0);
	assert_int_equal(p.min, 8 * PG);
	assert_int_equal(mempool_policy_preset(&p, "gradual", PG, 8 * PG, 5), 0);
	assert_int_equal(p.shrink_release_pct, 50);
	assert_int_equal(p.shrink_after, 5);
	assert_int_equal(mempool_policy_preset(&p, "eager", PG, 8 * PG, 5), 0);
	assert_int_equal(p.shrink_release_pct, 100);
	assert_int_equal(mempool_policy_preset(&p, "bogus", 0, 8 * PG, 5), -1);
	assert_int_equal(p.shrink_release_pct, 100);      /* untouched */
	assert_int_equal(mempool_policy_preset(&p, NULL, 0, 8 * PG, 5), 0);
	assert_int_equal(mempool_policy_preset(&p, "", 0, 8 * PG, 5), 0);
}

/* ---- the use case ---------------------------------------------------------- */

/*
 * A dynamic table of the HPACK kind: an entry ring followed by a byte arena,
 * addressed by offset, grown when the peer raises its limit. The owner keeps
 * two pointers and re-derives both after every grow.
 */
struct table {
	u8 *mem;
	size_t mem_len;
	u32 *ent;
	u8 *buf;
	u32 cap;
};

static void
table_layout(struct table *t)
{
	t->ent = (u32 *)t->mem;
	t->buf = t->mem + 64 * sizeof(u32);
}

static void
test_offset_addressed_table_grows(void **state)
{
	(void)state;
	struct mempool_policy pol = MEMPOOL_POLICY_GRADUAL(0, 256 * PG, 0);
	struct mempool p;
	struct table t;
	u8 *other;
	u32 i;

	assert_int_equal(mempool_init(&p, &pol), 0);
	t.cap = 4096;
	t.mem_len = 64 * sizeof(u32) + t.cap;
	t.mem = mempool_alloc(&p, t.mem_len);
	assert_non_null(t.mem);
	table_layout(&t);
	for (i = 0; i < 64; i++) {
		t.ent[i] = i * 64;
		memset(t.buf + i * 64, (int)i, 64);
	}
	/* something else lands right after, so the grow has to move */
	other = mempool_alloc(&p, PG);
	assert_non_null(other);

	t.cap = 65536;
	t.mem = mempool_grow(&p, t.mem, t.mem_len, 64 * sizeof(u32) + t.cap);
	assert_non_null(t.mem);
	t.mem_len = 64 * sizeof(u32) + t.cap;
	table_layout(&t);
	for (i = 0; i < 64; i++) {
		assert_int_equal(t.ent[i], i * 64);
		assert_int_equal(t.buf[t.ent[i]], (u8)i);
		assert_int_equal(t.buf[t.ent[i] + 63], (u8)i);
	}
	/* the peer lowers it again: the tail goes back, the bytes stay */
	t.cap = 4096;
	assert_ptr_equal(mempool_shrink(&p, t.mem, t.mem_len,
	                                64 * sizeof(u32) + t.cap), t.mem);
	t.mem_len = 64 * sizeof(u32) + t.cap;
	assert_int_equal(t.buf[t.ent[63]], 63);
	assert_true(mempool_gc(&p, 0) < 0);               /* dwell 0: releases */
	mempool_free(&p, t.mem, t.mem_len);
	mempool_free(&p, other, PG);
	mempool_fini(&p);
}

int
main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_init_rounds_to_grains),
		cmocka_unit_test(test_init_refuses_bad_grain),
		cmocka_unit_test(test_alloc_is_contiguous_and_aligned),
		cmocka_unit_test(test_alloc_fails_past_the_reservation),
		cmocka_unit_test(test_alloc_wraps_to_a_hole),
		cmocka_unit_test(test_grow_in_place_when_the_tail_is_free),
		cmocka_unit_test(test_grow_moves_when_blocked),
		cmocka_unit_test(test_grow_fails_intact),
		cmocka_unit_test(test_grow_from_nothing_allocates),
		cmocka_unit_test(test_shrink_in_place),
		cmocka_unit_test(test_grow_shrink_cycle_stays_put),
		cmocka_unit_test(test_static_never_releases),
		cmocka_unit_test(test_eager_releases_idle_after_dwell),
		cmocka_unit_test(test_gradual_releases_half_per_interval),
		cmocka_unit_test(test_release_stops_at_the_floor),
		cmocka_unit_test(test_usage_above_watermark_disarms),
		cmocka_unit_test(test_check_gate_vetoes),
		cmocka_unit_test(test_released_grains_come_back_usable),
		cmocka_unit_test(test_set_policy_keeps_the_reservation),
		cmocka_unit_test(test_policy_preset_by_name),
		cmocka_unit_test(test_offset_addressed_table_grows),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
