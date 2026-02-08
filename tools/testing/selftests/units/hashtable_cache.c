/*
 * Unit tests for an expiring hash cache: the intrusive hash table
 * <hpc/hash/table.h> holding objects allocated from the expiring block cache
 * <mem/slab_cache.h>.
 *
 * The two units are combined the way a real cache uses them: the object *is*
 * the slab block (the qnode lives inside the payload, so there is no separate
 * node allocation), and the block's TTL / idle deadline is the object's
 * lifetime.
 *
 * What is under test here is the *expiry policy*, not the two units in
 * isolation (they have units of their own, hashtable.c and slab_cache.c):
 *
 *   Expiry is never decided periodically. No timer, no background thread, and
 *   crucially no slab_cache_reap()/slab_cache_gc() sweep of the whole arena.
 *   A deadline is only ever noticed when the slot holding the object is hit -
 *   a lookup, a miss, or an insert that needs the memory. Probing a slot walks
 *   its chain, unlinks whatever has died and hands those blocks back to the
 *   slab; an object on a slot nobody touches stays linked and keeps its block
 *   for as long as the traffic stays away, however long it has been dead.
 *
 * The cost model that buys: expiry is paid for by the lookup that would have
 * walked the chain anyway (the chain is in cache, the deadline check is two
 * compares per node) and it is proportional to the traffic, not to the size of
 * the table. Nothing scans blocks nobody asked about.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>
#include <string.h>

#include <hpc/compiler.h>
#include <hpc/hash/table.h>
#include <mem/slab_cache.h>

#define CACHE_BITS 4                       /* 16 slots */
#define CACHE_SLOTS (1u << CACHE_BITS)

/*
 * The cached object. It is exactly one slab block: @q links it into its hash
 * slot, and slab_cache keeps its deadlines out-of-band, indexed by block index,
 * so the whole struct stays payload.
 *
 * @gen marks the incarnation of the block, so a test can tell a recycled block
 * from a resurrected object.
 */
struct object {
	u32 key;
	u32 gen;
	struct qnode q;
};

/*
 * One block per grain, so the block counts these tests assert on are the
 * policy's arithmetic and not the grain rounding's: the slab commits and
 * releases whole grains, and a policy asking for 4 blocks of a 32 byte object
 * would be rounded up to a whole page of them (see <mem/slab_vm.h>). The object
 * only uses the head of its block; expiry does not depend on the granularity.
 */
#define OBJECT_BLOCK_SIZE SLAB_GRAIN_BYTES

_Static_assert(sizeof(struct object) <= OBJECT_BLOCK_SIZE,
	"struct object must fit in one grain");

struct cache {
	DECLARE_HASHTABLE(table, CACHE_BITS);
	struct slab_cache blocks;
	u32 evicted;                       /* objects reclaimed on a slot hit */
	u32 probes;                        /* slots walked                    */
};

/* Keys map to slots by their low bits, so collisions are chosen, not hoped for:
 * key, key + 16, key + 32 ... all land on the same chain. */
static inline unsigned
cache_slot(u32 key)
{
	return hash_seq(key, CACHE_BITS);
}

static int
cache_init(struct cache *c, const struct slab_policy *pol, u32 ttl, u32 idle)
{
	memset(c, 0, sizeof(*c));
	hash_init_table(c->table, CACHE_BITS);
	return slab_cache_init(&c->blocks, OBJECT_BLOCK_SIZE, pol, ttl, idle);
}

static void
cache_fini(struct cache *c)
{
	slab_cache_fini(&c->blocks);
}

/*
 * cache_drop - unlink an object and return its block to the slab.
 *
 * The order matters and is the reason the object must leave the chain first:
 * a freed block is immediately overlaid by the slab's free-list node (struct
 * slab_node at offset 0), so the payload - including anything still pointing
 * at it - is gone the moment slab_cache_free() returns.
 */
static void
cache_drop(struct cache *c, struct object *o)
{
	hash_del(&o->q);
	slab_cache_free(&c->blocks, o);
	c->evicted++;
}

/*
 * cache_probe - walk one slot, reclaiming what has died, and look for @key.
 *
 * This is the only place expiry happens. The chain walk a lookup performs
 * anyway doubles as the reclaim pass for that slot: every node is tested
 * against @now, the expired ones are dropped, and a live match is touched
 * (its idle window restarts) and promoted to the head of the chain.
 *
 * A miss sweeps the slot just as thoroughly as a hit - the work is attached to
 * the slot being touched, not to the key being found.
 */
static struct object *
cache_probe(struct cache *c, u32 key, timestamp_t now)
{
	unsigned slot = cache_slot(key);
	struct object *hit = NULL;

	c->probes++;
	hash_for_each_delsafe(c->table, slot, it, struct object, q) {
		if (slab_cache_expired(&c->blocks, it, now)) {
			cache_drop(c, it);
			continue;
		}
		if (it->key == key)
			hit = it;
	}
	if (hit) {
		assert_true(slab_cache_touch(&c->blocks, hit, now));
		hash_ruc(c->table, &hit->q, slot);
	}
	return hit;
}

/*
 * cache_insert - install a new object, with a slot sweep as the pressure valve.
 *
 * When the slab is exhausted there is still no global reap: the slot this key
 * belongs to is swept and the allocation retried. Memory therefore comes back
 * only from the chain being touched - dead objects parked on other slots are
 * not reachable from here and do not help.
 */
static struct object *
cache_insert_ex(struct cache *c, u32 key, u32 gen, timestamp_t now,
                u32 ttl, u32 idle)
{
	unsigned slot = cache_slot(key);
	struct object *o = slab_cache_alloc_ex(&c->blocks, now, ttl, idle);

	if (!o) {
		cache_probe(c, key, now);      /* sweep the slot we need */
		o = slab_cache_alloc_ex(&c->blocks, now, ttl, idle);
		if (!o)
			return NULL;
	}
	o->key = key;
	o->gen = gen;
	hash_add(c->table, &o->q, slot);
	return o;
}

static struct object *
cache_insert(struct cache *c, u32 key, u32 gen, timestamp_t now)
{
	return cache_insert_ex(c, key, gen, now,
	                       c->blocks.ttl, c->blocks.idle);
}

/* ---- introspection: what is *linked*, live or dead ----------------------- */

static unsigned
cache_slot_len(struct cache *c, unsigned slot)
{
	unsigned n = 0;
	hash_for_each(c->table, slot, it, struct object, q)
		n++;
	return n;
}

static unsigned
cache_linked(struct cache *c)
{
	unsigned slot, n = 0;
	for (slot = 0; slot < CACHE_SLOTS; slot++)
		n += cache_slot_len(c, slot);
	return n;
}

/* Is @key still on its chain, regardless of whether it has expired? */
static bool
cache_linked_key(struct cache *c, u32 key)
{
	hash_for_each(c->table, cache_slot(key), it, struct object, q)
		if (it->key == key)
			return true;
	return false;
}

/* ---- expiry happens at the slot, and only at the slot -------------------- */

static void
test_expiry_only_on_slot_hit(void **state)
{
	(void)state;
	struct slab_policy pol = { .min = 8, .max = 8 };
	struct cache c;
	timestamp_t t = 0;

	/* TTL 1000 ms, no idle timeout. */
	assert_int_equal(cache_init(&c, &pol, 1000, 0), 0);

	assert_non_null(cache_insert(&c, 5, 1, t));   /* slot 5 */
	assert_non_null(cache_insert(&c, 6, 1, t));   /* slot 6 */
	assert_int_equal(slab_cache_live(&c.blocks), 2);

	/*
	 * Both deadlines have passed. Nothing has run: no reap, no gc, and no
	 * slot has been touched - so both objects are still linked and still
	 * hold their blocks.
	 */
	t += 1000;
	assert_int_equal(cache_linked(&c), 2);
	assert_int_equal(slab_cache_live(&c.blocks), 2);
	assert_int_equal(slab_used(&c.blocks.slab), 2);
	assert_int_equal(c.evicted, 0);

	/* Hitting slot 5 collects slot 5. The lookup misses: expired is gone. */
	assert_null(cache_probe(&c, 5, t));
	assert_false(cache_linked_key(&c, 5));
	assert_int_equal(c.evicted, 1);
	assert_int_equal(slab_cache_live(&c.blocks), 1);
	assert_int_equal(slab_used(&c.blocks.slab), 1);

	/* Slot 6 was not touched: dead for just as long, still occupying it. */
	assert_true(cache_linked_key(&c, 6));
	assert_int_equal(cache_slot_len(&c, 6), 1);
	assert_true(slab_cache_expired(&c.blocks, queue_entry(c.table[6].first,
	                               struct object, q), t));

	/* Ten seconds later it is still there - nothing polls it. */
	t += 10000;
	assert_int_equal(cache_linked(&c), 1);
	assert_int_equal(slab_used(&c.blocks.slab), 1);

	/* Until its slot is hit. */
	assert_null(cache_probe(&c, 6, t));
	assert_int_equal(c.evicted, 2);
	assert_int_equal(slab_cache_live(&c.blocks), 0);
	assert_int_equal(slab_used(&c.blocks.slab), 0);

	cache_fini(&c);
}

/* ---- a hit restarts the idle window; the TTL is absolute ----------------- */

static void
test_hit_refreshes_idle_not_ttl(void **state)
{
	(void)state;
	struct slab_policy pol = { .min = 8, .max = 8 };
	struct cache c;
	struct object *o;

	/* TTL 2000 ms, idle 500 ms. */
	assert_int_equal(cache_init(&c, &pol, 2000, 500), 0);

	assert_non_null(cache_insert(&c, 5, 1, 0));   /* goes idle */
	assert_non_null(cache_insert(&c, 6, 1, 0));   /* kept hot   */

	/* Both are hit inside their idle window, so both survive it. */
	assert_non_null(cache_probe(&c, 5, 400));
	assert_non_null(cache_probe(&c, 6, 400));
	assert_non_null(cache_probe(&c, 5, 800));
	assert_non_null(cache_probe(&c, 6, 800));

	/* Key 6 stays hot. */
	o = cache_probe(&c, 6, 1200);
	assert_non_null(o);
	assert_int_equal(o->key, 6);

	/*
	 * Key 5 was last touched at 800, so it dies at 1300 - 1300 ms into a
	 * 2000 ms TTL. Idle expiry is what got it, and the probe is what
	 * noticed.
	 */
	assert_null(cache_probe(&c, 5, 1300));
	assert_int_equal(slab_cache_live(&c.blocks), 1);
	assert_int_equal(c.evicted, 1);

	/* Key 6 keeps being touched, and dies at its TTL all the same. */
	assert_non_null(cache_probe(&c, 6, 1600));
	assert_null(cache_probe(&c, 6, 2000));
	assert_int_equal(slab_cache_live(&c.blocks), 0);
	assert_int_equal(slab_used(&c.blocks.slab), 0);

	cache_fini(&c);
}

/* ---- one probe cleans one chain: collisions and the untouched neighbour --- */

static void
test_probe_sweeps_whole_chain(void **state)
{
	(void)state;
	struct slab_policy pol = { .min = 8, .max = 8 };
	struct cache c;
	timestamp_t t = 0;

	assert_int_equal(cache_init(&c, &pol, 1000, 0), 0);

	/* Three keys colliding on slot 1, one neighbour on slot 2. */
	assert_non_null(cache_insert(&c, 1, 1, t));
	assert_non_null(cache_insert(&c, 17, 1, t));
	assert_non_null(cache_insert(&c, 33, 1, t));
	assert_non_null(cache_insert(&c, 2, 1, t));
	assert_int_equal(cache_slot_len(&c, 1), 3);

	/*
	 * A miss on slot 1 (key 49 collides but was never inserted) reclaims
	 * all three dead objects: the sweep follows the slot, not the key.
	 */
	t += 1000;
	assert_null(cache_probe(&c, 49, t));
	assert_int_equal(cache_slot_len(&c, 1), 0);
	assert_int_equal(c.evicted, 3);

	/* Slot 2 was not on the path and keeps its (equally dead) object. */
	assert_int_equal(cache_slot_len(&c, 2), 1);
	assert_int_equal(slab_cache_live(&c.blocks), 1);
	assert_int_equal(slab_used(&c.blocks.slab), 1);

	cache_fini(&c);
}

/* ---- dead nodes leave from the middle of a chain, survivors get promoted -- */

static void
test_chain_splice_and_promotion(void **state)
{
	(void)state;
	struct slab_policy pol = { .min = 8, .max = 8 };
	struct cache c;
	struct object *o;

	assert_int_equal(cache_init(&c, &pol, 0, 0), 0);   /* per-object only */

	/* Chain head order after the inserts: 33, 17, 1. */
	assert_non_null(cache_insert_ex(&c, 1, 1, 0, 1000, 0));
	assert_non_null(cache_insert_ex(&c, 17, 1, 0, 100, 0));  /* dies first */
	assert_non_null(cache_insert_ex(&c, 33, 1, 0, 1000, 0));
	assert_int_equal(cache_slot_len(&c, 1), 3);

	/*
	 * Probing for key 1 (the chain tail) at 200: key 17 has expired and is
	 * unlinked from the middle in O(1) through its **prev, and key 1 is
	 * promoted to the head as the recently used one.
	 */
	o = cache_probe(&c, 1, 200);
	assert_non_null(o);
	assert_int_equal(o->key, 1);
	assert_int_equal(cache_slot_len(&c, 1), 2);
	assert_int_equal(c.evicted, 1);
	assert_false(cache_linked_key(&c, 17));

	assert_int_equal(queue_entry(c.table[1].first, struct object, q)->key, 1);
	assert_int_equal(queue_entry(c.table[1].first->next, struct object,
	                             q)->key, 33);

	/* Both survivors are intact and still live. */
	assert_non_null(cache_probe(&c, 33, 200));
	assert_int_equal(slab_cache_live(&c.blocks), 2);

	cache_fini(&c);
}

/* ---- the reclaimed block is recycled, the old key is not resurrected ------ */

static void
test_recycled_block_is_a_new_object(void **state)
{
	(void)state;
	struct slab_policy pol = { .min = 4, .max = 4 };
	struct cache c;
	struct object *a, *b;
	u32 index;

	assert_int_equal(cache_init(&c, &pol, 1000, 0), 0);

	a = cache_insert(&c, 1, 0xAA, 0);
	assert_non_null(a);
	index = slab_index(&c.blocks.slab, a);

	/* The probe that notices the deadline is what frees the block. */
	assert_null(cache_probe(&c, 1, 1000));
	assert_int_equal(slab_used(&c.blocks.slab), 0);

	/* Next insert takes the same block back off the LIFO free list. */
	b = cache_insert(&c, 17, 0xBB, 1000);
	assert_non_null(b);
	assert_ptr_equal(a, b);
	assert_int_equal(slab_index(&c.blocks.slab, b), index);

	/* Same memory, new incarnation: fresh deadline, and key 1 stays gone. */
	assert_int_equal(b->gen, 0xBB);
	assert_false(slab_cache_expired(&c.blocks, b, 1000));
	assert_null(cache_probe(&c, 1, 1000));
	assert_int_equal(cache_slot_len(&c, 1), 1);

	/* The recycled object lives out its own TTL, measured from reuse. */
	assert_non_null(cache_probe(&c, 17, 1999));
	assert_null(cache_probe(&c, 17, 2000));

	cache_fini(&c);
}

/* ---- under pressure, only the slot you touch gives memory back ----------- */

static void
test_pressure_reclaims_only_the_touched_slot(void **state)
{
	(void)state;
	struct slab_policy pol = { .min = 4, .max = 4 };   /* no growth */
	struct cache c;
	struct object *o;
	timestamp_t t = 0;

	assert_int_equal(cache_init(&c, &pol, 1000, 0), 0);

	/* Fill the slab: two objects on slot 1, two on slot 2. */
	assert_non_null(cache_insert(&c, 1, 1, t));
	assert_non_null(cache_insert(&c, 17, 1, t));
	assert_non_null(cache_insert(&c, 2, 1, t));
	assert_non_null(cache_insert(&c, 18, 1, t));
	assert_int_equal(slab_used(&c.blocks.slab), 4);
	assert_null(slab_cache_alloc(&c.blocks, t));       /* exhausted */

	/* Everything is dead now, but nothing has looked. */
	t += 1000;
	assert_int_equal(slab_cache_live(&c.blocks), 4);

	/*
	 * Inserting on the empty slot 3 finds nothing to reclaim there: the
	 * four dead objects are on slots 1 and 2, out of reach of this probe.
	 * The insert fails rather than reaching across the table.
	 */
	assert_null(cache_insert(&c, 3, 2, t));
	assert_int_equal(c.evicted, 0);
	assert_int_equal(slab_cache_live(&c.blocks), 4);
	assert_int_equal(slab_used(&c.blocks.slab), 4);

	/* Inserting on slot 1 sweeps slot 1, and that is enough to proceed. */
	assert_non_null(cache_insert(&c, 33, 2, t));
	assert_int_equal(c.evicted, 2);
	assert_int_equal(cache_slot_len(&c, 1), 1);
	assert_int_equal(slab_cache_live(&c.blocks), 3);   /* 2 dead + 1 new */

	/* Slot 2 still holds its two dead objects, untouched and unnoticed. */
	assert_int_equal(cache_slot_len(&c, 2), 2);

	/*
	 * That sweep left one block spare, so an insert on slot 2 is served
	 * straight from the free list. No pressure, no sweep: the new object
	 * joins the chain in front of two corpses without disturbing them.
	 */
	assert_non_null(cache_insert(&c, 34, 2, t));
	assert_int_equal(c.evicted, 2);
	assert_int_equal(cache_slot_len(&c, 2), 3);        /* 2 dead + 1 live */
	assert_int_equal(slab_used(&c.blocks.slab), 4);

	/* They go when something finally walks that chain - here, a lookup. */
	o = cache_probe(&c, 34, t);
	assert_non_null(o);
	assert_int_equal(o->key, 34);
	assert_int_equal(c.evicted, 4);
	assert_int_equal(cache_slot_len(&c, 2), 1);
	assert_int_equal(slab_cache_live(&c.blocks), 2);   /* keys 33 and 34 */
	assert_int_equal(slab_used(&c.blocks.slab), 2);

	cache_fini(&c);
}

/* ---- sweeping every slot equals what a periodic reap would have done ------ */

static void
test_full_pass_equals_periodic_reap(void **state)
{
	(void)state;
	struct slab_policy pol = { .min = 32, .max = 32 };
	struct cache c;
	timestamp_t t = 0;
	unsigned slot, k;

	assert_int_equal(cache_init(&c, &pol, 0, 0), 0);

	/* 24 keys over 16 slots: even keys expire at 1000, odd keys much later. */
	for (k = 0; k < 24; k++)
		assert_non_null(cache_insert_ex(&c, k, k, t,
		                                (k & 1) ? 100000 : 1000, 0));
	assert_int_equal(cache_linked(&c), 24);
	assert_int_equal(slab_cache_live(&c.blocks), 24);

	/* Touch every slot once, with a key that is not there (a pure miss). */
	t += 1000;
	for (slot = 0; slot < CACHE_SLOTS; slot++)
		assert_null(cache_probe(&c, 1024 + slot, t));

	assert_int_equal(c.evicted, 12);
	assert_int_equal(cache_linked(&c), 12);
	assert_int_equal(slab_cache_live(&c.blocks), 12);
	assert_int_equal(slab_used(&c.blocks.slab), 12);

	/* Every survivor is an odd key, and every one of them is still live. */
	for (slot = 0; slot < CACHE_SLOTS; slot++)
		hash_for_each(c.table, slot, it, struct object, q) {
			assert_int_equal(it->key & 1, 1);
			assert_int_equal(it->gen, it->key);
			assert_false(slab_cache_expired(&c.blocks, it, t));
		}

	/*
	 * The point of the whole exercise: after the lazy pass there is nothing
	 * left for a periodic reaper to collect. It was never needed to reach
	 * this state - the traffic paid for it.
	 */
	assert_int_equal(slab_cache_reap(&c.blocks, t), 0);
	assert_int_equal(slab_cache_live(&c.blocks), 12);
	assert_int_equal(cache_linked(&c), 12);

	cache_fini(&c);
}

int
main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_expiry_only_on_slot_hit),
		cmocka_unit_test(test_hit_refreshes_idle_not_ttl),
		cmocka_unit_test(test_probe_sweeps_whole_chain),
		cmocka_unit_test(test_chain_splice_and_promotion),
		cmocka_unit_test(test_recycled_block_is_a_new_object),
		cmocka_unit_test(test_pressure_reclaims_only_the_touched_slot),
		cmocka_unit_test(test_full_pass_equals_periodic_reap),
	};
	return cmocka_run_group_tests_name("hashtable_cache", tests, NULL, NULL);
}
