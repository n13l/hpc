/*
 * Unit tests for the RCU spelling of the intrusive hashable queue
 * <hpc/queue.h> - queue_add_head_rcu()/queue_del_rcu()/queue_walk_rcu() and
 * friends, which publish and traverse with liburcu in the flavour this build
 * selected (<hpc/rcu.h>). The plain spelling has its own unit, queue.c.
 *
 * This unit exists only when CONFIG_RCU is enabled (see the Kbuild), so nothing
 * here is conditional. It runs single threaded, which bounds what it can claim:
 * the structural assertions are about what a reader would see, not about racing
 * one against a writer. What it does establish beyond structure is that the
 * read-side sections, grace periods and deferred reclaim of the linked liburcu
 * flavour are the ones the header publishes into.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <setjmp.h>
#include <cmocka.h>

#include <hpc/compiler.h>
#include <hpc/queue.h>
#include <hpc/rcu.h>

struct data { unsigned int id; struct qnode q; };

/* ---- structural correctness of the lockless spelling ---------------------- */

static void
test_rcu_functional(void **state)
{
	(void)state;
	DEFINE_QUEUE(q);
	struct data d[3] = { { .id = 0 }, { .id = 1 }, { .id = 2 } };

	for (unsigned i = 0; i < 3; i++)
		queue_add_head_rcu(&q, &d[i].q);

	unsigned n = 0, expect[3] = { 2, 1, 0 };
	struct qnode *it;
	queue_walk_rcu(&q, it)
		assert_int_equal(queue_entry(it, struct data, q)->id, expect[n++]);
	assert_int_equal(n, 3);

	/* typed rcu walk */
	n = 0;
	queue_for_each_rcu(&q, x, struct data, q)
		assert_int_equal(x->id, expect[n++]);
	assert_int_equal(n, 3);

	/* del_rcu unlinks and marks unhashed, keeping next for parked readers */
	struct qnode *victim_next = d[1].q.next;
	queue_del_rcu(&d[1].q);
	assert_true(qnode_unhashed(&d[1].q));
	assert_ptr_equal(d[1].q.next, victim_next); /* forward link preserved */

	unsigned m = 0, rest[2] = { 2, 0 };
	queue_walk_rcu(&q, it)
		assert_int_equal(queue_entry(it, struct data, q)->id, rest[m++]);
	assert_int_equal(m, 2);

	/* rcu insert before/after keep both directions linked */
	struct data e = { .id = 99 };
	queue_add_before_rcu(&e.q, &d[0].q);   /* before tail node id 0 */
	m = 0;
	unsigned rest2[3] = { 2, 99, 0 };
	queue_walk_rcu(&q, it)
		assert_int_equal(queue_entry(it, struct data, q)->id, rest2[m++]);
	assert_int_equal(m, 3);

	struct data f = { .id = 77 };
	queue_add_behind_rcu(&f.q, &d[2].q);   /* after head node id 2 */
	m = 0;
	unsigned rest3[4] = { 2, 77, 99, 0 };
	queue_walk_rcu(&q, it)
		assert_int_equal(queue_entry(it, struct data, q)->id, rest3[m++]);
	assert_int_equal(m, 4);
}

/* ---- read-side section, grace period, deferred reclaim ------------------- */

struct node { unsigned int id; struct qnode q; struct rcu_head rcu; };

static unsigned int reclaimed;

static void
reclaim(struct rcu_head *head)
{
	struct node *n = container_of(head, struct node, rcu);

	reclaimed |= 1u << n->id;
	free(n);
}

static void
test_rcu_retire(void **state)
{
	(void)state;
	DEFINE_QUEUE(q);

	rcu_register_thread();

	for (unsigned int i = 0; i < 3; i++) {
		struct node *n = calloc(1, sizeof(*n));
		assert_non_null(n);
		n->id = i;
		queue_add_head_rcu(&q, &n->q);
	}

	/* reader: one section covering the whole traversal */
	unsigned int seen = 0, count = 0;
	rcu_read_lock();
	queue_for_each_rcu(&q, it, struct node, q) {
		seen |= 1u << it->id;
		count++;
	}
	rcu_read_unlock();
	assert_int_equal(count, 3);
	assert_int_equal(seen, 0x7);

	/* writer: unlink, wait out the readers that could still see it, free */
	struct node *victim = queue_entry(queue_first(&q), struct node, q);
	unsigned int victim_id = victim->id;
	queue_del_rcu(&victim->q);
	synchronize_rcu();
	free(victim);

	struct qnode *it;
	count = 0;
	rcu_read_lock();
	queue_walk_rcu(&q, it) count++;
	rcu_read_unlock();
	assert_int_equal(count, 2);

	/* the same retirement, deferred to a callback instead of waited on */
	reclaimed = 0;
	queue_for_each_delsafe(&q, it, struct node, q) {
		queue_del_rcu(&it->q);
		call_rcu(&it->rcu, reclaim);
	}
	rcu_barrier();
	assert_int_equal(reclaimed, 0x7 & ~(1u << victim_id));
	assert_true(queue_empty(&q));

	rcu_unregister_thread();
}

int
main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_rcu_functional),
		cmocka_unit_test(test_rcu_retire),
	};
	return cmocka_run_group_tests_name("queue_rcu", tests, NULL, NULL);
}
