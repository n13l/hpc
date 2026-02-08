/*
 * The MIT License (MIT)                                           Measurements
 *
 * Copyright (c) 2015                               Daniel Kubec <niel@rtfm.cz>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"),to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef __HPC_MEASURE_H__
#define __HPC_MEASURE_H__

#include <hpc/compiler.h>
#include <hpc/array.h>
#include <string.h>

typedef const char *sa_ccstr;
typedef struct sa_counter { sa_ccstr name, desc; } sa_counter;

#ifndef CONFIG_SILENT

/* Counters: Metrics that only increase, representing total counts. */
/**
 * Defines a stats counter with optional grouping
 * @param _ns The namespace for the counter
 * @param ... 2-3 args: (_name, _desc) or (_group, _name, _desc)
 */
#define DEFINE_SA_COUNTER(_ns, ...) \
	va_dispatch(DEFINE_SA_COUNTER,__VA_ARGS__)(_ns,__VA_ARGS__)

/**
 * Defines a basic stats counter without grouping
 * @param _ns The namespace for the counter
 * @param _name The counter name 
 * @param _desc Description of what the counter tracks
 */
#define DEFINE_SA_COUNTER2(_ns,_name,_desc) \
	static sa_counter sa_counter_##_ns##_##_name = { \
		stringify(_name), _desc \
	};

/**
 * Defines a stats counter within a group
 * @param _ns The namespace for the counter
 * @param _group The group name for organizing related counters
 * @param _name The counter name
 * @param _desc Description of what the counter tracks
 */
#define DEFINE_SA_COUNTER3(_ns,_group,_name,_desc) \
	static sa_counter sa_counter_##_ns##_##_group##_##_name = { \
		stringify(_name), _desc \
	};

/**
 * Defines a stats counter with optional grouping
 * @param _ns The namespace for the counter
 * @param ... 2-3 args: (_name, _desc) or (_group, _name, _desc)
 */
#define DEFINE_SA_COUNTER_NAME(_ns, ...) \
	va_dispatch(DEFINE_SA_COUNTER_NAME,__VA_ARGS__)(_ns,__VA_ARGS__)

/**
 * Defines a basic stats counter without grouping
 * @param _ns The namespace for the counter
 * @param _name The counter name 
 * @param _desc Description of what the counter tracks
 */
#define DEFINE_SA_COUNTER_NAME1(_ns,_name) \
	static sa_counter sa_counter_##_ns##_##_name = { \
		stringify(_name), stringify(_desc) \
	};

/**
 * Defines a stats counter within a group
 * @param _ns The namespace for the counter
 * @param _group The group name for organizing related counters
 * @param _name The counter name
 * @param _desc Description of what the counter tracks
 */
#define DEFINE_SA_COUNTER_NAME2(_ns,_group,_name) \
	static sa_counter sa_counter_##_ns##_##_group##_##_name = { \
		stringify(_name), stringify(_desc) \
	};

/**
 * Creates a section containing related counters
 * @param _ns The namespace for the section
 * @param ... 1-2 args: (_members) or (_group, _members)
 */
#define DEFINE_SA_COUNTER_SECTION(_ns,...) \
	va_dispatch(DEFINE_SA_COUNTER_SECTION,__VA_ARGS__)(_ns,__VA_ARGS__)

/**
 * Helper macro for counter field type definition
 * @private
 */
#define SA_COUNTER_FIELD_TYPED1(_ns, name) \
	__typeof__(sa_counter_##_ns##_##name) name;

/**
 * Helper macro for counter field initialization
 * @private
 */
#define SA_COUNTER_INIT_TYPED1(_ns, name) \
	.name = sa_counter_##_ns##_##name,

/**
 * Defines a basic counter section struct and instance
 * @param _ns The namespace
 * @param _members Macro taking 3 args to generate members
 */
#define DEFINE_SA_COUNTER_SECTION1(_ns,_members) \
	static struct sa_counter_##_ns { \
		_members(_ns, SA_COUNTER_FIELD_TYPED1) \
	} sa_counter_##_ns = { \
		_members(_ns, SA_COUNTER_INIT_TYPED1) \
	}

/**
 * Helper macro for grouped counter field type definition
 * @private
 */
#define SA_COUNTER_FIELD_TYPED2(_ns,_group, name) \
	__typeof__(sa_counter_##_ns##_##_group##_##name) name;

/**
 * Helper macro for grouped counter field initialization
 * @private
 */
#define SA_COUNTER_INIT_TYPED2(_ns,_group, name) \
	.name = sa_counter_##_ns##_##_group##_##name,

/**
 * Defines a grouped counter section struct and instance
 * @param _ns The namespace
 * @param _group The group name
 * @param _members Macro taking 3 args to generate members
 */
#define DEFINE_SA_COUNTER_SECTION2(_ns,_group,_members) \
	static struct sa_counter_##_ns##_##_group { \
		_members(_ns,_group, SA_COUNTER_FIELD_TYPED2) \
	} sa_counter_##_ns##_##_group = { \
		_members(_ns,_group, SA_COUNTER_INIT_TYPED2) \
	}

/**
 * Gets byte offset of counter within its section
 * @param ns Namespace of the counter
 * @param counter Name of the counter
 * @return Offset in bytes
 */
#define SA_COUNTER_OFFSET(ns, counter) ({_offsetof(ns##_type, counter);})

/**
 * Gets array index of counter within its section
 * @param ns Namespace of the counter
 * @param counter Name of the counter
 * @return Zero-based index
 */
#define SA_COUNTER_INDEX(ns, counter) \
	(COUNTER_OFFSET(ns, counter) / sizeof(sa_counter))

/**
 * Gets total size of counter section in bytes
 * @param _ns The namespace
 * @return Size in bytes
 */
#define SA_COUNTER_SIZE(_ns) sizeof(sa_counter_##_ns)

/**
 * Gets number of counters in a section
 * @param _ns The namespace
 * @return Counter count
 */
#define SA_COUNTER_COUNT(_ns) (SA_COUNTER_SIZE(_ns) / sizeof(sa_counter))

/**
 * Gets pointer to first counter in section
 * @param _ns The namespace
 * @return Pointer to first counter
 */
#define SA_COUNTER_FIRST(_ns) ({\
	((struct sa_counter *)(&sa_counter_##_ns)); \
})

/**
 * Gets pointer to last counter in section
 * @param _ns The namespace
 * @return Pointer to last counter
 */
#define SA_COUNTER_LAST(_ns) ({\
	(((struct sa_counter *)SA_COUNTER_FIRST(_ns))+SA_COUNTER_COUNT(_ns)-1);\
})

/**
 * Iterates over all counters in a section
 * @param _ns The namespace
 * @param ... 2-3 args: (_it, _value, _block) or (_group, _it, _value, _block)
 */
#define SA_COUNTER_FOR_EACH(_ns, ...) \
	va_dispatch(SA_COUNTER_FOR_EACH,__VA_ARGS__)(_ns,__VA_ARGS__)

/**
 * Implementation of basic counter iteration
 * @private
 */
#define SA_COUNTER_FOR_EACH3(_ns, _it, _value, _block) {\
	const struct sa_counter *_it = SA_COUNTER_FIRST(_ns); \
	for (; _it <= SA_COUNTER_LAST(_ns); _it ++) { \
		_block \
	} \
}

/**
 * Implementation of grouped counter iteration
 * @private
 */
#define SA_COUNTER_FOR_EACH4(_ns, _group, _it, _value, _block) {\
	struct sa_counter *_it = SA_COUNTER_FIRST(_ns##_##_group); \
	for (; _it <= SA_COUNTER_LAST(_ns##_##_group); _it ++) { \
		_block \
	} \
}

#define SA_COUNTER_SECTION_TYPE(_ns,_name) sa_counter_##_ns##_##_name

#define DECLARE_SA_COUNTER_SECTION(_ns) \
	struct SA_COUNTER_SECTION_TYPE(_ns, data) { \
		u64 data[SA_COUNTER_COUNT(_ns)]; \
	} __attribute__ ((aligned(16))) SA_COUNTER_SECTION_TYPE(_ns, data)

/*
 * Gauges (metrics that move up and down), measurements (a value read at a
 * point in time) and history (those values over a timeframe, for trends) are
 * not part of the sa_counter descriptors above; they belong to the value
 * metrics that follow this block - MEASURE_GAUGE in the metric table, and the
 * history ring at the end of this header.
 */

#else

#endif

/*
 * Metrics: counters, gauges and ratios
 *
 * Where the sa_counter machinery above registers counter *descriptions*, this
 * defines the live *values* a subsystem accumulates, in three kinds:
 *
 *   counter  a monotonic total that only ever increases (events served)
 *   gauge    a level that moves up and down (blocks currently in use)
 *   ratio    a percentage DERIVED from two other fields (num*100/den); it has
 *            no storage of its own
 *
 * The distinction matters for aggregation. Summing N per-thread measurement
 * structs field by field (measure_aggregate) is correct for counters and
 * gauges - a grand total of events, and a grand total current level. A ratio
 * must NEVER be summed or averaged: it is recomputed from the aggregated
 * numerator and denominator, so e.g. slab usage across all threads is
 * sum(used)*100/sum(committed), not the mean of the per-thread percentages.
 *
 * One X-macro list is the single source of truth. It forwards the namespace
 * and three per-kind generators (C, G, R) to each entry:
 *
 *   #define NS_METRICS(_ns, C, G, R) \
 *       C(_ns, alloc,     "events served ...") \
 *       G(_ns, used,      "current level ...") \
 *       R(_ns, usage, used, committed, "used as %% of committed")
 *
 * Counters and gauges must be listed before ratios (ratios carry no storage).
 * DEFINE_MEASURE(ns, NS_METRICS) then generates:
 *
 *   struct ns_measure { u64 <counter/gauge fields...>; };  (dense u64 array)
 *   ns_measure_meta[]     metric table {name, desc, kind, num, den}
 *   ns_measure_count      metrics total (counters + gauges + ratios)
 *   ns_measure_nfield     stored fields (counters + gauges)
 *
 * The struct is caller-owned: a subsystem keeps only a pointer to it, so any
 * number of instances can share one struct and aggregate, hold their own, or
 * leave it NULL to not measure.
 */

/*
 * Does this build measure at all?
 *
 * For the one caller no #ifdef serves: a tool whose answer to "what did you
 * count" has to be a sentence rather than an absence. Every reporting path over
 * these tables is dead code without CONFIG_MEASURE - the numbers behind it are
 * all zero and the names are all empty - and a report printed anyway says
 * "nothing happened", which is the one wrong answer a reader cannot tell from
 * the truth. So the reports compile out and say this instead, in one wording, so
 * that a capture tool, a daemon and its control protocol all refuse alike.
 *
 * measure_unavailable is the whole message and not a fragment to build one from:
 * an operator who meets it in `un -m`, in `namo measure` and in a log line has
 * met the same sentence three times and only has to learn it once.
 */
#ifdef CONFIG_MEASURE
#define measure_available 1
#else
#define measure_available 0
#endif

#define measure_unavailable \
	"this build does not measure: CONFIG_MEASURE is not set"

enum measure_kind { MEASURE_COUNTER, MEASURE_GAUGE, MEASURE_RATIO };

typedef struct measure_metric {
	sa_ccstr name, desc;
	unsigned kind;
	unsigned num, den;   /* MEASURE_RATIO: field indices of numerator/denom */
} measure_metric;

/*
 * Names and descriptions are metadata for a report, and the only part of a
 * metric table that costs anything at rest: one .rodata string per metric.
 * They are emitted only under CONFIG_MEASURE. Without it the table keeps its
 * shape - the kind of each metric, and the field indices a ratio is computed
 * from, both of which the arithmetic below reads - and every name and
 * description reads as the empty string. Walking code still compiles and still
 * gets the right numbers; it just has nothing to label the rows with.
 *
 * That split is what lets a metric be always measured (see the always_measure_*
 * family further down) without dragging a description table into a build that
 * asked for no measurement: the values a subsystem's logic depends on stay,
 * the prose about them goes.
 */
#ifdef CONFIG_MEASURE
#define measure_string(_s) _s
#else
#define measure_string(_s) ""
#endif

/* per-kind generators used by DEFINE_MEASURE; a list forwards (_ns,C,G,R) */
#define MEASURE_FIELD(_ns, _name, _desc)               u64 _name;
#define MEASURE_FIELD_R(_ns, _name, _num, _den, _desc) /* ratio: no storage */

#define MEASURE_META_C(_ns, _name, _desc) \
	{ measure_string(stringify(_name)), measure_string(_desc), \
	  MEASURE_COUNTER, 0, 0 },
#define MEASURE_META_G(_ns, _name, _desc) \
	{ measure_string(stringify(_name)), measure_string(_desc), \
	  MEASURE_GAUGE, 0, 0 },
#define MEASURE_META_R(_ns, _name, _num, _den, _desc) \
	{ measure_string(stringify(_name)), measure_string(_desc), \
	  MEASURE_RATIO, \
	  offsetof(struct _ns##_measure, _num) / sizeof(u64), \
	  offsetof(struct _ns##_measure, _den) / sizeof(u64) },

/*
 * The same generators for a namespace whose names are not metadata but output:
 * the name stays in every build, the description still follows CONFIG_MEASURE.
 * See DEFINE_MEASURE_ALWAYS below for when that is the right trade.
 */
#define MEASURE_META_C_ALWAYS(_ns, _name, _desc) \
	{ stringify(_name), measure_string(_desc), MEASURE_COUNTER, 0, 0 },
#define MEASURE_META_G_ALWAYS(_ns, _name, _desc) \
	{ stringify(_name), measure_string(_desc), MEASURE_GAUGE, 0, 0 },
#define MEASURE_META_R_ALWAYS(_ns, _name, _num, _den, _desc) \
	{ stringify(_name), measure_string(_desc), MEASURE_RATIO, \
	  offsetof(struct _ns##_measure, _num) / sizeof(u64), \
	  offsetof(struct _ns##_measure, _den) / sizeof(u64) },

#define DEFINE_MEASURE_EX(_ns, _list, _C, _G, _R) \
	struct _ns##_measure { \
		_list(_ns, MEASURE_FIELD, MEASURE_FIELD, MEASURE_FIELD_R) \
	}; \
	static const measure_metric _ns##_measure_meta[] _unused = { \
		_list(_ns, _C, _G, _R) \
	}; \
	enum { _ns##_measure_count  = \
	           sizeof(_ns##_measure_meta) / sizeof(measure_metric), \
	       _ns##_measure_nfield = \
	           sizeof(struct _ns##_measure) / sizeof(u64) }; \
	MEASURE_HISTORY_TYPE(_ns); \
	_Static_assert(sizeof(struct _ns##_measure) % sizeof(u64) == 0, \
	               "measure struct must be a dense array of u64 fields")

#define DEFINE_MEASURE(_ns, _list) \
	DEFINE_MEASURE_EX(_ns, _list, MEASURE_META_C, MEASURE_META_G, \
	                  MEASURE_META_R)

/*
 * A namespace whose metrics are reported by name outside the process.
 *
 * The rule above - names and descriptions are metadata, and go with
 * CONFIG_MEASURE - is right for instrumentation, where the strings exist to
 * label a report that a build without measurement is not producing. It is wrong
 * for a namespace whose names *are* the interface: a metric a control protocol
 * names in a key, a tool selects by name, or an operator types. Compiling those
 * away does not remove a report, it breaks one, and it breaks it in a build that
 * still has every number the report is made of (see the always_measure_* family
 * below - the two go together and for the same reason).
 *
 * So this is the opt-in for exactly that case: the name is emitted whatever
 * CONFIG_MEASURE says, and the description - prose, and the bulk of the bytes -
 * still follows it. A caller that walks such a namespace can rely on
 * measure_name() and must still cope with measure_desc() being "".
 *
 * modules/net uses it: `namo measure` and `namo history` report the flow
 * layer's metrics by name in every build.
 */
#define DEFINE_MEASURE_ALWAYS(_ns, _list) \
	DEFINE_MEASURE_EX(_ns, _list, MEASURE_META_C_ALWAYS, \
	                  MEASURE_META_G_ALWAYS, MEASURE_META_R_ALWAYS)

/* metrics total (counters + gauges + ratios) */
#define measure_count(_ns)      (_ns##_measure_count)
/* stored fields (counters + gauges); ratios have no storage */
#define measure_nfield(_ns)     (_ns##_measure_nfield)
#define measure_name(_ns, _i)   (_ns##_measure_meta[_i].name)
#define measure_desc(_ns, _i)   (_ns##_measure_meta[_i].desc)
#define measure_kind(_ns, _i)   (_ns##_measure_meta[_i].kind)
/* raw stored u64 at field index @_i (lvalue; valid for counter/gauge fields) */
#define measure_value(_m, _i)   (((u64 *)(_m))[_i])

/* iterate metric indices [0, count) of namespace @_ns */
#define measure_for_each(_ns, _i) \
	for (unsigned _i = 0; _i < measure_count(_ns); _i++)

/*
 * The loop body binds to the trailing `else`, guarding against dangling-else:
 *     measure_for_each_gauge(slab, i)
 *         report(measure_name(slab, i), measure_value(m, i));
 */
#define measure_for_each_kind(_ns, _i, _kind) \
	for (unsigned _i = 0; _i < measure_count(_ns); _i++) \
		if (measure_kind(_ns, _i) != (unsigned)(_kind)) {} else
#define measure_for_each_counter(_ns, _i) \
	measure_for_each_kind(_ns, _i, MEASURE_COUNTER)
#define measure_for_each_gauge(_ns, _i) \
	measure_for_each_kind(_ns, _i, MEASURE_GAUGE)
#define measure_for_each_ratio(_ns, _i) \
	measure_for_each_kind(_ns, _i, MEASURE_RATIO)

/* percentage num*100/den, guarding divide-by-zero; the aggregation-safe ratio */
static inline u64
measure_pct(u64 num, u64 den)
{
	return den ? num * 100 / den : 0;
}
#define measure_ratio(_num, _den) measure_pct((u64)(_num), (u64)(_den))

/*
 * numeric value of metric @_i of @_m: the stored field, or for a ratio the
 * percentage recomputed from its numerator/denominator fields (so it is always
 * correct, including on an aggregated struct)
 */
static inline u64
measure_read(const measure_metric *meta, const u64 *vals, unsigned i)
{
	if (meta[i].kind == MEASURE_RATIO)
		return measure_pct(vals[meta[i].num], vals[meta[i].den]);
	return vals[i];
}
#define measure_at(_ns, _m, _i) \
	measure_read(_ns##_measure_meta, (const u64 *)(_m), (_i))

/* aggregate @_src into @_dst by summing stored fields (counters and gauges) */
static inline void
measure_sum(u64 *dst, const u64 *src, unsigned nfield)
{
	for (unsigned i = 0; i < nfield; i++)
		dst[i] += src[i];
}
#define measure_aggregate(_ns, _dst, _src) \
	measure_sum((u64 *)(_dst), (const u64 *)(_src), measure_nfield(_ns))

/* look up a stored field by name; returns &value or NULL (ratios excluded) */
static inline u64 *
measure_lookup(const measure_metric *meta, u64 *values, unsigned nfield,
               const char *name)
{
	for (unsigned i = 0; i < nfield; i++)
		if (!strcmp(meta[i].name, name))
			return &values[i];
	return NULL;
}
#define measure_of(_ns, _m, _name) \
	measure_lookup(_ns##_measure_meta, (u64 *)(_m), \
	               measure_nfield(_ns), (_name))

/*
 * Attaching and bumping: two families
 *
 * A subsystem holds a pointer to a caller-owned struct <ns>_measure and counts
 * through it. There are two families of operations over that pointer, the same
 * shapes throughout, differing only in whether CONFIG_MEASURE can take them
 * away:
 *
 *   measure_*         Instrumentation. Compiled in only under CONFIG_MEASURE;
 *                     with it off the storage pointer and every increment
 *                     expand to nothing - no field, no branch, no code, and @m
 *                     is not even evaluated, so referencing the (absent) member
 *                     never reaches the compiler.
 *
 *   always_measure_*  Metrics the logic itself reads. Always compiled in,
 *                     whatever CONFIG_MEASURE says. A gauge like the number of
 *                     blocks a slab has committed, or the entries a table
 *                     holds, is not an observation of the algorithm - it is an
 *                     input to it, tested to decide whether to grow, shrink or
 *                     evict. Switching measurement off must not take those
 *                     away, or the logic changes with the build. What does
 *                     follow CONFIG_MEASURE, even here, is the prose: with it
 *                     off the metric names and descriptions are empty strings
 *                     (see measure_string above), so an always-measured
 *                     namespace costs its u64s and nothing else.
 *
 * A namespace declares its storage pointer with exactly one of the two member
 * macros - they declare the same member, so it is one or the other. Choosing
 * always_measure_member() does not mean every event has to be counted
 * unconditionally: with the pointer always present, always_measure_* the few
 * levels the algorithm reads, and plain measure_* everything a report alone
 * cares about. The latter still vanish without CONFIG_MEASURE.
 *
 * storage:
 * - measure_member(ns) / always_measure_member(ns):
 *                        declare the pointer as a struct member
 *                        (`struct <ns>_measure *measure;`)
 * - measure_ptr(ns, nm) / always_measure_ptr(ns, nm):
 *                        declare a file-scope pointer @nm (for a subsystem with
 *                        no handle object, e.g. the bare hash table)
 * counters (monotonic):
 * - measure_inc(m, ev):        m ? m->ev++
 * - measure_add(m, ev, n):     m ? m->ev += n
 * - measure_inc_if(m, c, ev):  (m && c) ? m->ev++   (e.g. a collision test)
 * - measure_inc_at(m, off):    m ? (*(u64 *)((u8 *)m + off))++
 *                        the same increment, naming the counter by its
 *                        offsetof() rather than its member. For a counter
 *                        picked out by a value rather than written down in the
 *                        source: put the offsets in a table indexed by that
 *                        value and the dispatch is a load, where a switch over
 *                        the same values is a branch per arm. The struct is a
 *                        dense array of u64 (DEFINE_MEASURE asserts it), so any
 *                        member's offsetof() is a valid @off.
 * gauges (up and down):
 * - measure_dec(m, ev):        m ? m->ev--
 * - measure_sub(m, ev, n):     m ? m->ev -= n
 * - measure_set(m, ev, v):     m ? m->ev = v        (sample an absolute level)
 * reading back:
 * - measure_get(m, ev):        m ? m->ev : 0        (an rvalue, not an lvalue)
 *
 * @m is a measure pointer or NULL; a NULL measure counts nothing (one
 * predicted-not-taken branch). always_measure_get() is the one to reach for
 * when logic reads a level it also maintains - it is the only accessor whose
 * value a decision can depend on in every build; measure_get() is 0 without
 * CONFIG_MEASURE and so must only feed a report.
 */

#define always_measure_member(_ns)      struct _ns##_measure *measure;
#define always_measure_ptr(_ns, _name)  static struct _ns##_measure *_name _unused;

#define always_measure_inc(_m, _ev) \
	do { if (_m) (_m)->_ev++; } while (0)
#define always_measure_dec(_m, _ev) \
	do { if (_m) (_m)->_ev--; } while (0)
#define always_measure_add(_m, _ev, _n) \
	do { if (_m) (_m)->_ev += (u64)(_n); } while (0)
#define always_measure_sub(_m, _ev, _n) \
	do { if (_m) (_m)->_ev -= (u64)(_n); } while (0)
#define always_measure_set(_m, _ev, _v) \
	do { if (_m) (_m)->_ev = (u64)(_v); } while (0)
#define always_measure_inc_if(_m, _cond, _ev) \
	do { if ((_m) && (_cond)) (_m)->_ev++; } while (0)
#define always_measure_inc_at(_m, _off) \
	do { if (_m) (*(u64 *)((u8 *)(_m) + (_off)))++; } while (0)
#define always_measure_get(_m, _ev) \
	((_m) ? (_m)->_ev : (u64)0)

#ifdef CONFIG_MEASURE

#define measure_member(_ns)       always_measure_member(_ns)
#define measure_ptr(_ns, _name)   always_measure_ptr(_ns, _name)

#define measure_inc(_m, _ev)            always_measure_inc(_m, _ev)
#define measure_dec(_m, _ev)            always_measure_dec(_m, _ev)
#define measure_add(_m, _ev, _n)        always_measure_add(_m, _ev, _n)
#define measure_sub(_m, _ev, _n)        always_measure_sub(_m, _ev, _n)
#define measure_set(_m, _ev, _v)        always_measure_set(_m, _ev, _v)
#define measure_inc_if(_m, _cond, _ev)  always_measure_inc_if(_m, _cond, _ev)
#define measure_inc_at(_m, _off)        always_measure_inc_at(_m, _off)
#define measure_get(_m, _ev)            always_measure_get(_m, _ev)

#else

#define measure_member(_ns)
#define measure_ptr(_ns, _name)
#define measure_inc(_m, _ev)            ((void)0)
#define measure_dec(_m, _ev)            ((void)0)
#define measure_add(_m, _ev, _n)        ((void)0)
#define measure_sub(_m, _ev, _n)        ((void)0)
#define measure_set(_m, _ev, _v)        ((void)0)
#define measure_inc_if(_m, _cond, _ev)  ((void)0)
/*
 * @_off is discarded, not dropped: it is an ordinary expression - typically a
 * lookup in a table of offsetof()s - and a table whose only reader vanished
 * with CONFIG_MEASURE is a table the compiler calls unused. Evaluating and
 * throwing it away keeps it referenced and costs nothing, the load being dead
 * from there. The member-name family above cannot do this and does not need
 * to: a member is not an expression a caller had to build.
 */
#define measure_inc_at(_m, _off)        
#define measure_get(_m, _ev)            ((u64)0)

#endif

/*
 * ---------------------------------------------------------------------------
 * History: a ring of snapshots, one row every CONFIG_MEASURE_HISTORY_INTERVAL
 * ---------------------------------------------------------------------------
 *
 * A live measure struct answers "where are we now". A history answers "how did
 * we get here": it keeps the last CONFIG_MEASURE_HISTORY snapshots of one
 * namespace's struct, each stamped with the time it was taken, in a ring that
 * overwrites its oldest row. Counters recorded that way give rates (the
 * difference between two rows over the time between them), gauges give a
 * level over time, and a ratio is recomputed per row by measure_at() exactly
 * as it is on a live struct.
 *
 * The whole thing is caller-owned and self-contained: one object holds both the
 * live struct the subsystem counts through and the rows sampled from it.
 *
 *     DEFINE_MEASURE_HISTORY(slab, slab_hist);      // rows + one live struct
 *
 *     slab_init(&slab, ...);
 *     measure_history_init(slab, &slab_hist, now);  // now: timestamp_t, ms
 *     slab.measure = measure_history_live(&slab_hist);
 *
 * (that last line is the subsystem's ordinary attach point, so it follows
 * whichever member macro the subsystem declared it with - the slab's is
 * measure_member(), which is there only under CONFIG_MEASURE; a subsystem using
 * always_measure_member() can be given a history in any build)
 *
 * and from a loop the application already runs, at whatever rate it happens to
 * turn - the interval is enforced here, not by the caller:
 *
 *     measure_history_tick(slab, &slab_hist, now);  // saves a row when due
 *
 * Reading it back, oldest row first:
 *
 *     measure_history_for_each(&slab_hist, r)
 *         report(measure_history_time(&slab_hist, r),
 *                measure_at(slab, measure_history_at(&slab_hist, r), i));
 *
 * Time is supplied by the caller, as everywhere else in hpc (see slab_gc()):
 * @now is a timestamp_t, milliseconds on a monotonic clock. The header reads no
 * clock of its own, which keeps it free of a time dependency and makes a
 * history exactly as testable as the counters it records.
 *
 * CONFIG_MEASURE_HISTORY is the depth, and 0 is the off switch. A build that
 * keeps no history keeps the whole API: the type still exists and still holds
 * the live struct (it is the attach point, and taking it away would change
 * every subsystem's code with the build), and every macro keeps its name, its
 * arity and its result type while expanding to no code at all. The count is a
 * constant zero, so every loop over rows - the for_each below and any a caller
 * writes against measure_history_count() - is dead code the compiler deletes.
 *
 * A build without CONFIG_MEASURE is such a build. A history is a ring of
 * snapshots of a struct that nothing increments there, so every row it could
 * keep is a copy of zeros: the depth would buy a trend through numbers that
 * never move, at depth * sizeof(struct <ns>_measure) of memory - and for one
 * registry of twelve hundred metrics at the default depth that is most of a
 * megabyte per history. Kconfig says as much by making the depth `depends on
 * MEASURE`, which leaves the symbol undefined rather than zero, so the default
 * below is where the two have to be joined up.
 */

/*
 * build-time knobs; defaulted here so the header stands alone without kconfig.
 * The depth defaults to none without CONFIG_MEASURE for the reason just given,
 * and a build that means to keep a history over always-measured metrics with
 * instrumentation off can still say so by defining the depth outright.
 */
#ifndef CONFIG_MEASURE_HISTORY
#ifdef CONFIG_MEASURE
#define CONFIG_MEASURE_HISTORY 64
#else
#define CONFIG_MEASURE_HISTORY 0
#endif
#endif
#ifndef CONFIG_MEASURE_HISTORY_INTERVAL
#define CONFIG_MEASURE_HISTORY_INTERVAL 10
#endif

/*
 * Milliseconds between rows (the interval is configured in seconds). Defined in
 * every build: it is the default a runtime interval starts from, and a build
 * that keeps no history still has configuration code that reads it.
 */
#define measure_history_period \
	((timestamp_t)CONFIG_MEASURE_HISTORY_INTERVAL * 1000)

/* a file-scope history for namespace @_ns */
#define DEFINE_MEASURE_HISTORY(_ns, _name) \
	static struct _ns##_history _name _unused

/* the same, as a member of the caller's own struct */
#define measure_history_member(_ns, _name)  struct _ns##_history _name;

/* the live struct to attach to the subsystem (slab->measure = ...) */
#define measure_history_live(_h)  (&(_h)->live)

/* iterate held rows, oldest to newest */
#define measure_history_for_each(_h, _i) \
	for (unsigned _i = 0; _i < measure_history_count(_h); _i++)

#if CONFIG_MEASURE_HISTORY > 0

/* rows kept per history */
#define measure_history_depth  ((unsigned)CONFIG_MEASURE_HISTORY)

/*
 * The per-namespace history type, generated by DEFINE_MEASURE alongside the
 * measure struct itself. @live is the struct a subsystem is pointed at, so that
 * attaching a history is the same act as attaching a measure.
 */
#define MEASURE_HISTORY_TYPE(_ns) \
	struct _ns##_history { \
		struct _ns##_measure live;                        /* counted through */ \
		struct _ns##_measure row[measure_history_depth];  /* the ring        */ \
		timestamp_t stamp[measure_history_depth];         /* when each row   */ \
		timestamp_t next;    /* when the next row falls due       */ \
		timestamp_t period;  /* ms between rows; 0 saves on every tick */ \
		unsigned head;       /* where the next row goes           */ \
		unsigned rows;       /* rows filled, up to the depth      */ \
	}

/*
 * Arm a history at @_now, taking the configured interval. It clears the rows
 * and the live struct with them, so it is also how a history is restarted -
 * and why the subsystem is attached after arming, not before: anything counted
 * in between would be wiped. The first row falls due one interval from here.
 */
#define measure_history_init(_ns, _h, _now) do { \
	struct _ns##_history *__h = (_h); \
	memset(__h, 0, sizeof(*__h)); \
	__h->period = measure_history_period; \
	__h->next   = (timestamp_t)(_now) + __h->period; \
} while (0)

/*
 * Override the interval of an armed history, in milliseconds; 0 makes every
 * tick save a row. For a caller whose sampling rate is a runtime decision - the
 * build-time interval is the default, not a limit.
 */
#define measure_history_every(_ns, _h, _ms, _now) do { \
	struct _ns##_history *__h = (_h); \
	__h->period = (timestamp_t)(_ms); \
	__h->next   = (timestamp_t)(_now) + __h->period; \
} while (0)

/* copy the live struct into the ring, stamped @_now, whether or not it is due */
#define measure_history_save(_ns, _h, _now) do { \
	struct _ns##_history *__h = (_h); \
	unsigned __i = __h->head; \
	__h->row[__i]   = __h->live; \
	__h->stamp[__i] = (timestamp_t)(_now); \
	__h->head = (__i + 1) % measure_history_depth; \
	if (__h->rows < measure_history_depth) \
		__h->rows++; \
} while (0)

/*
 * Is the next row due at @_now? tick() asks this itself; it is here for a
 * caller that wants to skip work of its own - assembling the row it would
 * hand to tick() - when no row is going to be taken.
 */
#define measure_history_due(_h, _now) \
	((timestamp_t)(_now) >= (_h)->next)

/*
 * Save a row if the interval has elapsed; evaluates to 1 when one was saved.
 * Call it as often as convenient - every pass of a poll loop, every gc pass -
 * a call that is not due is a compare. A caller that fell behind does not get a
 * burst of catch-up rows: the next row is due one period from the row actually
 * taken, so the ring stays a record of when things were sampled.
 */
#define measure_history_tick(_ns, _h, _now) ({ \
	struct _ns##_history *__t = (_h); \
	timestamp_t __now = (timestamp_t)(_now); \
	int __saved = 0; \
	if (measure_history_due(__t, __now)) { \
		measure_history_save(_ns, __t, __now); \
		__t->next = __now + __t->period; \
		__saved = 1; \
	} \
	__saved; \
})

/* rows held right now, growing to measure_history_depth and staying there */
#define measure_history_count(_h)  ((_h)->rows)

/* ring slot of row @_i counted from the oldest; @_h and @_i are re-evaluated */
#define measure_history_slot(_h, _i) \
	(((_h)->head + measure_history_depth - (_h)->rows + (_i)) \
	 % measure_history_depth)

/* row @_i, oldest first: the struct, and the time it was taken */
#define measure_history_at(_h, _i)    (&(_h)->row[measure_history_slot(_h, _i)])
#define measure_history_time(_h, _i)  ((_h)->stamp[measure_history_slot(_h, _i)])

#else /* CONFIG_MEASURE_HISTORY == 0: the API stays, the code goes */

#define measure_history_depth  0u

/* the attach point survives; the ring and its bookkeeping do not */
#define MEASURE_HISTORY_TYPE(_ns) \
	struct _ns##_history { \
		struct _ns##_measure live;    /* counted through */ \
	}

/* restarting a history still clears what is being counted through it */
#define measure_history_init(_ns, _h, _now) \
	do { memset((_h), 0, sizeof(*(_h))); (void)(_now); } while (0)

#define measure_history_every(_ns, _h, _ms, _now)  ((void)0)
#define measure_history_save(_ns, _h, _now)        ((void)0)
#define measure_history_due(_h, _now)              (0)

/*
 * A tick is written as a statement wherever a caller has a loop (the rows it
 * would save are what it wants, not the count), so the rows-saved result has to
 * come from something the compiler accepts unused: a plain `(0);` is a
 * statement with no effect, and -Wunused-value says so at every call site.
 */
static inline int measure_history_off(void) { return 0; }
#define measure_history_tick(_ns, _h, _now) \
	((void)(_h), (void)(_now), measure_history_off())
#define measure_history_count(_h)                  (0u)
#define measure_history_slot(_h, _i)               (0u)
/*
 * Type-correct for a loop body, unreachable behind the count: every read of a
 * row is (or must be) guarded by measure_history_count(), which is zero here,
 * so what these return is never seen - they exist so the guarded code still
 * compiles, and then disappears with the loop around it.
 */
#define measure_history_at(_h, _i)                 (&(_h)->live)
#define measure_history_time(_h, _i)               ((timestamp_t)0)

#endif

/*
 * Per-field difference @_new - @_old over the stored fields, the rate side of a
 * history: two rows and the milliseconds between their stamps give counters per
 * second. Gauges subtract too, giving the change in level; a ratio is not
 * stored and is read from @_dst with measure_at() as usual.
 */
static inline void
measure_diff(u64 *dst, const u64 *new_, const u64 *old, unsigned nfield)
{
	for (unsigned i = 0; i < nfield; i++)
		dst[i] = new_[i] - old[i];
}
#define measure_delta(_ns, _dst, _new, _old) \
	measure_diff((u64 *)(_dst), (const u64 *)(_new), (const u64 *)(_old), \
	             measure_nfield(_ns))

/*
 * The two generators the sparse table is built from: one byte per metric, in the
 * order of the metric table, so a reader indexes both with the same i.
 *
 * Variadic because they stand in for all four generators and a ratio carries two
 * field names between its own name and its description.
 */
#define MEASURE_DENSE(...)	0,
#define MEASURE_SPARSE(...)	1,

/*
 * @_list is the four-generator list. Declared beside the namespace it belongs
 * to, and read through measure_sparse() below.
 */
#define DEFINE_MEASURE_SPARSE(_ns, _list) \
	static const u8 _ns##_measure_sparse[] _unused = { \
		_list(_ns, MEASURE_DENSE, MEASURE_DENSE, MEASURE_DENSE, \
		      MEASURE_SPARSE) \
	}

/* the table of a namespace, by name, for a caller that has the namespace and
 * not the array */
#define measure_sparse(_ns)	(_ns##_measure_sparse)

/*
 * The local pointer a function counts through: DECLARE_MEASURE_SECTION(ns,
 * &f->m, m) and then measure_inc(m, ...) for the rest of the body.
 *
 * It is declared in every build, CONFIG_MEASURE or not, because the off side
 * of the namespaced counting macros still names its arguments - ((void)sizeof
 * (_m)) - so that a variable read only by a counter does not become unused
 * when the counting goes away. That only works if the name they are given
 * exists, so this one has to. The struct is defined by DEFINE_MEASURE in
 * either build and the pointer is _unused and never dereferenced without
 * CONFIG_MEASURE, so nothing is emitted for it.
 */
#define DECLARE_MEASURE_SECTION(_ns, _ptr, _v) \
	struct _ns##_measure *_v _unused = (_ptr)

#endif
