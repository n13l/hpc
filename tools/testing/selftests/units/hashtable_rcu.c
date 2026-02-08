/*
 * Unit tests for the RCU variant of the intrusive hash table
 * <hpc/hash/table.h> - hash_add_rcu(), hash_del_rcu() and hash_for_each_rcu(),
 * which are the queue RCU primitives applied per bucket and so publish and
 * traverse with liburcu in the flavour this build selected (<hpc/rcu.h>). The
 * plain variant has its own unit, hashtable.c.
 *
 * This unit exists only when CONFIG_RCU is enabled (see the Kbuild), so nothing
 * here is conditional. It runs single threaded: the structural assertions say
 * what a reader would see, not what happens when one races a writer. The reclaim
 * test additionally goes through a real read-side section, grace period and
 * call_rcu() callback, which is what pins the header's publication to the
 * liburcu flavour actually linked.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <setjmp.h>
#include <cmocka.h>

#include <hpc/compiler.h>
#include <hpc/hash/table.h>
#include <hpc/rcu.h>

struct data { unsigned int id, hash; struct qnode q; };

/* ---- structural correctness of the lockless bucket walk ------------------- */

static void
test_table_rcu(void **state)
{
	(void)state;
	DECLARE_HASHTABLE(table, 4);
	hash_init_table(table, 4);

	struct data d[4];
	for (unsigned i = 0; i < 4; i++) {
		d[i].id = i;
		d[i].hash = 0;
		hash_add_rcu(table, &d[i].q, d[i].hash);
	}

	unsigned n = 0, seen = 0;
	hash_for_each_rcu(table, 0, it, struct data, q) {
		n++; seen |= 1u << it->id;
	}
	assert_int_equal(n, 4);
	assert_int_equal(seen, 0xf);

	/* del_rcu unlinks and marks unhashed but keeps ->next for parked readers */
	struct qnode *victim_next = d[1].q.next;
	hash_del_rcu(&d[1].q);
	assert_false(hash_hashed(&d[1].q));
	assert_ptr_equal(d[1].q.next, victim_next);

	n = 0; seen = 0;
	hash_for_each_rcu(table, 0, it, struct data, q) {
		n++; seen |= 1u << it->id;
	}
	assert_int_equal(n, 3);
	assert_int_equal(seen, (1u << 0) | (1u << 2) | (1u << 3));
}

/* ---- buckets stay independent under the lockless spelling ---------------- */

static void
test_table_rcu_buckets(void **state)
{
	(void)state;
	DECLARE_HASHTABLE(table, 4);
	hash_init_table(table, 4);

	struct data d[8];
	for (unsigned i = 0; i < 8; i++) {
		d[i].id = i;
		d[i].hash = hash_seq(i, 2);      /* 4 buckets, 2 keys each */
		hash_add_rcu(table, &d[i].q, d[i].hash);
	}

	for (unsigned b = 0; b < 4; b++) {
		unsigned n = 0;
		hash_for_each_rcu(table, b, it, struct data, q) {
			assert_int_equal(hash_seq(it->id, 2), b);
			n++;
		}
		assert_int_equal(n, 2);
	}

	/* removing from one bucket leaves the others as they were */
	hash_del_rcu(&d[0].q);
	unsigned n = 0;
	hash_for_each_rcu(table, hash_seq(0, 2), it, struct data, q) n++;
	assert_int_equal(n, 1);
	n = 0;
	hash_for_each_rcu(table, hash_seq(1, 2), it, struct data, q) n++;
	assert_int_equal(n, 2);
}

/* ---- read-side section, grace period, deferred reclaim ------------------- */

struct node { unsigned int id, hash; struct qnode q; struct rcu_head rcu; };

static unsigned int reclaimed;

static void
reclaim(struct rcu_head *head)
{
	struct node *n = container_of(head, struct node, rcu);

	reclaimed |= 1u << n->id;
	free(n);
}

static void
test_table_rcu_retire(void **state)
{
	(void)state;
	DECLARE_HASHTABLE(table, 4);
	hash_init_table(table, 4);

	rcu_register_thread();

	for (unsigned int i = 0; i < 4; i++) {
		struct node *n = calloc(1, sizeof(*n));
		assert_non_null(n);
		n->id = i;
		n->hash = 0;                     /* one bucket, so one chain */
		hash_add_rcu(table, &n->q, n->hash);
	}

	unsigned int seen = 0, count = 0;
	rcu_read_lock();
	hash_for_each_rcu(table, 0, it, struct node, q) {
		seen |= 1u << it->id;
		count++;
	}
	rcu_read_unlock();
	assert_int_equal(count, 4);
	assert_int_equal(seen, 0xf);

	/* writer: unlink, wait out the readers that could still see it, free */
	struct node *victim = queue_entry(table[0].first, struct node, q);
	unsigned int victim_id = victim->id;
	hash_del_rcu(&victim->q);
	synchronize_rcu();
	free(victim);

	count = 0;
	rcu_read_lock();
	hash_for_each_rcu(table, 0, it, struct node, q) count++;
	rcu_read_unlock();
	assert_int_equal(count, 3);

	/* drain the bucket, deferring every free to a callback */
	reclaimed = 0;
	hash_for_each_delsafe(table, 0, it, struct node, q) {
		hash_del_rcu(&it->q);
		call_rcu(&it->rcu, reclaim);
	}
	rcu_barrier();
	assert_int_equal(reclaimed, 0xf & ~(1u << victim_id));
	assert_true(hash_empty(table, 0));

	rcu_unregister_thread();
}

int
main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_table_rcu),
		cmocka_unit_test(test_table_rcu_buckets),
		cmocka_unit_test(test_table_rcu_retire),
	};
	return cmocka_run_group_tests_name("hashtable_rcu", tests, NULL, NULL);
}
