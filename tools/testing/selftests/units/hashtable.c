/*
 * Unit tests for the intrusive hash table <hpc/hash/table.h>, which buckets
 * struct queue heads (<hpc/queue.h>).
 *
 * The plain variant only. The RCU variant is a separate unit, hashtable_rcu.c,
 * built only when CONFIG_RCU is enabled - it needs liburcu linked, and there is
 * nothing to assert about it in a build where it does not exist.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>

#include <hpc/compiler.h>
#include <hpc/hash/table.h>

struct data { unsigned int id, hash; struct qnode q; };

/* ---- plain variant ------------------------------------------------------- */

static void
test_table_add_iterate(void **state)
{
	(void)state;
	DECLARE_HASHTABLE(table, 4);
	hash_init_table(table, 4);
	assert_true(hash_empty(table, 0));

	struct data d[6];
	for (unsigned i = 0; i < 6; i++) {
		d[i].id = i;
		d[i].hash = 0;            /* pile everything into one bucket */
		hash_add(table, &d[i].q, d[i].hash);
	}
	assert_false(hash_empty(table, 0));

	unsigned n = 0;
	hash_for_each(table, 0, it, struct data, q)
		n++;
	assert_int_equal(n, 6);

	/* other buckets stay empty */
	assert_true(hash_empty(table, 1));
	unsigned m = 0;
	hash_for_each(table, 1, it, struct data, q)
		m++;
	assert_int_equal(m, 0);
}

static void
test_table_del(void **state)
{
	(void)state;
	DECLARE_HASHTABLE(table, 4);
	hash_init_table(table, 4);

	struct data d[3] = { { 0, 0 }, { 1, 0 }, { 2, 0 } };
	for (unsigned i = 0; i < 3; i++)
		hash_add(table, &d[i].q, 0);

	assert_true(hash_hashed(&d[1].q));
	hash_del(&d[1].q);            /* O(1) middle removal, no bucket walk */

	unsigned n = 0, seen = 0;
	hash_for_each(table, 0, it, struct data, q) { n++; seen |= 1u << it->id; }
	assert_int_equal(n, 2);
	assert_int_equal(seen, (1u << 0) | (1u << 2));

	hash_del_init(&d[0].q);
	assert_false(hash_hashed(&d[0].q));

	n = 0;
	hash_for_each_delsafe(table, 0, it, struct data, q) {
		hash_del(&it->q); n++;
	}
	assert_int_equal(n, 1);       /* only id 2 remained */
	assert_true(hash_empty(table, 0));
}

static void
test_table_ruc(void **state)
{
	(void)state;
	DECLARE_HASHTABLE(table, 4);
	hash_init_table(table, 4);

	struct data d[3] = { { 0, 0 }, { 1, 0 }, { 2, 0 } };
	for (unsigned i = 0; i < 3; i++)
		hash_add(table, &d[i].q, 0);

	/* recently-used-cache: touch id 0, it should move to the bucket head */
	hash_ruc(table, &d[0].q, 0);
	assert_int_equal(queue_entry(table[0].first, struct data, q)->id, 0);
}

static void
test_table_uniform(void **state)
{
	(void)state;
	DECLARE_HASHTABLE(table, 8);
	hash_init_table(table, 8);

	struct data d[512];
	for (unsigned i = 0; i < 512; i++) {
		d[i].id = i;
		d[i].hash = hash_seq(i, 8);
		hash_add(table, &d[i].q, d[i].hash);
	}

	unsigned hits = 0;
	for (unsigned i = 0; i < 512; i++) {
		unsigned depth = 0;
		hash_for_each(table, d[i].hash, it, struct data, q) {
			depth++; hits++;
		}
		assert_int_equal(depth, 2); /* 512 keys over 256 buckets */
	}
	assert_int_equal(hits, 1024);
}

int
main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_table_add_iterate),
		cmocka_unit_test(test_table_del),
		cmocka_unit_test(test_table_ruc),
		cmocka_unit_test(test_table_uniform),
	};
	return cmocka_run_group_tests_name("hashtable", tests, NULL, NULL);
}
