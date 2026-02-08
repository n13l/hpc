/*
 * Unit tests for the expiring block cache over the slab: TTL expiry, idle
 * expiry (with touch), reaping back to the free list, and the build-time
 * struct slab_cache_class variant.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>
#include <string.h>

#include <hpc/compiler.h>

#define SLAB_CLASS_BLOCK_SIZE 2048

#include <mem/slab_cache.h>
#include <mem/slab_cache_class.h>

/* ---- TTL expiry ---------------------------------------------------------- */

static void
test_cache_ttl_expiry(void **state)
{
	(void)state;
	struct slab_policy pol = { .min = 4, .max = 16, .grow_step = 4 };
	struct slab_cache c;
	timestamp_t t = 10000;
	void *a;

	/* TTL = 1000 ms, no idle timeout. */
	assert_int_equal(slab_cache_init(&c, 256, &pol, 1000, 0), 0);

	a = slab_cache_alloc(&c, t);
	assert_non_null(a);
	memset(a, 0xab, slab_cache_block_size(&c));
	assert_int_equal(slab_cache_live(&c), 1);

	/* Still alive just before the deadline; contents intact. */
	assert_false(slab_cache_expired(&c, a, t + 999));
	assert_int_equal(*(u8 *)a, 0xab);

	/* Touch does NOT extend the absolute TTL. */
	assert_true(slab_cache_touch(&c, a, t + 999));
	assert_true(slab_cache_expired(&c, a, t + 1000));

	/* Reap returns the block to the slab free list. */
	assert_int_equal(slab_cache_reap(&c, t + 1000), 1);
	assert_int_equal(slab_cache_live(&c), 0);
	assert_int_equal(slab_used(&c.slab), 0);

	slab_cache_fini(&c);
}

/* ---- idle expiry and touch ----------------------------------------------- */

static void
test_cache_idle_expiry(void **state)
{
	(void)state;
	struct slab_policy pol = { .min = 4, .max = 16, .grow_step = 4 };
	struct slab_cache c;
	timestamp_t t = 0;
	void *a, *b;

	/* No TTL, idle timeout = 500 ms. */
	assert_int_equal(slab_cache_init(&c, 256, &pol, 0, 500), 0);

	a = slab_cache_alloc(&c, t);   /* never touched again */
	b = slab_cache_alloc(&c, t);   /* kept alive by touching */
	assert_non_null(a);
	assert_non_null(b);

	/* Neither is idle yet. */
	assert_false(slab_cache_expired(&c, a, t + 499));

	/* Touch b at 400 -> its idle window restarts from 400. */
	assert_true(slab_cache_touch(&c, b, t + 400));

	/* At 500: a has been idle 500 ms (expired); b only 100 ms (alive). */
	assert_true(slab_cache_expired(&c, a, t + 500));
	assert_false(slab_cache_expired(&c, b, t + 500));

	/* Reaping at 500 collects only a. */
	assert_int_equal(slab_cache_reap(&c, t + 500), 1);
	assert_int_equal(slab_cache_live(&c), 1);

	/* b expires 500 ms after its last touch (400 + 500 = 900). */
	assert_false(slab_cache_expired(&c, b, t + 899));
	assert_true(slab_cache_expired(&c, b, t + 900));
	assert_int_equal(slab_cache_reap(&c, t + 900), 1);
	assert_int_equal(slab_cache_live(&c), 0);

	slab_cache_fini(&c);
}

/* ---- reaped block is reusable -------------------------------------------- */

static void
test_cache_reap_recycles_block(void **state)
{
	(void)state;
	struct slab_policy pol = { .min = 2, .max = 8, .grow_step = 2 };
	struct slab_cache c;
	timestamp_t t = 1000;
	void *a, *b;
	u32 ia;

	assert_int_equal(slab_cache_init(&c, 256, &pol, 100, 0), 0);

	a = slab_cache_alloc(&c, t);
	ia = slab_index(&c.slab, a);
	assert_int_equal(slab_cache_reap(&c, t + 100), 1);   /* a expires */

	/* The freed slot is available again (LIFO -> same block). */
	b = slab_cache_alloc(&c, t + 100);
	assert_int_equal(slab_index(&c.slab, b), ia);
	assert_ptr_equal(a, b);
	assert_int_equal(slab_cache_live(&c), 1);
	slab_cache_fini(&c);
}

/* ---- expiry drains usage, gc returns memory to the OS --------------------- */

static void
test_cache_gc_shrinks_after_expiry(void **state)
{
	(void)state;
	struct slab_policy pol = {
		.min = 0, .max = 16, .grow_step = 4,
		.grow_usage_pct = 90, .shrink_usage_pct = 40,
		.shrink_release_pct = 50, .shrink_after = 0,
	};
	struct slab_cache c;
	timestamp_t t = 5000;
	int i;

	/*
	 * One grain per block, so the committed counts below are the policy's
	 * arithmetic and not the grain rounding's - memory is grown and released in
	 * whole grains (see <mem/slab_vm.h>). The TTL and idle units above keep a
	 * smaller block on purpose; expiry does not depend on the granularity.
	 */
	assert_int_equal(slab_cache_init(&c, SLAB_GRAIN_BYTES, &pol, 1000, 0), 0);

	for (i = 0; i < 8; i++)
		assert_non_null(slab_cache_alloc(&c, t));
	assert_int_equal(slab_committed(&c.slab), 8);
	assert_int_equal(slab_cache_live(&c), 8);

	/* Everything has expired by now; gc reaps then shrinks the slab. */
	assert_int_equal(slab_cache_gc(&c, t + 1000), -4); /* 50% of 8 free */
	assert_int_equal(c.reaps, 8);
	assert_int_equal(slab_cache_live(&c), 0);
	assert_int_equal(slab_used(&c.slab), 0);
	assert_int_equal(slab_committed(&c.slab), 4);

	slab_cache_fini(&c);
}

/* ---- a touched block survives a reap that clears the rest ----------------- */

static void
test_cache_touch_keeps_block_alive(void **state)
{
	(void)state;
	struct slab_policy pol = { .min = 4, .max = 16, .grow_step = 4 };
	struct slab_cache c;
	timestamp_t t = 0;
	void *blk[4], *keep;
	u32 ikeep;
	int i;

	assert_int_equal(slab_cache_init(&c, 256, &pol, 0, 1000), 0);

	for (i = 0; i < 4; i++)
		blk[i] = slab_cache_alloc(&c, t);
	keep = blk[0];
	ikeep = slab_index(&c.slab, keep);
	memset(keep, 0x5c, slab_cache_block_size(&c));

	/* Keep blk[0] alive; let the other three go idle. */
	assert_true(slab_cache_touch(&c, keep, t + 900));

	assert_int_equal(slab_cache_reap(&c, t + 1000), 3);
	assert_int_equal(slab_cache_live(&c), 1);

	/* The survivor kept its address and its contents. */
	assert_ptr_equal(keep, slab_at(&c.slab, ikeep));
	assert_int_equal(*(u8 *)keep, 0x5c);
	assert_false(slab_cache_expired(&c, keep, t + 1000));

	slab_cache_fini(&c);
}

/* ---- build-time (class) variant ------------------------------------------ */

static void
test_cache_class_variant(void **state)
{
	(void)state;
	struct slab_class_policy pol = { .min = 4, .max = 16, .grow_step = 4 };
	struct slab_cache_class c;
	timestamp_t t = 2000;
	void *a;

	assert_int_equal(slab_cache_class_init(&c, &pol, 300, 0), 0);
	assert_int_equal(slab_cache_class_block_size(&c), SLAB_CLASS_BLOCK_SIZE);

	a = slab_cache_class_alloc(&c, t);
	assert_non_null(a);
	memset(a, 0x33, slab_cache_class_block_size(&c));
	assert_int_equal(slab_cache_class_live(&c), 1);

	assert_false(slab_cache_class_expired(&c, a, t + 299));
	assert_true(slab_cache_class_expired(&c, a, t + 300));

	assert_int_equal(slab_cache_class_reap(&c, t + 300), 1);
	assert_int_equal(slab_cache_class_live(&c), 0);
	assert_int_equal(slab_class_used(&c.cls), 0);

	slab_cache_class_fini(&c);
}

int
main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_cache_ttl_expiry),
		cmocka_unit_test(test_cache_idle_expiry),
		cmocka_unit_test(test_cache_reap_recycles_block),
		cmocka_unit_test(test_cache_gc_shrinks_after_expiry),
		cmocka_unit_test(test_cache_touch_keeps_block_alive),
		cmocka_unit_test(test_cache_class_variant),
	};
	return cmocka_run_group_tests_name("slab_cache", tests, NULL, NULL);
}
