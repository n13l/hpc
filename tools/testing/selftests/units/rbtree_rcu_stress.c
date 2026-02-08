/*
 * Stress test: an expiring ordered cache under lockless readers.
 *
 * The intrusive red-black tree <hpc/rbtree.h> in its RCU spelling, holding
 * objects that *are* blocks of an expiring block cache <mem/slab_cache.h>, with
 * four reader threads descending and scanning the tree while one writer inserts,
 * expires objects, retires them through grace periods and lets the slab hand
 * memory back to the OS underneath all of it. The scaffolding - the poisoned
 * payload, the retire batching, the metrics and the report - is shared with
 * hashtable_rcu_stress.c and documented in stress_util.h. What an ordered
 * container changes:
 *
 *   - Expiry has nowhere local to happen. A hash cache reclaims on the chain a
 *     lookup was walking anyway, and pays for expiry with traffic; a tree has no
 *     such slot - the nodes that share a key's neighbourhood are its neighbours
 *     in *order*, not in lifetime. So a deadline is noticed on the one node a
 *     descent lands on, and everything else waits for a sweep: one in-order pass
 *     over the whole tree, run once a round and as the pressure valve. That cost
 *     is the honest price of the ordering, and it is why the writer metrics count
 *     sweeps separately.
 *
 *   - The tree publishes less than the table does. rbtree_link_node_rcu() and
 *     rbtree_replace_rcu() are atomic for readers, but rbtree_insert_color() and
 *     rbtree_erase() rebalance by mutating several existing nodes with plain
 *     stores. A reader racing a rebalance may therefore miss a node that is in
 *     the tree, see one twice, or walk a shape that never existed - all of which
 *     this test tolerates on purpose. What it does not tolerate is a reader
 *     reading memory that is not a valid object: every node a traversal steps on
 *     is checked, and the reader-side step caps exist because a rebalance can
 *     briefly make a traversal cyclic. An abort is counted, not fatal; a failed
 *     integrity check fails the run.
 *
 *   - Between rounds, with no mutation in flight, the writer audits the red-black
 *     invariants outright: parent links, no red node with a red child, equal
 *     black height on every path, and BST order. Readers only read, so the audit
 *     is exact even while they run.
 *
 * This unit exists only when CONFIG_RCU is enabled (see the Kbuild), so nothing
 * here is conditional.
 */

#include "stress_util.h"

#include <hpc/rbtree.h>

/* Nodes a reader scans in one in-order pass; a full pass over a large tree would
 * hold a read-side section open long enough to stall the writer's next grace
 * period, which is not what this is measuring. */
#define SCAN_STEPS 64u

struct object {
	struct stress_head h;
	struct rbnode rb;
};

_Static_assert(sizeof(struct object) <= STRESS_POISON_OFF,
	"the object head and its node must sit ahead of the poison");

struct cache {
	struct rbtree tree;
	struct stress_arena arena;
	u32 linked;                        /* writer's own count of live nodes */
	u32 gen;                           /* incarnation stamped on the next  */
};

static u32
cache_ttl(u32 key)
{
	return STRESS_TTL_MS + (key % 4u) * 50u;
}

static u32
cache_idle(u32 key)
{
	return (key & 1u) ? STRESS_IDLE_MS : 0u;
}

/* ---- writer-side ordering ------------------------------------------------ */

/* Descend to the slot @key belongs in, reporting the node that will parent it. */
static struct rbnode **
tree_slot(struct rbtree *t, u32 key, struct rbnode **parent_out)
{
	struct rbnode **link = &t->root, *parent = NULL;

	while (*link) {
		struct object *o = rbtree_entry(*link, struct object, rb);
		parent = *link;
		if (key < o->h.key)
			link = &(*link)->left;
		else if (key > o->h.key)
			link = &(*link)->right;
		else
			break;                 /* already present */
	}
	*parent_out = parent;
	return link;
}

static struct object *
tree_find(struct rbtree *t, u32 key)
{
	struct rbnode *parent, **link = tree_slot(t, key, &parent);

	return *link ? rbtree_entry(*link, struct object, rb) : NULL;
}

/* ---- the red-black audit ------------------------------------------------- */

static unsigned
tree_audit(struct rbnode *n, struct rbnode *parent, u32 *count)
{
	unsigned lh, rh;

	if (!n)
		return 1;                      /* NULL leaves count black */
	assert_ptr_equal(n->parent, parent);
	(*count)++;
	if (n->color == RBTREE_RED) {
		assert_true(!n->left  || n->left->color  == RBTREE_BLACK);
		assert_true(!n->right || n->right->color == RBTREE_BLACK);
	}
	if (n->left)
		assert_true(rbtree_entry(n->left, struct object, rb)->h.key <
		            rbtree_entry(n, struct object, rb)->h.key);
	if (n->right)
		assert_true(rbtree_entry(n->right, struct object, rb)->h.key >
		            rbtree_entry(n, struct object, rb)->h.key);
	lh = tree_audit(n->left, n, count);
	rh = tree_audit(n->right, n, count);
	assert_int_equal(lh, rh);
	return lh + (n->color == RBTREE_BLACK ? 1u : 0u);
}

/*
 * Audit the tree and cross-check its size against the writer's own count. The
 * count is what makes this more than a shape check: it is derived by walking the
 * structure, so it catches a node that was allocated and never linked, or
 * unlinked and never accounted.
 */
static void
tree_validate(void *ctx)
{
	struct cache *c = (struct cache *)ctx;
	u32 count = 0;

	if (c->tree.root) {
		assert_int_equal(c->tree.root->color, RBTREE_BLACK);
		assert_null(c->tree.root->parent);
		tree_audit(c->tree.root, NULL, &count);
	}
	assert_int_equal(count, c->linked);
}

/* ---- expiry -------------------------------------------------------------- */

/*
 * One in-order pass, erasing whatever has died, repeated until a pass finds
 * nothing.
 *
 * The repetition is not paranoia. rbtree_walk_delsafe caches the in-order
 * successor before the body runs, and an erase with two children splices the
 * successor into the victim's slot - so the cached node is still a valid node of
 * the tree, but the traversal that follows it can skip nodes it has not seen yet.
 * A pass that erases nothing, on the other hand, mutates nothing, so it is a
 * complete traversal: once one of those has run, no expired node is left.
 */
static u32
tree_sweep_ex(struct cache *c, timestamp_t now)
{
	u32 total = 0, erased;

	do {
		struct rbnode *it, *tmp;

		erased = 0;
		c->arena.m.sweeps++;
		rbtree_walk_delsafe(&c->tree, it, tmp) {
			struct object *o = rbtree_entry(it, struct object, rb);
			if (!slab_cache_expired(&c->arena.cache, o, now))
				continue;
			rbtree_erase(&c->tree, it);
			c->linked--;
			c->arena.m.expiries++;
			stress_retire(&c->arena, o);
			erased++;
		}
		total += erased;
	} while (erased);
	return total;
}

static void
tree_sweep(void *ctx, timestamp_t now)
{
	tree_sweep_ex((struct cache *)ctx, now);
}

/*
 * One writer operation on @key: a descent that either touches a live object,
 * notices a dead one, or installs a fresh incarnation.
 *
 * Every mutation invalidates the slot a descent found, so the reclaiming happens
 * first and the insertion descends again. The retry path is the pressure valve,
 * and under RCU it has to pay a grace period: the blocks a sweep frees are not
 * available until the readers that could still be standing on them are gone.
 */
static void
cache_op(void *ctx, u32 key, timestamp_t now)
{
	struct cache *c = (struct cache *)ctx;
	struct rbnode *parent, **link;
	struct object *o;

	c->arena.m.ops++;
	o = tree_find(&c->tree, key);
	if (o) {
		if (!slab_cache_expired(&c->arena.cache, o, now)) {
			/* just tested against the same @now, so the touch cannot
			 * lose to a deadline: it restarts the idle window */
			assert_true(slab_cache_touch(&c->arena.cache, o, now));
			c->arena.m.touches++;
			return;
		}
		rbtree_erase(&c->tree, &o->rb);
		c->linked--;
		c->arena.m.expiries++;
		stress_retire(&c->arena, o);
	}

	o = stress_alloc(&c->arena, now, key, c->gen, cache_ttl(key),
	                 cache_idle(key));
	if (!o) {
		tree_sweep_ex(c, now);
		stress_drain(&c->arena);
		o = stress_alloc(&c->arena, now, key, c->gen, cache_ttl(key),
		                 cache_idle(key));
		if (!o)
			return;                /* genuinely full of live objects */
	}
	c->gen++;
	link = tree_slot(&c->tree, key, &parent);
	assert_null(*link);                    /* the key is gone or never was */
	rbtree_link_node_rcu(&o->rb, parent, link);
	rbtree_insert_color(&c->tree, &o->rb);
	c->linked++;
}

/* ---- the readers --------------------------------------------------------- */

/*
 * A lockless descent. Child links are read with rcu_dereference(), so a node
 * published by rbtree_link_node_rcu() is either fully visible or not visible at
 * all; a node the writer is rotating past may send the descent the wrong way,
 * which costs a miss and nothing else.
 */
static struct object *
tree_lookup_rcu(struct rbtree *t, u32 key, struct stress_reader *r,
                struct object **held)
{
	struct rbnode *n = rcu_dereference(t->root);
	unsigned steps = 0;

	while (n) {
		struct object *o = rbtree_entry(n, struct object, rb);
		u32 k;

		if (++steps > STRESS_STEPS) {
			r->m.aborts++;
			return NULL;
		}
		r->m.visits++;
		if (!stress_reader_check(r, o))
			return NULL;
		*held = o;
		k = o->h.key;
		if (key < k)
			n = rcu_dereference(n->left);
		else if (key > k)
			n = rcu_dereference(n->right);
		else
			return o;
	}
	return NULL;
}

/*
 * A lockless in-order scan of at most SCAN_STEPS nodes. Ordering is deliberately
 * not asserted: a reader racing a rebalance is allowed to see a shape that never
 * existed (see the header comment). What every node it lands on must be is a
 * valid object.
 *
 * Stopping at SCAN_STEPS is this traversal's normal end - the tree is routinely
 * larger than that - so it is not counted as an abort. The cap is also what makes
 * a rebalance that briefly turns the walk cyclic harmless here; a cycle in the
 * *descent*, which cannot be explained that way, is what @aborts counts.
 */
static void
tree_scan_rcu(struct rbtree *t, struct stress_reader *r, struct object **held)
{
	struct rbnode *it;
	unsigned steps = 0;

	r->m.scans++;
	rbtree_walk_rcu(t, it) {
		struct object *o = rbtree_entry(it, struct object, rb);
		if (++steps > SCAN_STEPS)
			return;
		r->m.visits++;
		if (!stress_reader_check(r, o))
			return;
		*held = o;
	}
}

static void *
cache_reader(void *arg)
{
	struct stress_reader *r = (struct stress_reader *)arg;
	struct cache *c = (struct cache *)r->container;

	stress_reader_register();
	stress_gate_arrive(r->gate);

	while (!CMM_LOAD_SHARED(*r->stop)) {
		unsigned n;
		for (n = 0; n < 64; n++) {
			u32 key = stress_rand(&r->seed) % STRESS_KEYS;
			struct object *held = NULL;

			r->m.sections++;
			if ((n & 7u) == 0) {
				rcu_read_lock();
				tree_scan_rcu(&c->tree, r, &held);
				if (stress_reader_should_dwell(r))
					stress_reader_dwell(r, held);
				rcu_read_unlock();
				continue;
			}
			r->m.lookups++;
			rcu_read_lock();
			if (tree_lookup_rcu(&c->tree, key, r, &held))
				r->m.hits++;
			/* every so often, keep the last node the descent stood on
			 * and look at it again after a wait - same section */
			if (stress_reader_should_dwell(r))
				stress_reader_dwell(r, held);
			rcu_read_unlock();
		}
		stress_quiescent();
	}

	r->m.threads = 1;
	stress_reader_unregister();
	return NULL;
}

/* ---- the detector is not vacuous ----------------------------------------- */

static void
test_integrity_detector(void **state)
{
	(void)state;
	stress_check_detector();
}

/* ---- the run ------------------------------------------------------------- */

/* A drain phase empties the tree, and an empty tree still has to audit. */
static void
cache_wave_end(void *ctx)
{
	struct cache *c = (struct cache *)ctx;

	assert_int_equal(c->linked, 0);
	assert_true(rbtree_empty(&c->tree));
	tree_validate(c);
}

static void
test_stress_tree_cache_under_readers(void **state)
{
	(void)state;
	struct slab_policy pol = {
		.min = STRESS_MIN, .max = STRESS_MAX, .grow_step = STRESS_STEP,
		.grow_usage_pct = 80, .shrink_usage_pct = 25,
		.shrink_release_pct = 50, .shrink_after = 0,
	};
	struct cache c;
	struct stress_arena *a = &c.arena;
	struct stress_reader readers[STRESS_READERS];
	struct reader_measure rm;
	struct stress_gate gate;
	pthread_t th[STRESS_READERS];
	struct timespec t0;
	volatile int stop = 0;
	unsigned op, i;
	timestamp_t now = 0;
	/*
	 * An ordered container has no per-key locality to reclaim from, so the
	 * whole-tree pass runs every round, not only in a drain - and the audit
	 * runs with it, because between rounds nothing is mutating.
	 */
	const struct stress_driver drv = {
		.ctx = &c,
		.op = cache_op,
		.reap = tree_sweep,
		.audit = tree_validate,
		.wave = cache_wave_end,
		.reap_each_round = true,
	};

	memset(&c, 0, sizeof(c));
	memset(&rm, 0, sizeof(rm));
	c.tree = init_rbtree;
	assert_int_equal(stress_arena_init(a, STRESS_BLOCK, &pol,
	                                  STRESS_TTL_MS, 0), 0);
	clock_gettime(CLOCK_MONOTONIC, &t0);

	stress_readers_start(th, readers, STRESS_READERS, cache_reader, a, &c,
	                     &gate, &stop);

	now = stress_run(a, &drv, STRESS_WAVES * stress_scale(), now);

	stress_readers_join(th, readers, STRESS_READERS, &stop, &gate, &rm);

	/*
	 * The readers are gone. The tree is empty at the end of a wave, so refill
	 * it single-threaded and audit that too: the settled state has to hold
	 * with objects in it, not only when everything has been swept away.
	 */
	now += STRESS_TICK_MS;
	for (op = 0; op < STRESS_OPS; op++)
		cache_op(&c, op, now);
	stress_drain(a);

	/* the report comes first, so a run that fails an assertion below still
	 * says what it was doing when it did */
	a->m.linked = c.linked;
	stress_report("rbtree_rcu", a, &rm, STRESS_READERS,
	              stress_ms_since(&t0));

	tree_validate(&c);
	assert_true(c.linked > 0);
	stress_assert_settled(a, c.linked);
	stress_assert_exercised(a);

	/*
	 * Purge: with the tree empty and no reader anywhere, the slab must give
	 * every block back down to the policy floor - which is the grain-rounded
	 * minimum, not the number that was asked for.
	 */
	now += STRESS_TTL_MS * 4;
	tree_sweep(&c, now);
	stress_drain(a);
	for (i = 0; i < 32 && stress_gc(a, now) < 0; i++)
		;
	assert_true(rbtree_empty(&c.tree));
	stress_assert_settled(a, 0);
	assert_int_equal(slab_committed(stress_slab(a)),
	                 slab_policy_min(stress_slab(a)));

	stress_arena_fini(a);
}

int
main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_integrity_detector),
		cmocka_unit_test(test_stress_tree_cache_under_readers),
	};
	return cmocka_run_group_tests_name("rbtree_rcu_stress", tests,
	                                   NULL, NULL);
}
