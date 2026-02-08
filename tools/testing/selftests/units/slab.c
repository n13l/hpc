/*
 * Unit tests for the slab allocator: runtime <mem/slab.h> and the build-time
 * <mem/slab_class.h> variant, including the TCP-reordering use case.
 *
 * The policy units below - watermarks, the idle dwell, the check() gate, the hot
 * swap - use a block of exactly one grain on purpose. Memory is grown and released
 * in whole grains (see <mem/slab_vm.h>), and a grain-sized block puts one block in
 * a grain, so a step of n blocks is a step of n and the arithmetic in these
 * assertions is the policy's, with no rounding folded into it - under any grain,
 * including the 2 MB one CONFIG_MEM_HUGEPAGE selects. The grain itself, and what it
 * does to a smaller block, is what the test_grain_* units cover.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>
#include <string.h>

#include <hpc/compiler.h>

/* Build-time block size for the static variant: 2048B holds a 1500B MTU. */
#define SLAB_CLASS_BLOCK_SIZE 2048

#include <mem/slab.h>
#include <mem/slab_class.h>

/* ---- block-size selection ------------------------------------------------ */

static void
test_shift_for(void **state)
{
	(void)state;
	/* MTU-driven sizes round up to the next power of two. */
	assert_int_equal(slab_shift_for(SLAB_MTU_ETHERNET), 11); /* 1500 -> 2048 */
	assert_int_equal(slab_shift_for(SLAB_MTU_JUMBO), 14);    /* 9000 -> 16384 */
	assert_int_equal(slab_shift_for(2048), 11);              /* exact pow2   */
	assert_int_equal(slab_shift_for(2049), 12);              /* -> 4096      */
	assert_int_equal(slab_shift_for(1), 2);                  /* clamped to 4 */
}

static void
test_block_size(void **state)
{
	(void)state;
	struct slab_policy pol = { .min = 2, .max = 8 };
	struct slab vm;

	assert_int_equal(slab_init(&vm, SLAB_MTU_ETHERNET, &pol), 0);
	assert_int_equal(slab_block_size(&vm), 2048);
	/* the committed set is the rounded min, whatever the grain rounded it to */
	assert_int_equal(slab_committed(&vm), slab_policy_min(&vm));
	assert_int_equal(slab_avail(&vm), slab_committed(&vm));
	assert_int_equal(slab_used(&vm), 0);
	slab_fini(&vm);
}

/* ---- allocation / free --------------------------------------------------- */

static void
test_alloc_free_lifo(void **state)
{
	(void)state;
	struct slab_policy pol = { .min = 4, .max = 4 };
	struct slab vm;
	void *a, *b, *c;

	assert_int_equal(slab_init(&vm, 512, &pol), 0);

	a = slab_alloc(&vm);
	b = slab_alloc(&vm);
	assert_non_null(a);
	assert_non_null(b);
	assert_ptr_not_equal(a, b);
	assert_int_equal(slab_used(&vm), 2);

	/* whole block is usable payload while allocated */
	memset(a, 0xaa, slab_block_size(&vm));
	memset(b, 0x55, slab_block_size(&vm));

	slab_free(&vm, b);
	assert_int_equal(slab_used(&vm), 1);

	/* LIFO free list hands the most recently freed block back first */
	c = slab_alloc(&vm);
	assert_ptr_equal(c, b);
	assert_int_equal(slab_used(&vm), 2);

	slab_fini(&vm);
}

static void
test_exhaustion_grows_to_max(void **state)
{
	(void)state;
	struct slab_policy pol = { .min = 2, .max = 4, .grow_step = 2 };
	struct slab vm;
	void *p[4];

	assert_int_equal(slab_init(&vm, SLAB_GRAIN_BYTES, &pol), 0);

	/* first two come from the initial commit */
	p[0] = slab_alloc(&vm);
	p[1] = slab_alloc(&vm);
	assert_int_equal(slab_committed(&vm), 2);

	/* third exhausts the free list and auto-grows a step up to max */
	p[2] = slab_alloc(&vm);
	assert_non_null(p[2]);
	assert_int_equal(slab_committed(&vm), 4);

	p[3] = slab_alloc(&vm);
	assert_non_null(p[3]);
	assert_int_equal(slab_used(&vm), 4);

	/* at max: the next allocation must fail */
	assert_null(slab_alloc(&vm));

	slab_fini(&vm);
}

/* ---- check() gate -------------------------------------------------------- */

static bool
deny_all(struct slab *vm, int grow, void *arg)
{
	(void)vm; (void)grow; (void)arg;
	return false;
}

static void
test_check_vetoes_grow(void **state)
{
	(void)state;
	struct slab_policy pol = {
		.min = 2, .max = 16, .grow_step = 4, .check = deny_all,
	};
	struct slab vm;

	assert_int_equal(slab_init(&vm, SLAB_GRAIN_BYTES, &pol), 0);
	assert_non_null(slab_alloc(&vm));
	assert_non_null(slab_alloc(&vm));

	/* free list empty; check() denies the grow, so allocation fails */
	assert_null(slab_alloc(&vm));
	assert_int_equal(slab_committed(&vm), 2);
	slab_fini(&vm);
}

/* ---- policy-driven grow / shrink via slab_gc ----------------------------- */

static void
test_gc_grow_and_shrink(void **state)
{
	(void)state;
	struct slab_policy pol = {
		.min = 2, .max = 16, .grow_step = 2,
		.grow_usage_pct = 50, .shrink_usage_pct = 25,
		.shrink_release_pct = 50,
	};
	struct slab vm;
	void *p[8];
	int i;

	assert_int_equal(slab_init(&vm, SLAB_GRAIN_BYTES, &pol), 0);

	/* Drain the free list so usage reaches the grow watermark. */
	p[0] = slab_alloc(&vm);
	p[1] = slab_alloc(&vm);
	assert_int_equal(slab_avail(&vm), 0);

	/* used==100% >= 50% -> gc grows one step (shrink_after==0 -> immediate) */
	assert_int_equal(slab_gc(&vm, 0), 2);
	assert_int_equal(slab_committed(&vm), 4);

	/* Grab everything, then free it all so usage hits the shrink mark. */
	for (i = 2; i < 4; i++)
		p[i] = slab_alloc(&vm);
	for (i = 0; i < 4; i++)
		slab_free(&vm, p[i]);
	assert_int_equal(slab_used(&vm), 0);
	assert_int_equal(slab_avail(&vm), 4);

	/* used==0% <= 25% -> release 50% of the 4 free blocks, but not below min */
	assert_int_equal(slab_gc(&vm, 0), -2);
	assert_int_equal(slab_committed(&vm), 2);
	assert_int_equal(slab_gc(&vm, 0), 0);         /* at min, no-op */
	assert_int_equal(slab_committed(&vm), 2);
	slab_fini(&vm);
}

/*
 * Time-based shrink: once usage has stayed at/under shrink_usage_pct for
 * shrink_after, gc releases shrink_release_pct of the free blocks, then
 * re-arms - so half the then-free memory is returned per interval (8 -> 4 -> 2
 * free blocks released as committed falls), a gradual return to the OS.
 */
static void
test_idle_shrink_after_dwell(void **state)
{
	(void)state;
	struct slab_policy pol = {
		.min = 0, .max = 8, .grow_step = 8,
		.grow_usage_pct = 75, .shrink_usage_pct = 30,
		.shrink_release_pct = 50,
		.shrink_after = 1000,          /* 1s idle dwell */
	};
	struct slab vm;
	void *p[8];
	timestamp_t t = 10000;
	int i;

	assert_int_equal(slab_init(&vm, SLAB_GRAIN_BYTES, &pol), 0);
	for (i = 0; i < 8; i++)
		p[i] = slab_alloc(&vm);
	assert_int_equal(slab_committed(&vm), 8);

	for (i = 0; i < 8; i++)
		slab_free(&vm, p[i]);
	assert_int_equal(slab_used(&vm), 0);

	/* First gc arms the timer but releases nothing. */
	assert_int_equal(slab_gc(&vm, t), 0);
	assert_int_equal(slab_committed(&vm), 8);

	/* Still inside the dwell window -> nothing released. */
	assert_int_equal(slab_gc(&vm, t + 999), 0);
	assert_int_equal(slab_committed(&vm), 8);

	/* Dwell elapsed -> release 50% of the 8 free blocks and re-arm. */
	assert_int_equal(slab_gc(&vm, t + 1000), -4);
	assert_int_equal(slab_committed(&vm), 4);

	/* Rate-limited: right after a release nothing more happens. */
	assert_int_equal(slab_gc(&vm, t + 1000), 0);
	assert_int_equal(slab_committed(&vm), 4);

	/* Next interval -> release 50% of the now-4 free blocks. */
	assert_int_equal(slab_gc(&vm, t + 2000), -2);
	assert_int_equal(slab_committed(&vm), 2);
	slab_fini(&vm);
}

/*
 * Hysteresis: a burst of activity that lifts usage back above the watermark
 * cancels the pending dwell, so a transient dip never triggers a shrink and
 * the timer restarts from scratch once usage falls again.
 */
static void
test_idle_timer_resets_on_activity(void **state)
{
	(void)state;
	struct slab_policy pol = {
		.min = 0, .max = 4, .grow_step = 2,
		.grow_usage_pct = 75, .shrink_usage_pct = 30,
		.shrink_release_pct = 50,
		.shrink_after = 1000,
	};
	struct slab vm;
	void *a, *b, *c, *d;

	assert_int_equal(slab_init(&vm, SLAB_GRAIN_BYTES, &pol), 0);
	a = slab_alloc(&vm); b = slab_alloc(&vm);
	c = slab_alloc(&vm); d = slab_alloc(&vm);
	assert_int_equal(slab_committed(&vm), 4);

	slab_free(&vm, a); slab_free(&vm, b);
	slab_free(&vm, c); slab_free(&vm, d);

	/* Arm the timer at t=1000. */
	assert_int_equal(slab_gc(&vm, 1000), 0);
	assert_int_equal(vm.idle, 1);

	/* Activity lifts usage above the watermark -> timer cancelled. */
	a = slab_alloc(&vm); b = slab_alloc(&vm); c = slab_alloc(&vm);
	assert_int_equal(slab_gc(&vm, 1500), 0);
	assert_int_equal(vm.idle, 0);
	assert_int_equal(slab_committed(&vm), 4);

	/* Usage falls again: the dwell restarts from 2000, not from 1000. */
	slab_free(&vm, a); slab_free(&vm, b); slab_free(&vm, c);
	assert_int_equal(slab_gc(&vm, 2000), 0);
	assert_int_equal(slab_committed(&vm), 4);
	assert_int_equal(slab_gc(&vm, 2999), 0);      /* dwell not yet met */
	assert_int_equal(slab_committed(&vm), 4);
	assert_true(slab_gc(&vm, 3000) < 0);          /* now it releases */
	slab_fini(&vm);
}

/*
 * Change the policy and thresholds while the slab is operating and holding
 * live data, with no thread synchronization. The swap takes effect on the next
 * API call, leaves the held blocks and their contents untouched, and keeps
 * block addresses stable.
 */
static void
test_policy_hot_swap(void **state)
{
	(void)state;
	/* Start with shrinking disabled and no watermark growth. */
	struct slab_policy p1 = {
		.min = 2, .max = 16, .grow_step = 4,
		.shrink_release_pct = 0,
	};
	struct slab vm;
	void *blk[6];
	u32 i0, i1;
	int i;

	assert_int_equal(slab_init(&vm, SLAB_GRAIN_BYTES, &p1), 0);

	for (i = 0; i < 6; i++) {
		blk[i] = slab_alloc(&vm);
		assert_non_null(blk[i]);
		memset(blk[i], 0x40 + i, slab_block_size(&vm));
	}
	assert_int_equal(slab_committed(&vm), 6);

	/* Hold blk[0] and blk[1]; free the other four -> usage falls to 33%. */
	i0 = slab_index(&vm, blk[0]);
	i1 = slab_index(&vm, blk[1]);
	for (i = 2; i < 6; i++)
		slab_free(&vm, blk[i]);
	assert_int_equal(slab_used(&vm), 2);
	assert_int_equal(slab_avail(&vm), 4);

	/* Old policy has shrinking disabled: gc is a no-op even though idle. */
	assert_int_equal(slab_gc(&vm, 1000), 0);
	assert_int_equal(slab_committed(&vm), 6);

	/* Hot-swap: enable an aggressive immediate shrink (no locking). */
	{
		struct slab_policy p2 = {
			.min = 2, .max = 16, .grow_step = 4,
			.grow_usage_pct = 90, .shrink_usage_pct = 50,
			.shrink_release_pct = 50, .shrink_after = 0,
		};
		slab_set_policy(&vm, &p2);
	}

	/* Held data survived the policy change unchanged. */
	assert_int_equal(*(u8 *)blk[0], 0x40);
	assert_int_equal(*(u8 *)blk[1], 0x41);

	/* The new policy takes effect on the next gc: release 50% of 4 free. */
	assert_int_equal(slab_gc(&vm, 2000), -2);
	assert_int_equal(slab_committed(&vm), 4);
	assert_int_equal(slab_used(&vm), 2);

	/* Live blocks kept their addresses and contents across the shrink. */
	assert_ptr_equal(blk[0], slab_at(&vm, i0));
	assert_ptr_equal(blk[1], slab_at(&vm, i1));
	assert_int_equal(*(u8 *)blk[0], 0x40);
	assert_int_equal(*(u8 *)blk[1], 0x41);
	slab_fini(&vm);
}

/*
 * The headline scenario: usage stays under 30% for 30 s, then half of the
 * currently unused memory is returned to the OS. Two of eight blocks stay
 * live (25% usage); of the six free blocks, exactly three (50%) are released.
 */
static void
test_shrink_half_of_unused(void **state)
{
	(void)state;
	struct slab_policy pol = {
		.min = 0, .max = 8, .grow_step = 8,
		.grow_usage_pct = 90,
		.shrink_usage_pct = 30,          /* usage under 30% ...        */
		.shrink_after = 30000,           /* ... for 30 s ...           */
		.shrink_release_pct = 50,        /* ... releases 50% of unused */
	};
	struct slab vm;
	void *all[8], *live[2] = { NULL, NULL };
	timestamp_t t = 100000;
	int i;

	assert_int_equal(slab_init(&vm, SLAB_GRAIN_BYTES, &pol), 0);
	for (i = 0; i < 8; i++)
		all[i] = slab_alloc(&vm);
	assert_int_equal(slab_committed(&vm), 8);

	/* Keep the two lowest-index blocks live, free the six-block tail. */
	for (i = 0; i < 8; i++) {
		u32 ix = slab_index(&vm, all[i]);
		if (ix < 2)
			live[ix] = all[i];       /* key by index, not by order */
		else
			slab_free(&vm, all[i]);
	}
	assert_non_null(live[0]);
	assert_non_null(live[1]);
	assert_int_equal(slab_used(&vm), 2);     /* 2/8 = 25% < 30% */
	assert_int_equal(slab_avail(&vm), 6);

	/* Arm at t; nothing happens until the 30 s dwell elapses. */
	assert_int_equal(slab_gc(&vm, t), 0);
	assert_int_equal(slab_gc(&vm, t + 29999), 0);
	assert_int_equal(slab_committed(&vm), 8);

	/* Dwell met -> release 50% of the 6 unused blocks = 3. */
	assert_int_equal(slab_gc(&vm, t + 30000), -3);
	assert_int_equal(slab_committed(&vm), 5);
	assert_int_equal(slab_used(&vm), 2);     /* live blocks untouched */
	assert_int_equal(slab_avail(&vm), 3);

	/* Survivors keep their addresses. */
	assert_ptr_equal(live[0], slab_at(&vm, 0));
	assert_ptr_equal(live[1], slab_at(&vm, 1));
	slab_fini(&vm);
}

static void
test_shrink_stops_at_live_block(void **state)
{
	(void)state;
	struct slab_policy pol = { .min = 0, .max = 4, .grow_step = 4 };
	struct slab vm;
	void *blk[4];
	u32 idx[4];
	int i;

	assert_int_equal(slab_init(&vm, SLAB_GRAIN_BYTES, &pol), 0);
	for (i = 0; i < 4; i++) {
		blk[i] = slab_alloc(&vm);
		idx[i] = slab_index(&vm, blk[i]);
	}
	assert_int_equal(slab_committed(&vm), 4);

	/* Free the two lowest-index blocks; the two highest stay live. */
	for (i = 0; i < 4; i++) {
		if (idx[i] == 0 || idx[i] == 1)
			slab_free(&vm, blk[i]);
	}
	/* Top of the committed range is still live -> nothing reclaimable. */
	assert_int_equal(slab_shrink(&vm, 4), 0);
	assert_int_equal(slab_committed(&vm), 4);

	/* Free the single top block; shrink peels exactly one, stops at live. */
	for (i = 0; i < 4; i++) {
		if (idx[i] == 3)
			slab_free(&vm, blk[i]);
	}
	assert_int_equal(slab_shrink(&vm, 4), 1);
	assert_int_equal(slab_committed(&vm), 3);
	slab_fini(&vm);
}

static void
test_shrink_keeps_addresses_stable(void **state)
{
	(void)state;
	struct slab_policy pol = { .min = 0, .max = 8, .grow_step = 8 };
	struct slab vm;
	void *live[3];
	int i;

	assert_int_equal(slab_init(&vm, SLAB_GRAIN_BYTES, &pol), 0);

	/* Commit 8, keep 3 live at the lowest indices, free the rest. */
	{
		void *all[8];
		for (i = 0; i < 8; i++)
			all[i] = slab_alloc(&vm);
		/* Sort so the three lowest-index blocks are the survivors. */
		for (i = 0; i < 8; i++) {
			u32 ix = slab_index(&vm, all[i]);
			if (ix < 3)
				live[ix] = all[i];
			else
				slab_free(&vm, all[i]);
		}
	}
	for (i = 0; i < 3; i++)
		memset(live[i], 0x11 + i, slab_block_size(&vm));

	assert_true(slab_shrink(&vm, 8) >= 1);
	assert_int_equal(slab_committed(&vm), 3);

	/* Survivors keep their addresses and their contents after shrink. */
	for (i = 0; i < 3; i++) {
		assert_ptr_equal(live[i], slab_at(&vm, (u32)i));
		assert_int_equal(*(u8 *)live[i], (u8)(0x11 + i));
	}
	slab_fini(&vm);
}

/* ---- build-time (static) variant ----------------------------------------- */

static void
test_static_variant(void **state)
{
	(void)state;
	struct slab_class_policy pol = { .min = 2, .max = 8, .grow_step = 2 };
	struct slab_class vm;
	void *a, *b;

	assert_int_equal(slab_class_init(&vm, &pol), 0);
	assert_int_equal(slab_class_block_size(&vm), SLAB_CLASS_BLOCK_SIZE);
	assert_int_equal(slab_class_block_size(&vm), 2048);
	/* 2048 byte block: the grain is however many of those fit a grain */
	assert_int_equal(slab_class_grain(&vm), SLAB_GRAIN_BYTES / 2048);

	a = slab_class_alloc(&vm);
	b = slab_class_alloc(&vm);
	assert_non_null(a);
	assert_non_null(b);
	memset(a, 0x7e, slab_class_block_size(&vm));

	/* exhaust and auto-grow within policy, by whole grains */
	assert_non_null(slab_class_alloc(&vm));
	assert_int_equal(slab_class_committed(&vm) % slab_class_grain(&vm), 0);
	assert_true(slab_class_committed(&vm) >= slab_class_policy_min(&vm));
	assert_true(slab_class_committed(&vm) <= slab_class_policy_max(&vm));

	slab_class_free(&vm, a);
	slab_class_free(&vm, b);
	slab_class_fini(&vm);
}

/* ---- TCP reordering scenario --------------------------------------------- */

struct seg {
	u32 seq;
	u8  data[64];
};

static void
fill_seg(struct seg *s, u32 seq)
{
	unsigned j;
	s->seq = seq;
	for (j = 0; j < sizeof(s->data); j++)
		s->data[j] = (u8)(seq * 31u + j);
}

static int
check_seg(const struct seg *s, u32 seq)
{
	unsigned j;
	if (s->seq != seq)
		return 0;
	for (j = 0; j < sizeof(s->data); j++)
		if (s->data[j] != (u8)(seq * 31u + j))
			return 0;
	return 1;
}

/*
 * Emulate a TCP receiver holding out-of-order segments in a slab-backed
 * reorder window: block size chosen from the MTU, segments arrive shuffled,
 * are parked by sequence number, then delivered in order. The slab grows to
 * absorb the burst and shrinks as the window drains, while every parked
 * pointer stays valid throughout.
 */
static void
test_tcp_reorder(void **state)
{
	(void)state;
	enum { N = 40 };
	struct slab_policy pol = {
		.min = 4, .max = 64, .grow_step = 8,
		.grow_usage_pct = 75, .shrink_usage_pct = 20,
		.shrink_release_pct = 50,
	};
	struct slab vm;
	struct seg *win[N];
	unsigned i, order, delivered = 0;

	memset(win, 0, sizeof(win));
	/* MTU-driven block size (1500 -> 2048), big enough for a struct seg. */
	assert_int_equal(slab_init(&vm, SLAB_MTU_ETHERNET, &pol), 0);
	assert_true(slab_block_size(&vm) >= sizeof(struct seg));

	/* Receive all N segments in a shuffled order and park them. */
	for (i = 0; i < N; i++) {
		u32 seq = (i * 37u + 11u) % N;   /* deterministic permutation */
		struct seg *s = (struct seg *)slab_alloc(&vm);
		assert_non_null(s);
		fill_seg(s, seq);
		win[seq] = s;
	}
	assert_int_equal(slab_used(&vm), N);
	/* The burst forces growth only when the window does not already fit in the
	 * minimum - with a 2 MB grain all N segments land inside one grain and there
	 * is nothing to grow. Compare against the policy as enforced, not as asked. */
	if (slab_policy_max(&vm) > slab_policy_min(&vm))
		assert_true(slab_committed(&vm) > slab_policy_min(&vm));

	/* Deliver in strict sequence order, freeing and running the GC. */
	for (order = 0; order < N; order++) {
		struct seg *s = win[order];
		assert_non_null(s);
		assert_true(check_seg(s, order));
		slab_free(&vm, s);
		win[order] = NULL;
		delivered++;
		slab_gc(&vm, order);         /* drain -> policy may shrink */
	}

	assert_int_equal(delivered, N);
	assert_int_equal(slab_used(&vm), 0);
	/* window drained -> policy shrank the committed set back toward min */
	assert_true(slab_committed(&vm) >= slab_policy_min(&vm));
	assert_true(slab_committed(&vm) <= slab_policy_max(&vm));
	slab_fini(&vm);
}

/* ---- release granularity: the grain ---------------------------------------- */

/*
 * min, max and grow_step are rounded up to whole grains at init, and the rounded
 * values are what the slab enforces. A 256 byte block puts 16 blocks in a 4 K
 * page, so a policy asking for 2 and 4 gets 16 - which is not the slab being
 * generous, it is the slab admitting that one page is the smallest thing mmap
 * was ever going to give it.
 */
static void
test_grain_rounds_the_policy(void **state)
{
	(void)state;
	/* a block one sixteenth of a grain, so a grain is 16 blocks whatever the
	 * configured grain size is - CONFIG_MEM_HUGEPAGE makes it 2 MB, not a page */
	const unsigned bsize = SLAB_GRAIN_BYTES / 16;
	struct slab_policy pol = { .min = 2, .max = 20, .grow_step = 1 };
	struct slab vm;
	u32 g;

	assert_int_equal(slab_init(&vm, bsize, &pol), 0);
	g = slab_grain(&vm);
	assert_int_equal(g, 16);
	assert_int_equal(slab_grain_bytes(&vm), SLAB_GRAIN_BYTES);

	assert_int_equal(slab_policy_min(&vm), g);       /* 2  -> 16 */
	assert_int_equal(slab_policy_max(&vm), 2 * g);   /* 20 -> 32 */
	assert_int_equal(slab_committed(&vm), g);        /* min committed */
	assert_int_equal(slab_committed_bytes(&vm), SLAB_GRAIN_BYTES);

	/* grow_step of 1 block is meaningless: a step is at least a grain */
	assert_int_equal(slab_grow(&vm, 1), g);
	assert_int_equal(slab_committed(&vm), 2 * g);
	slab_fini(&vm);
}

/* A block at least as large as the grain already covers whole grains: grain is
 * one block, and there is then nothing to round. */
static void
test_grain_is_one_block_when_block_exceeds_grain(void **state)
{
	(void)state;
	struct slab_policy pol = { .min = 3, .max = 7, .grow_step = 1 };
	struct slab vm;

	assert_int_equal(slab_init(&vm, SLAB_GRAIN_BYTES, &pol), 0);
	assert_int_equal(slab_block_size(&vm), SLAB_GRAIN_BYTES);
	assert_int_equal(slab_grain(&vm), 1);
	/* nothing to round: the policy is passed through untouched */
	assert_int_equal(slab_policy_min(&vm), 3);
	assert_int_equal(slab_policy_max(&vm), 7);
	assert_int_equal(slab_committed(&vm), 3);
	slab_fini(&vm);
}

/*
 * The point of all of it: a shrink either releases a whole page or reports
 * nothing. Freeing less than a grain's worth of blocks must not move committed,
 * because madvise() could not have returned a page for it.
 */
static void
test_grain_shrink_is_all_or_nothing(void **state)
{
	(void)state;
	const unsigned bsize = SLAB_GRAIN_BYTES / 16;      /* grain = 16 blocks */
	struct slab_policy pol = { .min = 0, .max = 64, .grow_step = 16 };
	struct slab vm;
	void *blk[64];
	u32 g, i, committed;

	assert_int_equal(slab_init(&vm, bsize, &pol), 0);
	g = slab_grain(&vm);
	assert_int_equal(slab_grow(&vm, 2 * g), 2 * g);

	for (i = 0; i < 2 * g; i++) {
		blk[i] = slab_alloc(&vm);
		assert_non_null(blk[i]);
	}
	committed = slab_committed(&vm);

	/* Free every block of the top grain but one: still not releasable. */
	for (i = committed - g; i < committed - 1; i++)
		slab_free(&vm, slab_at(&vm, i));
	assert_int_equal(slab_shrink(&vm, g), 0);
	assert_int_equal(slab_committed(&vm), committed);

	/* Free the last one and the whole grain goes at once. */
	slab_free(&vm, slab_at(&vm, committed - 1));
	assert_int_equal(slab_shrink(&vm, g), g);
	assert_int_equal(slab_committed(&vm), committed - g);
	slab_fini(&vm);
}

/* Committed is grain-aligned after every operation, so the block count and the
 * memory held never drift apart. */
static void
test_grain_keeps_committed_aligned(void **state)
{
	(void)state;
	struct slab_policy pol = {
		.min = 1, .max = 100, .grow_step = 5,
		.grow_usage_pct = 60, .shrink_usage_pct = 40,
		.shrink_release_pct = 50, .shrink_after = 0,
	};
	struct slab vm;
	void *held[512];
	u32 g, n = 0, i, round;

	assert_int_equal(slab_init(&vm, SLAB_GRAIN_BYTES / 8, &pol), 0);
	g = slab_grain(&vm);
	assert_int_equal(g, 8);

	for (round = 0; round < 12; round++) {
		/* allocate a while, then free most of it, gc in between */
		for (i = 0; i < 7 && n < 512; i++) {
			void *p = slab_alloc(&vm);
			if (p)
				held[n++] = p;
		}
		assert_int_equal(slab_committed(&vm) % g, 0);
		slab_gc(&vm, round * 1000);
		assert_int_equal(slab_committed(&vm) % g, 0);
		for (i = 0; i < 5 && n; i++)
			slab_free(&vm, held[--n]);
		slab_gc(&vm, round * 1000 + 500);
		assert_int_equal(slab_committed(&vm) % g, 0);
		assert_int_equal(slab_committed_bytes(&vm) % SLAB_GRAIN_BYTES, 0);
	}
	while (n)
		slab_free(&vm, held[--n]);
	slab_fini(&vm);
}

/* A grain coarser than a page - a huge-page or hugetlb backing - is the same
 * rule with a bigger unit, and is what stops a shrink below it. */
static void
test_grain_coarse_unit(void **state)
{
	(void)state;
	/* SLAB_GRAIN_BYTES is a build-time knob; check the arithmetic it drives. */
	assert_int_equal(slab_grain_for(11), SLAB_GRAIN_BYTES / 2048);
	assert_int_equal(slab_grain_for(12), SLAB_GRAIN_BYTES / 4096);
	/* a block at or above the grain always yields one */
	assert_int_equal(slab_grain_for(21), 1);
	assert_int_equal(slab_grain_for(30), 1);
	/* rounding saturates instead of wrapping */
	assert_int_equal(slab_grain_round(0, 16), 0);
	assert_int_equal(slab_grain_round(1, 16), 16);
	assert_int_equal(slab_grain_round(16, 16), 16);
	assert_int_equal(slab_grain_round(17, 16), 32);
	assert_int_equal(slab_grain_round((u32)~0U, 16), (u32)~0U - 15);
	assert_int_equal(slab_grain_round(42, 1), 42);
}

int
main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_shift_for),
		cmocka_unit_test(test_block_size),
		cmocka_unit_test(test_alloc_free_lifo),
		cmocka_unit_test(test_exhaustion_grows_to_max),
		cmocka_unit_test(test_check_vetoes_grow),
		cmocka_unit_test(test_gc_grow_and_shrink),
		cmocka_unit_test(test_idle_shrink_after_dwell),
		cmocka_unit_test(test_idle_timer_resets_on_activity),
		cmocka_unit_test(test_policy_hot_swap),
		cmocka_unit_test(test_shrink_half_of_unused),
		cmocka_unit_test(test_shrink_stops_at_live_block),
		cmocka_unit_test(test_shrink_keeps_addresses_stable),
		cmocka_unit_test(test_grain_rounds_the_policy),
		cmocka_unit_test(test_grain_is_one_block_when_block_exceeds_grain),
		cmocka_unit_test(test_grain_shrink_is_all_or_nothing),
		cmocka_unit_test(test_grain_keeps_committed_aligned),
		cmocka_unit_test(test_grain_coarse_unit),
		cmocka_unit_test(test_static_variant),
		cmocka_unit_test(test_tcp_reorder),
	};
	return cmocka_run_group_tests_name("slab", tests, NULL, NULL);
}
