/*
 * Unit tests for the RCU spelling of the intrusive red-black tree
 * <hpc/rbtree.h> - rbtree_link_node_rcu(), rbtree_replace_rcu() and the
 * lockless in-order traversal, which publish and traverse with liburcu in the
 * flavour this build selected (<hpc/rcu.h>). The plain spelling has its own
 * unit, rbtree.c; the shared payload type, BST descent and red-black audit come
 * from rbtree_util.h.
 *
 * This unit exists only when CONFIG_RCU is enabled (see the Kbuild), so nothing
 * here is conditional. It runs single threaded: the structural assertions say
 * what a reader would see, not what happens when one races a writer. Note what
 * the tree does *not* promise - only linking and replacement are published
 * atomically; rbtree_insert_color() and rbtree_erase() rebalance by mutating
 * several existing nodes, so a reader racing a rebalance may transiently miss a
 * node that is in the tree. Retiring a node still needs a grace period, which
 * the reclaim test exercises.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <setjmp.h>
#include <cmocka.h>

#include <hpc/compiler.h>
#include <hpc/rbtree.h>
#include <hpc/rcu.h>

#include "rbtree_util.h"

/* ---- test-owned ordering: BST descent + rcu link + rebalance -------------- */

static bool
tree_insert_rcu(struct rbtree *t, struct data *d)
{
	struct rbnode *parent, **link = find_slot(t, d->id, &parent);

	if (*link)
		return false;                  /* already present */
	rbtree_link_node_rcu(&d->rb, parent, link);
	rbtree_insert_color(t, &d->rb);
	return true;
}

/* ---- structural correctness of the lockless spelling ---------------------- */

static void
test_rcu_functional(void **state)
{
	(void)state;
	DEFINE_RBTREE(t);
	unsigned ids[] = { 40, 20, 60, 10, 30, 50, 70, 5, 25 };
	enum { N = sizeof(ids) / sizeof(ids[0]) };
	struct data d[N];

	for (unsigned i = 0; i < N; i++) {
		d[i].id = ids[i];
		assert_true(tree_insert_rcu(&t, &d[i]));
		validate(&t);
	}

	/* lockless in-order walk matches the sorted key set */
	unsigned expect[N];
	for (unsigned i = 0; i < N; i++)
		expect[i] = ids[i];
	/* simple ascending sort of expect[] */
	for (unsigned i = 0; i < N; i++)
		for (unsigned j = i + 1; j < N; j++)
			if (expect[j] < expect[i]) {
				unsigned tmp = expect[i];
				expect[i] = expect[j];
				expect[j] = tmp;
			}

	unsigned n = 0;
	struct rbnode *it;
	rbtree_walk_rcu(&t, it)
		assert_int_equal(rbtree_entry(it, struct data, rb)->id,
		                 expect[n++]);
	assert_int_equal(n, N);

	/* typed rcu walk agrees */
	n = 0;
	rbtree_for_each_rcu(&t, x, struct data, rb)
		assert_int_equal(x->id, expect[n++]);
	assert_int_equal(n, N);

	/* replace_rcu publishes a fresh node in the same slot */
	struct data repl = { .id = 30 };
	struct data *old = tree_find(&t, 30);
	rbtree_replace_rcu(&t, &old->rb, &repl.rb);
	validate(&t);
	assert_ptr_equal(tree_find(&t, 30), &repl);

	n = 0;
	rbtree_walk_rcu(&t, it)
		assert_int_equal(rbtree_entry(it, struct data, rb)->id,
		                 expect[n++]);
	assert_int_equal(n, N);
}

/* ---- read-side section, grace period, deferred reclaim ------------------- */

struct node { struct data d; struct rcu_head rcu; };

static unsigned int reclaimed;

static void
reclaim(struct rcu_head *head)
{
	struct node *n = container_of(head, struct node, rcu);

	reclaimed |= 1u << n->d.id;
	free(n);
}

static void
test_rcu_retire(void **state)
{
	(void)state;
	DEFINE_RBTREE(t);

	rcu_register_thread();

	for (unsigned int i = 0; i < 5; i++) {
		struct node *n = calloc(1, sizeof(*n));
		assert_non_null(n);
		n->d.id = i;
		assert_true(tree_insert_rcu(&t, &n->d));
		validate(&t);
	}

	/* reader: one section covering the whole in-order traversal */
	unsigned int n = 0;
	rcu_read_lock();
	rbtree_for_each_rcu(&t, x, struct data, rb)
		assert_int_equal(x->id, n++);
	rcu_read_unlock();
	assert_int_equal(n, 5);

	/*
	 * Replace a node the RCU way: the new node is published into the slot, the
	 * old one is only freed once the readers that could still be standing on
	 * it are gone.
	 */
	struct node *fresh = calloc(1, sizeof(*fresh));
	assert_non_null(fresh);
	fresh->d.id = 2;
	struct data *old = tree_find(&t, 2);
	struct node *stale = container_of(old, struct node, d);
	rbtree_replace_rcu(&t, &old->rb, &fresh->d.rb);
	validate(&t);
	assert_ptr_equal(tree_find(&t, 2), &fresh->d);
	synchronize_rcu();
	free(stale);

	n = 0;
	rcu_read_lock();
	rbtree_for_each_rcu(&t, x, struct data, rb)
		assert_int_equal(x->id, n++);
	rcu_read_unlock();
	assert_int_equal(n, 5);

	/* tear the tree down, deferring every free to a callback */
	reclaimed = 0;
	struct rbnode *it, *tmp;
	rbtree_walk_delsafe(&t, it, tmp) {
		struct node *victim =
			container_of(rbtree_entry(it, struct data, rb),
			             struct node, d);
		rbtree_erase(&t, it);
		call_rcu(&victim->rcu, reclaim);
	}
	rcu_barrier();
	assert_int_equal(reclaimed, 0x1f);
	assert_true(rbtree_empty(&t));

	rcu_unregister_thread();
}

int
main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_rcu_functional),
		cmocka_unit_test(test_rcu_retire),
	};
	return cmocka_run_group_tests_name("rbtree_rcu", tests, NULL, NULL);
}
