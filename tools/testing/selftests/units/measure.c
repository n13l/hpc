/*
 * Unit tests for the measurement metrics: counters (monotonic), gauges
 * (up/down) and ratios (a percentage derived from two fields).
 *
 * The metric table, ratio arithmetic and aggregation operate on a struct the
 * caller owns, so those tests run unconditionally. The live counting (a
 * subsystem bumping its attached measure) needs the storage pointer, compiled
 * in only under CONFIG_MEASURE, so those tests skip when it is off.
 *
 * The headline case is aggregation: sum several per-thread measurement structs
 * into one global (counters and gauges add), then read a ratio - it recomputes
 * from the aggregated numerator/denominator, giving e.g. the true global slab
 * usage rather than an average of per-thread percentages.
 *
 * Two things run in every build, by design, and are tested that way:
 *
 *   always_measure_*  the family for metrics the logic itself reads. It is
 *                     compiled in whatever CONFIG_MEASURE says, so the tests
 *                     below count through it and assert the values in both
 *                     configurations. Only the names and descriptions follow
 *                     CONFIG_MEASURE - without it they are empty strings, which
 *                     is why every name assertion here goes through
 *                     expect_name() rather than asserting the literal.
 *
 *   history           a ring of CONFIG_MEASURE_HISTORY snapshots, one saved per
 *                     CONFIG_MEASURE_HISTORY_INTERVAL seconds. Time is passed
 *                     in by the caller, so the interval, the wrap and the row
 *                     order are all tested with a made-up clock.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <string.h>
#include <cmocka.h>

#include <hpc/compiler.h>
#include <hpc/measure.h>
#include <mem/slab.h>
#include <mem/measure.h>
#include <hpc/hash/table.h>
#include <hpc/hash/measure.h>

/* index of the sole ratio metric in a namespace's table */
#define ratio_index(ns, out) do { \
	measure_for_each(ns, _i) \
		if (measure_kind(ns, _i) == MEASURE_RATIO) { (out) = _i; break; } \
} while (0)

/*
 * Metric names and descriptions are metadata for a report and are emitted only
 * under CONFIG_MEASURE; without it the table keeps its shape - kinds, ratio
 * fields, counts - and every string reads empty. Assert names through this.
 */
#ifdef CONFIG_MEASURE
#define expect_name(_got, _want)  assert_string_equal(_got, _want)
#else
#define expect_name(_got, _want) \
	do { (void)(_want); assert_string_equal(_got, ""); } while (0)
#endif

/*
 * A namespace of the test's own, measured with the always-family: its counters
 * exist in every build because the code under test reads them back.
 */
#define DEMO_METRICS(_ns, C, G, R) \
	C(_ns, events, "Events served") \
	C(_ns, drops,  "Events dropped") \
	G(_ns, level,  "Current level") \
	R(_ns, loss, drops, events, "Dropped as percent of served")

DEFINE_MEASURE(demo, DEMO_METRICS);

/*
 * The same list again as a namespace whose names are its interface: something
 * outside the process selects these metrics by name, so DEFINE_MEASURE_ALWAYS
 * keeps the names in every build and lets only the descriptions follow
 * CONFIG_MEASURE.
 */
DEFINE_MEASURE_ALWAYS(named, DEMO_METRICS);

/* a subsystem handle whose measure pointer is there whatever the build says */
struct demo_logic {
	always_measure_member(demo)
	unsigned watermark;
};

/* ---- metric table introspection (always available) ----------------------- */

static void
test_slab_metric_table(void **state)
{
	(void)state;

	assert_int_equal(measure_count(slab), 11);  /* 8 counter + 2 gauge + 1 ratio */
	assert_int_equal(measure_nfield(slab), 10); /* ratio has no storage */

	unsigned counters = 0, gauges = 0, ratios = 0;
	measure_for_each(slab, i) {
		switch (measure_kind(slab, i)) {
		case MEASURE_COUNTER: counters++; break;
		case MEASURE_GAUGE:   gauges++;   break;
		case MEASURE_RATIO:   ratios++;   break;
		}
		assert_non_null(measure_desc(slab, i));
	}
	assert_int_equal(counters, 8);
	assert_int_equal(gauges, 2);
	assert_int_equal(ratios, 1);

	/* gauges follow the counters; the ratio is last */
	expect_name(measure_name(slab, 8), "used");
	expect_name(measure_name(slab, 9), "committed");
	expect_name(measure_name(slab, 10), "usage");
	assert_int_equal(measure_kind(slab, 8), MEASURE_GAUGE);
	assert_int_equal(measure_kind(slab, 10), MEASURE_RATIO);
}

static void
test_slab_ratio(void **state)
{
	(void)state;

	struct slab_measure m = { 0 };
	m.used = 3;
	m.committed = 4;

	/* direct ratio helper */
	assert_int_equal(measure_ratio(m.used, m.committed), 75);
	/* divide-by-zero guard */
	assert_int_equal(measure_ratio(m.used, 0), 0);

	/* the ratio metric read generically recomputes from the fields */
	unsigned r = 0;
	ratio_index(slab, r);
	assert_int_equal(measure_at(slab, &m, r), 75);

	/* a stored field read generically returns the field itself */
	assert_int_equal(measure_at(slab, &m, 8), 3);   /* used */
}

/* aggregation: per-thread structs summed to a global, ratio recomputed */
static void
test_slab_aggregate(void **state)
{
	(void)state;

	struct slab_measure t0 = { 0 }, t1 = { 0 }, global = { 0 };

	t0.alloc = 5; t0.used = 2; t0.committed = 4;   /* thread 0: 50% used */
	t1.alloc = 3; t1.used = 1; t1.committed = 6;   /* thread 1: ~16% used */

	measure_aggregate(slab, &global, &t0);
	measure_aggregate(slab, &global, &t1);

	/* counters and gauges add */
	assert_int_equal(global.alloc, 8);
	assert_int_equal(global.used, 3);
	assert_int_equal(global.committed, 10);

	/* global usage is sum(used)/sum(committed) = 30%, NOT (50+16)/2 */
	unsigned r = 0;
	ratio_index(slab, r);
	assert_int_equal(measure_at(slab, &global, r), 30);
	assert_int_equal(measure_ratio(global.used, global.committed), 30);
}

static void
test_hash_metric_table(void **state)
{
	(void)state;

	assert_int_equal(measure_count(hash), 5);   /* 4 counter + 1 gauge */
	assert_int_equal(measure_nfield(hash), 5);  /* no ratio */

	const char *expect[] = { "add", "del", "collision", "ruc", "entries" };
	measure_for_each(hash, i)
		expect_name(measure_name(hash, i), expect[i]);
	assert_int_equal(measure_kind(hash, 4), MEASURE_GAUGE);
}

/* per-kind section walks (the C-clean SA_COUNTER_FOR_EACH replacement) */
static void
test_slab_sections(void **state)
{
	(void)state;

	unsigned nc = 0, ng = 0, nr = 0;
	measure_for_each_counter(slab, i) { nc++; assert_int_equal(measure_kind(slab, i), MEASURE_COUNTER); }
	measure_for_each_gauge(slab, i)   { ng++; assert_int_equal(measure_kind(slab, i), MEASURE_GAUGE); }
	measure_for_each_ratio(slab, i)   { nr++; assert_int_equal(measure_kind(slab, i), MEASURE_RATIO); }
	assert_int_equal(nc, 8);
	assert_int_equal(ng, 2);
	assert_int_equal(nr, 1);

	/* the gauge section is exactly {used, committed}, in order */
	const char *g[2];
	unsigned k = 0;
	measure_for_each_gauge(slab, i)
		g[k++] = measure_name(slab, i);
	assert_int_equal(k, 2);
	expect_name(g[0], "used");
	expect_name(g[1], "committed");

	/* walking a section reads values of that kind off a caller's struct */
	struct slab_measure m = { 0 };
	m.alloc = 10; m.used = 3; m.committed = 4;

	u64 csum = 0;
	measure_for_each_counter(slab, i)
		csum += measure_value(&m, i);
	assert_int_equal(csum, 10);              /* only alloc was set */

	measure_for_each_ratio(slab, i)
		assert_int_equal(measure_at(slab, &m, i), 75); /* usage 3/4 */
}

static void
test_hash_sections(void **state)
{
	(void)state;

	unsigned nc = 0, ng = 0, nr = 0;
	measure_for_each_counter(hash, i) nc++;
	measure_for_each_gauge(hash, i)   ng++;
	measure_for_each_ratio(hash, i)   nr++;
	assert_int_equal(nc, 4);
	assert_int_equal(ng, 1);
	assert_int_equal(nr, 0);

	const char *name = NULL;
	measure_for_each_gauge(hash, i)
		name = measure_name(hash, i);
	expect_name(name, "entries");
}

/* walk every counter, gauge and ratio and print name = value */
static void
test_slab_report(void **state)
{
	(void)state;

	/* a representative snapshot to report */
	struct slab_measure m = { 0 };
	m.alloc = 128; m.free = 120; m.fail = 2;
	m.grow = 4; m.shrink = 1; m.gc = 3; m.commit = 32; m.reclaim = 8;
	m.used = 24; m.committed = 32;

	print_message("slab metrics:\n");

	print_message("  counters:\n");
	measure_for_each_counter(slab, i)
		print_message("    %-10s = %llu    (%s)\n", measure_name(slab, i),
		              (unsigned long long)measure_value(&m, i),
		              measure_desc(slab, i));

	print_message("  gauges:\n");
	measure_for_each_gauge(slab, i)
		print_message("    %-10s = %llu    (%s)\n", measure_name(slab, i),
		              (unsigned long long)measure_value(&m, i),
		              measure_desc(slab, i));

	print_message("  ratios:\n");
	measure_for_each_ratio(slab, i)
		print_message("    %-10s = %llu%%   (%s)\n", measure_name(slab, i),
		              (unsigned long long)measure_at(slab, &m, i),
		              measure_desc(slab, i));

	/* the printed usage ratio is used/committed = 24/32 = 75% */
	measure_for_each_ratio(slab, i)
		assert_int_equal(measure_at(slab, &m, i), 75);
}

/* ---- the always-family (compiled in whatever CONFIG_MEASURE says) -------- */

/*
 * The point of the family: a subsystem whose logic reads its own counters. The
 * pointer, the increments and the values are all there in both builds, so this
 * test asserts real numbers unconditionally.
 */
static void
test_always_measure(void **state)
{
	(void)state;

	struct demo_measure m = { 0 };
	struct demo_logic l = { 0 };

	l.measure = &m;
	l.watermark = 4;

	always_measure_inc(l.measure, events);
	always_measure_add(l.measure, events, 9);
	always_measure_set(l.measure, level, 5);
	always_measure_inc(l.measure, level);         /* gauge up   */
	always_measure_dec(l.measure, level);         /* gauge down */
	always_measure_sub(l.measure, level, 2);
	always_measure_inc_if(l.measure, 1, drops);
	always_measure_inc_if(l.measure, 0, drops);

	assert_int_equal(m.events, 10);
	assert_int_equal(m.level, 3);
	assert_int_equal(m.drops, 1);

	/* read back - the accessor logic may branch on, in any build */
	assert_int_equal(always_measure_get(l.measure, events), 10);
	assert_int_equal(always_measure_get(l.measure, level), 3);

	/* the level is an input to the algorithm, not just a report */
	assert_true(always_measure_get(l.measure, level) < l.watermark);

	/* a NULL measure counts nothing and reads zero */
	l.measure = NULL;
	always_measure_inc(l.measure, events);
	assert_int_equal(always_measure_get(l.measure, events), 0);
	assert_int_equal(m.events, 10);

	/* ratio over always-measured fields: 1 drop of 10 events */
	assert_int_equal(measure_at(demo, &m, 3), 10);
}

/*
 * Labels are the only part that follows CONFIG_MEASURE: the table keeps its
 * shape either way, the strings go.
 */
static void
test_measure_labels(void **state)
{
	(void)state;

	assert_int_equal(measure_count(demo), 4);   /* 2 counter + 1 gauge + 1 ratio */
	assert_int_equal(measure_nfield(demo), 3);  /* the ratio has no storage */
	assert_int_equal(measure_kind(demo, 3), MEASURE_RATIO);

	/* never NULL, so walking code needs no guard of its own */
	measure_for_each(demo, i) {
		assert_non_null(measure_name(demo, i));
		assert_non_null(measure_desc(demo, i));
	}

	expect_name(measure_name(demo, 0), "events");
#ifdef CONFIG_MEASURE
	assert_string_equal(measure_desc(demo, 2), "Current level");
#else
	assert_string_equal(measure_name(demo, 0), "");
	assert_string_equal(measure_desc(demo, 2), "");
#endif
}

/*
 * DEFINE_MEASURE_ALWAYS: the names are output, not metadata, so they survive a
 * build with no measurement in it. Everything else about the namespace is the
 * same, which is the point - a caller cannot tell the two apart except by
 * asking for a name.
 */
static void
test_measure_named(void **state)
{
	(void)state;

	assert_int_equal(measure_count(named), measure_count(demo));
	assert_int_equal(measure_nfield(named), measure_nfield(demo));

	/* the names are there in both builds */
	assert_string_equal(measure_name(named, 0), "events");
	assert_string_equal(measure_name(named, 1), "drops");
	assert_string_equal(measure_name(named, 2), "level");
	assert_string_equal(measure_name(named, 3), "loss");
	assert_int_equal(measure_kind(named, 2), MEASURE_GAUGE);
	assert_int_equal(measure_kind(named, 3), MEASURE_RATIO);

	/* the descriptions still follow CONFIG_MEASURE */
#ifdef CONFIG_MEASURE
	assert_string_equal(measure_desc(named, 2), "Current level");
#else
	assert_string_equal(measure_desc(named, 2), "");
#endif

	/* and it counts and reads back like any other namespace. Through a
	 * pointer, as a subsystem holds one: the NULL check in the accessors is
	 * what they are for, and handing them the address of a local only tells
	 * the compiler it can never be NULL */
	struct named_measure store = { 0 };
	struct named_measure *m = &store;

	always_measure_add(m, events, 200);
	always_measure_add(m, drops, 50);
	always_measure_set(m, level, 9);
	assert_int_equal(measure_at(named, m, 3), 25);           /* 50 of 200 */
	assert_non_null(measure_of(named, m, "level"));
	assert_int_equal(*measure_of(named, m, "level"), 9);
}

/* ---- history: a ring of snapshots on a caller-supplied clock ------------- */

DEFINE_MEASURE_HISTORY(demo, demo_hist);

/* the interval decides which ticks save a row; the rest are a compare */
static void
test_history_interval(void **state)
{
	(void)state;

	struct demo_logic l = { 0 };
	timestamp_t now = 1000;

	/* nothing is saved where the depth is 0 - see the header on that build */
	if (!measure_history_depth)
		skip();

	measure_history_init(demo, &demo_hist, now);
	l.measure = measure_history_live(&demo_hist);

	assert_int_equal(measure_history_count(&demo_hist), 0);

	always_measure_add(l.measure, events, 7);
	assert_int_equal(measure_history_tick(demo, &demo_hist, now + 1), 0);
	assert_int_equal(measure_history_count(&demo_hist), 0);

	now += measure_history_period;                 /* now it is due */
	assert_int_equal(measure_history_tick(demo, &demo_hist, now), 1);
	assert_int_equal(measure_history_count(&demo_hist), 1);
	assert_int_equal(measure_history_time(&demo_hist, 0), now);
	assert_int_equal(measure_history_at(&demo_hist, 0)->events, 7);

	/* the row is a snapshot: later counting does not change it */
	always_measure_add(l.measure, events, 5);
	assert_int_equal(measure_history_at(&demo_hist, 0)->events, 7);
	assert_int_equal(always_measure_get(l.measure, events), 12);

	/* an unforced tick before the next is due changes nothing... */
	assert_int_equal(measure_history_tick(demo, &demo_hist,
	                                      now + measure_history_period - 1), 0);
	assert_int_equal(measure_history_count(&demo_hist), 1);
	/* ...while a save takes a row whenever the caller wants one */
	measure_history_save(demo, &demo_hist, now + 1);
	if (measure_history_depth >= 2) {           /* a depth-1 ring just wrapped */
		assert_int_equal(measure_history_count(&demo_hist), 2);
		assert_int_equal(measure_history_at(&demo_hist, 1)->events, 12);
	}

	/* and the interval itself is a default, overridable at runtime */
	measure_history_every(demo, &demo_hist, 500, now);
	assert_int_equal(measure_history_tick(demo, &demo_hist, now + 499), 0);
	assert_int_equal(measure_history_tick(demo, &demo_hist, now + 500), 1);
}

/* the ring wraps: oldest row drops, iteration stays oldest to newest */
static void
test_history_wrap(void **state)
{
	(void)state;

	struct demo_logic l = { 0 };
	const timestamp_t base = 10000;
	const unsigned over = 3;                       /* saves past a full ring */
	timestamp_t now = base;

	/* there is no ring to wrap at depth 0 */
	if (!measure_history_depth)
		skip();

	measure_history_init(demo, &demo_hist, now);
	l.measure = measure_history_live(&demo_hist);

	for (unsigned i = 0; i < measure_history_depth + over; i++) {
		always_measure_inc(l.measure, events);
		now += measure_history_period;
		assert_int_equal(measure_history_tick(demo, &demo_hist, now), 1);
	}

	/* it holds exactly the depth, and stops there */
	assert_int_equal(measure_history_count(&demo_hist),
	                 measure_history_depth);

	/* the oldest row is the (over+1)-th taken, the newest the last */
	assert_int_equal(measure_history_time(&demo_hist, 0),
	                 base + (timestamp_t)(over + 1) * measure_history_period);
	assert_int_equal(measure_history_time(&demo_hist,
	                                      measure_history_depth - 1), now);
	assert_int_equal(measure_history_at(&demo_hist,
	                                    measure_history_depth - 1)->events,
	                 measure_history_depth + over);

	/* walking gives rows in time order, one interval apart */
	timestamp_t prev = 0;
	unsigned seen = 0;
	measure_history_for_each(&demo_hist, r) {
		timestamp_t t = measure_history_time(&demo_hist, r);
		if (seen)
			assert_int_equal(t - prev, measure_history_period);
		prev = t;
		seen++;
	}
	assert_int_equal(seen, measure_history_depth);
}

/* what a history is for: rates between rows, and a ratio read per row */
static void
test_history_rates(void **state)
{
	(void)state;

	struct demo_logic l = { 0 };
	timestamp_t now = 0;

	/* a rate is read between two rows; CONFIG_MEASURE_HISTORY may be 1 */
	if (measure_history_depth < 2)
		skip();

	measure_history_init(demo, &demo_hist, now);
	l.measure = measure_history_live(&demo_hist);

	always_measure_add(l.measure, events, 100);
	always_measure_add(l.measure, drops, 10);
	always_measure_set(l.measure, level, 4);
	now += measure_history_period;
	measure_history_tick(demo, &demo_hist, now);

	always_measure_add(l.measure, events, 300);   /* 300 more in one interval */
	always_measure_add(l.measure, drops, 30);
	always_measure_set(l.measure, level, 7);
	now += measure_history_period;
	measure_history_tick(demo, &demo_hist, now);

	const struct demo_measure *older = measure_history_at(&demo_hist, 0);
	const struct demo_measure *newer = measure_history_at(&demo_hist, 1);

	/* the loss ratio is recomputed per row, never averaged across them */
	assert_int_equal(measure_at(demo, older, 3), 10);   /* 10 of 100  */
	assert_int_equal(measure_at(demo, newer, 3), 10);   /* 40 of 400  */

	struct demo_measure d;
	measure_delta(demo, &d, newer, older);
	assert_int_equal(d.events, 300);
	assert_int_equal(d.drops, 30);
	assert_int_equal(d.level, 3);                       /* change in level */

	/* events per second over the interval between the two rows */
	timestamp_t ms = measure_history_time(&demo_hist, 1) -
	                 measure_history_time(&demo_hist, 0);
	assert_int_equal(ms, measure_history_period);
	assert_int_equal(d.events * 1000 / ms,
	                 300 / CONFIG_MEASURE_HISTORY_INTERVAL);

	/* the delta of a ratio's fields still reads as a ratio */
	assert_int_equal(measure_at(demo, &d, 3), 10);
}

/* a history of a real namespace, and a member rather than a file-scope object */
static void
test_history_member(void **state)
{
	(void)state;

	struct holder {
		measure_history_member(slab, hist)
		unsigned other;
	} h;

	/* the member survives a depth of 0, but it holds no row to read back */
	if (!measure_history_depth)
		skip();

	measure_history_init(slab, &h.hist, 0);
	struct slab_measure *m = measure_history_live(&h.hist);

	always_measure_add(m, used, 3);
	always_measure_add(m, committed, 4);
	measure_history_save(slab, &h.hist, 1);

	unsigned r = 0;
	ratio_index(slab, r);
	assert_int_equal(measure_at(slab, measure_history_at(&h.hist, 0), r), 75);
	assert_int_equal(measure_history_time(&h.hist, 0), 1);
}

/* ---- live counting (needs the storage pointer -> CONFIG_MEASURE) --------- */

#ifdef CONFIG_MEASURE

static void
test_slab_live(void **state)
{
	(void)state;
	/* A block of exactly one grain, so the gauge below counts the blocks this
	 * test asks for rather than the grain they round up to - a slab grows and
	 * shrinks in whole grains (see <mem/slab_vm.h>). */
	struct slab_policy pol = { .min = 0, .max = 8, .grow_step = 4 };
	struct slab s;
	struct slab_measure m = { 0 };

	assert_int_equal(slab_init(&s, SLAB_GRAIN_BYTES, &pol), 0);
	s.measure = &m;

	assert_int_equal(slab_grow(&s, 4), 4);      /* commit 4 blocks */
	void *p0 = slab_alloc(&s);
	void *p1 = slab_alloc(&s);
	void *p2 = slab_alloc(&s);                  /* 3 live of 4 committed */
	slab_free(&s, p1);                          /* gauge falls back to 2 */

	assert_int_equal(m.grow, 1);                /* counters */
	assert_int_equal(m.commit, 4);
	assert_int_equal(m.alloc, 3);
	assert_int_equal(m.free, 1);
	assert_int_equal(m.used, 2);                /* gauge went 0..3..2 */
	assert_int_equal(m.committed, 4);           /* gauge */

	unsigned r = 0;
	ratio_index(slab, r);
	assert_int_equal(measure_at(slab, &m, r), 50); /* usage 2/4 */

	(void)p0; (void)p2;
	slab_fini(&s);
}

/* the concrete motivation: two per-"thread" slabs aggregated to a global */
static void
test_slab_aggregate_live(void **state)
{
	(void)state;
	struct slab_policy pol = { .min = 0, .max = 16, .grow_step = 4 };
	struct slab a, b;
	struct slab_measure ma = { 0 }, mb = { 0 }, global = { 0 };

	assert_int_equal(slab_init(&a, SLAB_GRAIN_BYTES, &pol), 0);
	assert_int_equal(slab_init(&b, SLAB_GRAIN_BYTES, &pol), 0);
	a.measure = &ma;
	b.measure = &mb;

	slab_grow(&a, 4);
	slab_alloc(&a);                             /* a: 1 used / 4 committed */

	slab_grow(&b, 8);
	for (int i = 0; i < 6; i++)
		slab_alloc(&b);                     /* b: 6 used / 8 committed */

	measure_aggregate(slab, &global, &ma);
	measure_aggregate(slab, &global, &mb);
	assert_int_equal(global.used, 7);
	assert_int_equal(global.committed, 12);

	unsigned r = 0;
	ratio_index(slab, r);
	assert_int_equal(measure_at(slab, &global, r), 58); /* 7*100/12 */

	slab_fini(&a);
	slab_fini(&b);
}

static void
test_slab_no_measure(void **state)
{
	(void)state;
	struct slab_policy pol = { .min = 1, .max = 4, .grow_step = 1 };
	struct slab s;

	assert_int_equal(slab_init(&s, SLAB_GRAIN_BYTES, &pol), 0);
	assert_null(s.measure);
	slab_free(&s, slab_alloc(&s));              /* no measure -> no crash */
	slab_fini(&s);
}

struct hdata { unsigned int id; struct qnode q; };

static void
test_hash_live(void **state)
{
	(void)state;
	struct hash_measure hm = { 0 };
	hash_measure = &hm;

	DECLARE_HASHTABLE(table, 4);
	hash_init_table(table, 4);

	struct hdata d[3] = { { .id = 0 }, { .id = 1 }, { .id = 2 } };
	for (unsigned i = 0; i < 3; i++)
		hash_add(table, &d[i].q, 0);        /* one bucket */

	assert_int_equal(hm.add, 3);
	assert_int_equal(hm.collision, 2);
	assert_int_equal(hm.entries, 3);            /* gauge up */

	hash_del(&d[0].q);
	hash_del_init(&d[1].q);
	assert_int_equal(hm.del, 2);
	assert_int_equal(hm.entries, 1);            /* gauge down */

	hash_ruc(table, &d[2].q, 0);
	assert_int_equal(hm.ruc, 1);
	assert_int_equal(hm.entries, 1);            /* a move: gauge unchanged */

	hash_measure = NULL;
}

/*
 * The whole path, on a real subsystem: attach a history's live struct to a
 * slab, work it, and tick the history from the loop the caller already has.
 * The slab declares its pointer with measure_member(), so the attach - not the
 * history - is what needs CONFIG_MEASURE here.
 */
static void
test_slab_history_live(void **state)
{
	(void)state;
	struct slab_policy pol = { .min = 0, .max = 8, .grow_step = 4 };
	struct slab s;
	struct slab_history h;
	timestamp_t now = 500;

	/* before and after a free are two rows; CONFIG_MEASURE_HISTORY may be 1 */
	if (measure_history_depth < 2)
		skip();

	assert_int_equal(slab_init(&s, SLAB_GRAIN_BYTES, &pol), 0);
	measure_history_init(slab, &h, now);
	s.measure = measure_history_live(&h);

	slab_grow(&s, 4);
	void *p0 = slab_alloc(&s);
	slab_alloc(&s);

	now += measure_history_period;
	assert_int_equal(measure_history_tick(slab, &h, now), 1);

	slab_free(&s, p0);                          /* after the snapshot */
	now += measure_history_period;
	assert_int_equal(measure_history_tick(slab, &h, now), 1);

	assert_int_equal(measure_history_count(&h), 2);
	assert_int_equal(measure_history_at(&h, 0)->used, 2);
	assert_int_equal(measure_history_at(&h, 1)->used, 1);

	unsigned r = 0;
	ratio_index(slab, r);
	assert_int_equal(measure_at(slab, measure_history_at(&h, 0), r), 50);
	assert_int_equal(measure_at(slab, measure_history_at(&h, 1), r), 25);

	slab_fini(&s);
}

#else /* !CONFIG_MEASURE */

static void test_slab_live(void **s)           { (void)s; skip(); }
static void test_slab_aggregate_live(void **s) { (void)s; skip(); }
static void test_slab_no_measure(void **s)     { (void)s; skip(); }
static void test_hash_live(void **s)           { (void)s; skip(); }
static void test_slab_history_live(void **s)   { (void)s; skip(); }

#endif

int
main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_slab_metric_table),
		cmocka_unit_test(test_slab_ratio),
		cmocka_unit_test(test_slab_aggregate),
		cmocka_unit_test(test_hash_metric_table),
		cmocka_unit_test(test_slab_sections),
		cmocka_unit_test(test_hash_sections),
		cmocka_unit_test(test_slab_report),
		cmocka_unit_test(test_always_measure),
		cmocka_unit_test(test_measure_labels),
		cmocka_unit_test(test_measure_named),
		cmocka_unit_test(test_history_interval),
		cmocka_unit_test(test_history_wrap),
		cmocka_unit_test(test_history_rates),
		cmocka_unit_test(test_history_member),
		cmocka_unit_test(test_slab_live),
		cmocka_unit_test(test_slab_aggregate_live),
		cmocka_unit_test(test_slab_no_measure),
		cmocka_unit_test(test_hash_live),
		cmocka_unit_test(test_slab_history_live),
	};
	return cmocka_run_group_tests_name("measure", tests, NULL, NULL);
}
