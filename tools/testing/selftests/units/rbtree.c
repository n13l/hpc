/*
 * Unit tests for the intrusive red-black tree <hpc/rbtree.h> - the house-style
 * intrusive counterpart to the <hpc/rb.h> macro tree. Ordering is supplied by
 * the test: a small BST descent feeds
 * rbtree_link_node()/rbtree_insert_color(), mirroring the kernel rbtree split.
 *
 * Besides checking element order, each mutation is followed by a full structural
 * audit (validate, from rbtree_util.h): BST order, parent back-links, no red-red
 * edge, a black root and equal black-height on every path.
 *
 * The plain spelling only. The lockless one is a separate unit, rbtree_rcu.c,
 * built only when CONFIG_RCU is enabled - it needs liburcu linked, and there is
 * nothing to assert about it in a build where it does not exist.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>

#include <hpc/compiler.h>
#include <hpc/rbtree.h>

#include "rbtree_util.h"

/* index of the sole ratio metric in a namespace's table */
#define ratio_index(ns, out) do { \
	measure_for_each(ns, _i) \
		if (measure_kind(ns, _i) == MEASURE_RATIO) { (out) = _i; break; } \
} while (0)

/* ---- test-owned ordering: BST descent + link + rebalance ----------------- */

static bool
tree_insert(struct rbtree *t, struct data *d)
{
	struct rbnode *parent, **link = find_slot(t, d->id, &parent);

	if (*link)
		return false;                  /* already present */
	rbtree_link_node(&d->rb, parent, link);
	rbtree_insert_color(t, &d->rb);
	return true;
}

/* in-order walk must yield strictly ascending ids; returns the count */
static unsigned
assert_sorted(struct rbtree *t)
{
	unsigned n = 0;
	long prev = -1;
	struct rbnode *it;

	rbtree_walk(t, it) {
		struct data *d = rbtree_entry(it, struct data, rb);
		assert_true((long)d->id > prev);
		prev = d->id;
		n++;
	}
	return n;
}

/* ---- init / empty -------------------------------------------------------- */

static void
test_init_empty(void **state)
{
	(void)state;
	DEFINE_RBTREE(t);
	assert_true(rbtree_empty(&t));
	assert_null(rbtree_first(&t));
	assert_null(rbtree_last(&t));

	struct data d = { .id = 1 };
	rbnode_init(&d.rb);
	assert_true(rbnode_unlinked(&d.rb));
	assert_false(rbnode_linked(&d.rb));
}

/* ---- insertion keeps the tree ordered and balanced ----------------------- */

static void
test_insert_order(void **state)
{
	(void)state;
	DEFINE_RBTREE(t);
	/* deliberately scrambled, spanning both rotation directions */
	unsigned ids[] = { 50, 20, 70, 10, 30, 60, 80, 5, 25, 65, 90, 1, 55 };
	enum { N = sizeof(ids) / sizeof(ids[0]) };
	struct data d[N];

	for (unsigned i = 0; i < N; i++) {
		d[i].id = ids[i];
		assert_true(tree_insert(&t, &d[i]));
		validate(&t);
	}
	assert_false(rbtree_empty(&t));
	assert_int_equal(assert_sorted(&t), N);

	/* duplicate keys are rejected, tree unchanged */
	struct data dup = { .id = 50 };
	assert_false(tree_insert(&t, &dup));
	assert_int_equal(assert_sorted(&t), N);

	/* every key is findable; a missing one is not */
	for (unsigned i = 0; i < N; i++)
		assert_ptr_equal(tree_find(&t, ids[i]), &d[i]);
	assert_null(tree_find(&t, 999));

	/* min / max */
	struct rbnode *lo = rbtree_first(&t), *hi = rbtree_last(&t);
	assert_non_null(lo);
	assert_non_null(hi);
	assert_int_equal(rbtree_entry(lo, struct data, rb)->id, 1);
	assert_int_equal(rbtree_entry(hi, struct data, rb)->id, 90);
}

/* ---- next / prev bracket the ordered sequence ---------------------------- */

static void
test_next_prev(void **state)
{
	(void)state;
	DEFINE_RBTREE(t);
	struct data d[5];
	for (unsigned i = 0; i < 5; i++) {
		d[i].id = (i + 1) * 10;        /* 10,20,30,40,50 */
		tree_insert(&t, &d[i]);
	}

	/* forward via next */
	struct rbnode *n = rbtree_first(&t);
	unsigned fwd[5] = { 10, 20, 30, 40, 50 }, i = 0;
	for (; n; n = rbtree_next(n))
		assert_int_equal(rbtree_entry(n, struct data, rb)->id, fwd[i++]);
	assert_int_equal(i, 5);

	/* backward via prev / reverse walk */
	unsigned rev[5] = { 50, 40, 30, 20, 10 };
	i = 0;
	rbtree_walk_reverse(&t, n)
		assert_int_equal(rbtree_entry(n, struct data, rb)->id, rev[i++]);
	assert_int_equal(i, 5);

	/* typed forward / reverse iteration must agree */
	i = 0;
	rbtree_for_each(&t, x, struct data, rb)
		assert_int_equal(x->id, fwd[i++]);
	assert_int_equal(i, 5);
	i = 0;
	rbtree_for_each_reverse(&t, x, struct data, rb)
		assert_int_equal(x->id, rev[i++]);
	assert_int_equal(i, 5);
}

/* ---- erase every shape: leaf, one child, two children, root -------------- */

static void
test_erase(void **state)
{
	(void)state;
	DEFINE_RBTREE(t);
	unsigned ids[] = { 50, 20, 70, 10, 30, 60, 80, 5, 25, 65, 90, 1, 55 };
	enum { N = sizeof(ids) / sizeof(ids[0]) };
	struct data d[N];
	for (unsigned i = 0; i < N; i++) {
		d[i].id = ids[i];
		tree_insert(&t, &d[i]);
	}

	/* remove in a different scrambled order, auditing after each */
	unsigned order[] = { 70, 5, 50, 90, 20, 1, 65, 30, 80, 25, 60, 55, 10 };
	unsigned left = N;
	for (unsigned i = 0; i < N; i++) {
		struct data *victim = tree_find(&t, order[i]);
		assert_non_null(victim);
		rbtree_erase(&t, &victim->rb);
		left--;
		validate(&t);
		assert_int_equal(assert_sorted(&t), left);
		assert_null(tree_find(&t, order[i]));
	}
	assert_true(rbtree_empty(&t));

	/* erase_init leaves the node reusable and reporting unlinked */
	struct data solo = { .id = 42 };
	tree_insert(&t, &solo);
	rbtree_erase_init(&t, &solo.rb);
	assert_true(rbnode_unlinked(&solo.rb));
	assert_true(rbtree_empty(&t));
	/* reusable: it inserts cleanly again */
	assert_true(tree_insert(&t, &solo));
	validate(&t);
}

/* ---- replace swaps a node for an equal-keyed stand-in -------------------- */

static void
test_replace(void **state)
{
	(void)state;
	DEFINE_RBTREE(t);
	struct data d[5];
	for (unsigned i = 0; i < 5; i++) {
		d[i].id = (i + 1) * 10;
		tree_insert(&t, &d[i]);
	}

	struct data repl = { .id = 30 };
	struct data *old = tree_find(&t, 30);
	rbtree_replace(&t, &old->rb, &repl.rb);
	validate(&t);
	assert_ptr_equal(tree_find(&t, 30), &repl);
	assert_int_equal(assert_sorted(&t), 5);
}

/* ---- delsafe iteration draining the whole tree --------------------------- */

static void
test_walk_delsafe(void **state)
{
	(void)state;
	DEFINE_RBTREE(t);
	struct data d[8];
	for (unsigned i = 0; i < 8; i++) {
		d[i].id = i;
		tree_insert(&t, &d[i]);
	}

	unsigned n = 0;
	struct rbnode *it, *tmp;
	rbtree_walk_delsafe(&t, it, tmp) { rbtree_erase(&t, it); n++; }
	assert_int_equal(n, 8);
	assert_true(rbtree_empty(&t));

	/* typed delsafe variant */
	for (unsigned i = 0; i < 8; i++)
		tree_insert(&t, &d[i]);
	n = 0;
	rbtree_for_each_delsafe(&t, x, struct data, rb) {
		rbtree_erase(&t, &x->rb);
		n++;
	}
	assert_int_equal(n, 8);
	assert_true(rbtree_empty(&t));
}

/* ---- measurement: counters, gauge and ratio ----------------------------- *
 * The metric table, ratio arithmetic and aggregation operate on a caller-owned
 * struct, so those run unconditionally. Live counting needs the storage pointer,
 * compiled in only under CONFIG_MEASURE, so it skips when that is off.
 */

static void
test_measure_table(void **state)
{
	(void)state;

	assert_int_equal(measure_count(rbtree), 5);  /* 3 counter+1 gauge+1 ratio */
	assert_int_equal(measure_nfield(rbtree), 4); /* ratio carries no storage */

	/*
	 * Names and descriptions are metadata, emitted only under
	 * CONFIG_MEASURE; without it the table keeps its shape and every string
	 * reads empty (see <hpc/measure.h>).
	 */
	const char *expect[] = { "insert", "erase", "rotate",
	                         "entries", "rebalance" };
	measure_for_each(rbtree, i) {
#ifdef CONFIG_MEASURE
		assert_string_equal(measure_name(rbtree, i), expect[i]);
#else
		assert_string_equal(measure_name(rbtree, i), "");
#endif
		assert_non_null(measure_desc(rbtree, i));
	}
	(void)expect;

	/* counters first, then the gauge, then the ratio last */
	unsigned nc = 0, ng = 0, nr = 0;
	measure_for_each_counter(rbtree, i) nc++;
	measure_for_each_gauge(rbtree, i)   ng++;
	measure_for_each_ratio(rbtree, i)   nr++;
	assert_int_equal(nc, 3);
	assert_int_equal(ng, 1);
	assert_int_equal(nr, 1);
	assert_int_equal(measure_kind(rbtree, 3), MEASURE_GAUGE);
	assert_int_equal(measure_kind(rbtree, 4), MEASURE_RATIO);
}

static void
test_measure_ratio(void **state)
{
	(void)state;

	struct rbtree_measure m = { 0 };
	m.rotate = 3;
	m.insert = 4;

	/* direct helper and the divide-by-zero guard */
	assert_int_equal(measure_ratio(m.rotate, m.insert), 75);
	assert_int_equal(measure_ratio(m.rotate, 0), 0);

	/* the ratio metric read generically recomputes from the two fields */
	unsigned r = 0;
	ratio_index(rbtree, r);
	assert_int_equal(measure_at(rbtree, &m, r), 75);

	/* a stored field read generically returns the field itself (rotate=idx 2) */
	assert_int_equal(measure_at(rbtree, &m, 2), 3);
}

/* aggregation: per-tree structs summed to a global, ratio recomputed */
static void
test_measure_aggregate(void **state)
{
	(void)state;

	struct rbtree_measure a = { 0 }, b = { 0 }, global = { 0 };
	a.insert = 10; a.rotate = 4; a.entries = 10;   /* tree a: 40% rebalance */
	b.insert = 30; b.rotate = 2; b.entries = 30;   /* tree b: ~6% rebalance */

	measure_aggregate(rbtree, &global, &a);
	measure_aggregate(rbtree, &global, &b);

	/* counters and the gauge add */
	assert_int_equal(global.insert, 40);
	assert_int_equal(global.rotate, 6);
	assert_int_equal(global.entries, 40);

	/* global rebalance is sum(rotate)*100/sum(insert)=15%, NOT (40+6)/2 */
	unsigned r = 0;
	ratio_index(rbtree, r);
	assert_int_equal(measure_at(rbtree, &global, r), 15);
	assert_int_equal(measure_ratio(global.rotate, global.insert), 15);
}

/* ---- live counting (needs the storage pointer -> CONFIG_MEASURE) --------- */

#ifdef CONFIG_MEASURE

static void
test_measure_live(void **state)
{
	(void)state;
	DEFINE_RBTREE(t);
	struct rbtree_measure m = { 0 };
	rbtree_measure_attach(&t, &m);

	unsigned ids[] = { 50, 20, 70, 10, 30, 60, 80, 5, 25, 65, 90, 1, 55 };
	enum { N = sizeof(ids) / sizeof(ids[0]) };
	struct data d[N];
	for (unsigned i = 0; i < N; i++) {
		d[i].id = ids[i];
		assert_true(tree_insert(&t, &d[i]));
	}

	assert_int_equal(m.insert, N);           /* one per insert_color */
	assert_int_equal(m.entries, N);          /* gauge up */
	assert_true(m.rotate > 0);               /* rebalancing did happen */

	/* a rejected duplicate is not linked, so it is not counted */
	struct data dup = { .id = 50 };
	assert_false(tree_insert(&t, &dup));
	assert_int_equal(m.insert, N);

	/* erase drops the gauge and bumps the erase counter */
	rbtree_erase(&t, &tree_find(&t, 50)->rb);
	rbtree_erase_init(&t, &tree_find(&t, 20)->rb);
	assert_int_equal(m.erase, 2);
	assert_int_equal(m.entries, N - 2);      /* gauge down */

	/* the rebalance ratio recomputes from the live fields */
	unsigned r = 0;
	ratio_index(rbtree, r);
	assert_int_equal(measure_at(rbtree, &m, r),
	                 measure_ratio(m.rotate, m.insert));

	/* stored fields are also reachable by name */
	assert_ptr_equal(measure_of(rbtree, &m, "insert"), &m.insert);
	assert_int_equal(*measure_of(rbtree, &m, "rotate"), m.rotate);
	assert_null(measure_of(rbtree, &m, "rebalance"));  /* ratio: no storage */
}

/* a NULL measure counts nothing and never crashes */
static void
test_measure_none(void **state)
{
	(void)state;
	DEFINE_RBTREE(t);
	assert_null(t.measure);                  /* DEFINE_RBTREE zero-inits it */

	struct data a = { .id = 1 }, b = { .id = 2 };
	tree_insert(&t, &a);
	tree_insert(&t, &b);
	rbtree_erase(&t, &a.rb);
	assert_false(rbtree_empty(&t));
}

#else /* !CONFIG_MEASURE */

static void test_measure_live(void **s) { (void)s; skip(); }
static void test_measure_none(void **s) { (void)s; skip(); }

#endif

int
main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_init_empty),
		cmocka_unit_test(test_insert_order),
		cmocka_unit_test(test_next_prev),
		cmocka_unit_test(test_erase),
		cmocka_unit_test(test_replace),
		cmocka_unit_test(test_walk_delsafe),
		cmocka_unit_test(test_measure_table),
		cmocka_unit_test(test_measure_ratio),
		cmocka_unit_test(test_measure_aggregate),
		cmocka_unit_test(test_measure_live),
		cmocka_unit_test(test_measure_none),
	};
	return cmocka_run_group_tests_name("rbtree", tests, NULL, NULL);
}
