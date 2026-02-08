/*
 * Stress test: an expiring hash cache under lockless readers.
 *
 * The intrusive hash table <hpc/hash/table.h> in its RCU spelling, holding
 * objects that *are* blocks of an expiring block cache <mem/slab_cache.h>, with
 * four reader threads walking chains while one writer inserts, expires objects,
 * retires them through grace periods and lets the slab hand memory back to the
 * OS underneath all of it. The scaffolding - the poisoned payload, the retire
 * batching, the metrics and the report - is shared with rbtree_rcu_stress.c and
 * documented in stress_util.h; what is specific to a hash cache is here:
 *
 *   - Expiry follows the *slot*, not a timer. Every operation walks the chain its
 *     key belongs to, and that walk - which a lookup owes anyway - is what
 *     notices deadlines, unlinks what has died and hands the blocks back. An
 *     object on a chain nobody touches keeps its block, however long it has been
 *     dead. hashtable_cache.c makes that a set of exact single-threaded
 *     assertions; here it runs against readers and a moving allocator.
 *
 *   - Under pressure the chain sweep is the only source of memory, and under RCU
 *     it is not enough on its own: the blocks it frees are not free until a grace
 *     period has passed. So the pressure valve is sweep, wait, retry - and the
 *     wait is the price of the lockless read side.
 *
 *   - There is no promotion. hash_ruc() moves a node with the plain queue
 *     primitives (a delete followed by an insert, neither of them published), so
 *     it has no place in a table readers are walking without a lock; an RCU chain
 *     cache either forgoes recency or re-publishes a fresh node. This one forgoes
 *     it, and expiry is what bounds the chains.
 *
 * This unit exists only when CONFIG_RCU is enabled (see the Kbuild), so nothing
 * here is conditional.
 */

#include "stress_util.h"

#include <hpc/hash/table.h>

#define CACHE_BITS  8                      /* 256 slots */
#define CACHE_SLOTS (1u << CACHE_BITS)

/*
 * The cached object: one slab block. The head carries the stamp the integrity
 * check reads, the qnode links it into its slot, and everything from
 * STRESS_POISON_OFF to the end of the block is poison.
 */
struct object {
	struct stress_head h;
	struct qnode q;
};

_Static_assert(sizeof(struct object) <= STRESS_POISON_OFF,
	"the object head and its link must sit ahead of the poison");
_Static_assert(STRESS_KEYS >= CACHE_SLOTS,
	"every slot should be reachable from the key space");

struct cache {
	DECLARE_HASHTABLE(table, CACHE_BITS);
	struct stress_arena arena;
	u32 linked;                        /* writer's own count of live nodes */
	u32 gen;                           /* incarnation stamped on the next  */
};

/* Keys map to slots by their low bits, so a chain is a known set of keys. */
static unsigned
cache_slot(u32 key)
{
	return hash_seq(key, CACHE_BITS);
}

/* Per-object deadlines, spread over the key space so both paths fire: every key
 * has a TTL, the odd ones also have an idle timeout. */
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

/*
 * Sweep one slot, reclaiming what has died, and report whether @key is on it.
 *
 * This is the only place expiry happens. Unlinking publishes with
 * hash_del_rcu(), which leaves the node's ->next intact so a reader parked on it
 * walks on into the rest of the chain, and the block goes onto the retire batch
 * rather than back to the slab - it is still reachable until a grace period says
 * otherwise.
 */
static struct object *
cache_sweep(struct cache *c, unsigned slot, u32 key, timestamp_t now)
{
	struct object *hit = NULL;

	c->arena.m.sweeps++;
	hash_for_each_delsafe(c->table, slot, it, struct object, q) {
		if (slab_cache_expired(&c->arena.cache, it, now)) {
			hash_del_rcu(&it->q);
			c->linked--;
			c->arena.m.expiries++;
			stress_retire(&c->arena, it);
			continue;
		}
		if (it->h.key == key)
			hit = it;
	}
	return hit;
}

/* Sweep every slot: the whole-table pass a drain phase needs. */
static void
cache_sweep_all(void *ctx, timestamp_t now)
{
	struct cache *c = (struct cache *)ctx;
	unsigned slot;

	for (slot = 0; slot < CACHE_SLOTS; slot++)
		cache_sweep(c, slot, STRESS_KEYS, now);   /* a key nobody has */
}

/* Count what is linked, independently of the writer's own bookkeeping. */
static u32
cache_count(struct cache *c)
{
	unsigned slot;
	u32 n = 0;

	for (slot = 0; slot < CACHE_SLOTS; slot++)
		hash_for_each(c->table, slot, it, struct object, q)
			n++;
	return n;
}

/*
 * One writer operation on @key: sweep its slot, then either touch what is there
 * or install a new object.
 *
 * The allocation is where pressure shows up, and the retry path is the point:
 * sweeping more slots finds more dead objects, but under RCU their blocks only
 * come back once a grace period has elapsed, so the valve has to drain the
 * retire batch before the retry can succeed.
 */
static void
cache_op(void *ctx, u32 key, timestamp_t now)
{
	struct cache *c = (struct cache *)ctx;
	unsigned slot = cache_slot(key);
	struct object *o;

	c->arena.m.ops++;
	o = cache_sweep(c, slot, key, now);
	if (o) {
		/* the sweep skipped everything expired at @now, so the touch
		 * cannot lose to a deadline: it restarts the idle window */
		assert_true(slab_cache_touch(&c->arena.cache, o, now));
		c->arena.m.touches++;
		return;
	}

	o = stress_alloc(&c->arena, now, key, c->gen, cache_ttl(key),
	                 cache_idle(key));
	if (!o) {
		unsigned i;
		for (i = 1; i <= 8; i++)
			cache_sweep(c, (slot + i) % CACHE_SLOTS, STRESS_KEYS,
			            now);
		stress_drain(&c->arena);
		o = stress_alloc(&c->arena, now, key, c->gen, cache_ttl(key),
		                 cache_idle(key));
		if (!o)
			return;                /* genuinely full of live objects */
	}
	c->gen++;
	hash_add_rcu(c->table, &o->q, slot);
	c->linked++;
}

/* ---- the readers --------------------------------------------------------- */

/*
 * Walk the chain @key belongs to inside one read-side section, checking every
 * object it steps on and reporting whether the key was there.
 *
 * The walk does not stop at the key: a chain is the unit of work here, exactly
 * as it is for the writer's sweep, and finishing it is what puts a reader on the
 * nodes the writer is unlinking. The step cap is insurance, not policy - a chain
 * cannot legitimately exceed the key space - and an abort is counted rather than
 * fatal.
 */
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
			unsigned slot = cache_slot(key), steps = 0;
			struct object *held = NULL;
			bool hit = false;

			r->m.lookups++;
			r->m.sections++;
			rcu_read_lock();
			hash_for_each_rcu(c->table, slot, it, struct object, q) {
				if (++steps > STRESS_STEPS) {
					r->m.aborts++;
					break;
				}
				r->m.visits++;
				if (!stress_reader_check(r, it))
					break;
				held = it;
				if (it->h.key == key)
					hit = true;
			}
			/* every so often, keep the last node of the chain and look
			 * at it again after a wait - still in the same section */
			if (stress_reader_should_dwell(r))
				stress_reader_dwell(r, held);
			rcu_read_unlock();
			if (hit)
				r->m.hits++;
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

/* A drain phase empties the table, so the next wave starts from nothing. */
static void
cache_wave_end(void *ctx)
{
	struct cache *c = (struct cache *)ctx;

	assert_int_equal(c->linked, 0);
	assert_int_equal(cache_count(c), 0);
}

static void
test_stress_hash_cache_under_readers(void **state)
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
	u32 counted;
	/*
	 * Where a hash cache reclaims is the whole point of it: on the chain the
	 * traffic was walking anyway. So there is no per-round whole-table pass -
	 * the sweep is only what a drain phase needs.
	 */
	const struct stress_driver drv = {
		.ctx = &c,
		.op = cache_op,
		.reap = cache_sweep_all,
		.wave = cache_wave_end,
		.reap_each_round = false,
	};

	memset(&c, 0, sizeof(c));
	memset(&rm, 0, sizeof(rm));
	hash_init_table(c.table, CACHE_BITS);
	assert_int_equal(stress_arena_init(a, STRESS_BLOCK, &pol,
	                                  STRESS_TTL_MS, 0), 0);
	clock_gettime(CLOCK_MONOTONIC, &t0);

	stress_readers_start(th, readers, STRESS_READERS, cache_reader, a, &c,
	                     &gate, &stop);

	now = stress_run(a, &drv, STRESS_WAVES * stress_scale(), now);

	stress_readers_join(th, readers, STRESS_READERS, &stop, &gate, &rm);

	/*
	 * The readers are gone. A wave ends on an empty table, so refill it
	 * single-threaded before the settled check: the invariant has to hold with
	 * objects in it, not only when everything has been swept away.
	 *
	 * The count comes from walking the table, not from the allocator, so the
	 * two have to agree with each other and with the slab.
	 */
	now += STRESS_TICK_MS;
	for (op = 0; op < STRESS_OPS; op++)
		cache_op(&c, op, now);
	stress_drain(a);

	/* the report comes first, so a run that fails an assertion below still
	 * says what it was doing when it did */
	counted = cache_count(&c);
	a->m.linked = counted;
	stress_report("hashtable_rcu", a, &rm, STRESS_READERS,
	              stress_ms_since(&t0));

	assert_int_equal(counted, c.linked);
	assert_true(counted > 0);
	stress_assert_settled(a, counted);
	stress_assert_exercised(a);

	/*
	 * Purge: with the table empty and no reader anywhere, the slab must give
	 * every block back down to the policy floor - which is the grain-rounded
	 * minimum, not the number that was asked for.
	 */
	now += STRESS_TTL_MS * 4;
	cache_sweep_all(&c, now);
	stress_drain(a);
	for (i = 0; i < 32 && stress_gc(a, now) < 0; i++)
		;
	assert_int_equal(cache_count(&c), 0);
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
		cmocka_unit_test(test_stress_hash_cache_under_readers),
	};
	return cmocka_run_group_tests_name("hashtable_rcu_stress", tests,
	                                   NULL, NULL);
}
