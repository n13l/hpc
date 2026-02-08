/*
 * Unit tests for the intrusive hashable queue <hpc/queue.h> - the Linux-hlist
 * style single-head doubly linked list with **prev removal.
 *
 * The plain spelling only. The lockless one is a separate unit, queue_rcu.c,
 * built only when CONFIG_RCU is enabled - it needs liburcu linked, and there is
 * nothing to assert about it in a build where it does not exist.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>

#include <hpc/compiler.h>
#include <hpc/queue.h>

struct data { unsigned int id; struct qnode q; };

/* ---- init / empty -------------------------------------------------------- */

static void
test_init_empty(void **state)
{
	(void)state;
	DEFINE_QUEUE(q);
	assert_true(queue_empty(&q));
	assert_null(queue_first(&q));

	struct data d = { .id = 1 };
	qnode_init(&d.q);
	assert_true(qnode_unhashed(&d.q));
	assert_false(qnode_hashed(&d.q));
}

/* ---- add_head keeps LIFO order, like hlist_add_head ---------------------- */

static void
test_add_head_order(void **state)
{
	(void)state;
	DEFINE_QUEUE(q);
	struct data d[3] = { { .id = 0 }, { .id = 1 }, { .id = 2 } };

	for (unsigned i = 0; i < 3; i++)
		queue_add_head(&q, &d[i].q);

	assert_false(queue_empty(&q));
	assert_true(qnode_hashed(&d[0].q));

	unsigned n = 0, expect[3] = { 2, 1, 0 };
	struct qnode *it;
	queue_walk(&q, it) {
		struct data *x = queue_entry(it, struct data, q);
		assert_int_equal(x->id, expect[n++]);
	}
	assert_int_equal(n, 3);

	/* typed iteration must agree with the node walk */
	n = 0;
	queue_for_each(&q, x, struct data, q)
		assert_int_equal(x->id, expect[n++]);
	assert_int_equal(n, 3);
}

/* ---- **prev removal from the middle, no head walk needed ----------------- */

static void
test_del_middle(void **state)
{
	(void)state;
	DEFINE_QUEUE(q);
	struct data d[3] = { { .id = 0 }, { .id = 1 }, { .id = 2 } };

	for (unsigned i = 0; i < 3; i++)
		queue_add_head(&q, &d[i].q);

	/* queue is 2,1,0; drop the middle node (id 1) */
	queue_del(&d[1].q);

	unsigned n = 0, expect[2] = { 2, 0 };
	struct qnode *it;
	queue_walk(&q, it)
		assert_int_equal(queue_entry(it, struct data, q)->id, expect[n++]);
	assert_int_equal(n, 2);

	/* del_init leaves the node reusable and reporting unhashed */
	queue_del_init(&d[2].q);
	assert_true(qnode_unhashed(&d[2].q));
	/* del_init is idempotent on an already-removed node */
	queue_del_init(&d[2].q);
	assert_true(qnode_unhashed(&d[2].q));

	n = 0;
	queue_walk(&q, it)
		assert_int_equal(queue_entry(it, struct data, q)->id, 0);
	queue_walk(&q, it) n++;
	assert_int_equal(n, 1);
}

/* ---- add_before / add_behind --------------------------------------------- */

static void
test_add_before_behind(void **state)
{
	(void)state;
	DEFINE_QUEUE(q);
	struct data a = { .id = 10 }, b = { .id = 20 }, c = { .id = 30 };

	queue_add_head(&q, &a.q);              /* [a] */
	queue_add_behind(&b.q, &a.q);          /* [a, b] */
	queue_add_before(&c.q, &b.q);          /* [a, c, b] */

	unsigned n = 0, expect[3] = { 10, 30, 20 };
	struct qnode *it;
	queue_walk(&q, it)
		assert_int_equal(queue_entry(it, struct data, q)->id, expect[n++]);
	assert_int_equal(n, 3);
}

/* ---- delsafe iteration draining the whole queue -------------------------- */

static void
test_walk_delsafe(void **state)
{
	(void)state;
	DEFINE_QUEUE(q);
	struct data d[6];
	for (unsigned i = 0; i < 6; i++) {
		d[i].id = i;
		queue_add_head(&q, &d[i].q);
	}

	unsigned n = 0;
	struct qnode *it, *tmp;
	queue_walk_delsafe(&q, it, tmp) { queue_del(it); n++; }
	assert_int_equal(n, 6);
	assert_true(queue_empty(&q));

	/* typed delsafe variant */
	for (unsigned i = 0; i < 6; i++)
		queue_add_head(&q, &d[i].q);
	n = 0;
	queue_for_each_delsafe(&q, x, struct data, q) { queue_del(&x->q); n++; }
	assert_int_equal(n, 6);
	assert_true(queue_empty(&q));
}

/* ---- queue_move splices a whole queue onto another head ------------------ */

static void
test_move(void **state)
{
	(void)state;
	DEFINE_QUEUE(src);
	DEFINE_QUEUE(dst);
	struct data d[3] = { { .id = 0 }, { .id = 1 }, { .id = 2 } };
	for (unsigned i = 0; i < 3; i++)
		queue_add_head(&src, &d[i].q);

	queue_move(&dst, &src);
	assert_true(queue_empty(&src));
	assert_false(queue_empty(&dst));

	unsigned n = 0, expect[3] = { 2, 1, 0 };
	struct qnode *it;
	queue_walk(&dst, it)
		assert_int_equal(queue_entry(it, struct data, q)->id, expect[n++]);
	assert_int_equal(n, 3);

	/* removal through the moved-in prev links stays consistent */
	queue_del(&d[2].q);
	n = 0;
	queue_walk(&dst, it) n++;
	assert_int_equal(n, 2);
}

int
main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_init_empty),
		cmocka_unit_test(test_add_head_order),
		cmocka_unit_test(test_del_middle),
		cmocka_unit_test(test_add_before_behind),
		cmocka_unit_test(test_walk_delsafe),
		cmocka_unit_test(test_move),
	};
	return cmocka_run_group_tests_name("queue", tests, NULL, NULL);
}
