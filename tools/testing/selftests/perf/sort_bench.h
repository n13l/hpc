/*
 * Shared scaffolding for the sort benchmarks in this directory
 *
 * The sort_merge and sort_intro programs sort the SAME record type from the
 * SAME deterministic random stream, so their numbers can be read side by
 * side. Everything they share lives here: the record, the timers, the RNG
 * and the record filler.
 */

#ifndef __HPC_SELFTESTS_PERF_SORT_BENCH_H__
#define __HPC_SELFTESTS_PERF_SORT_BENCH_H__

#include <hpc/compiler.h>
#include <hpc/list.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

/* A record with a sort key, an intrusive link and enough payload that the
 * working set leaves L1/L2 well before the end of the size sweep. */
struct person {
	u64         id;
	struct node link;
	char        name[64];
	unsigned    age;
};

static inline u64
ns_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

static inline u64
rdtsc_now(void)
{
#if defined(__x86_64__) || defined(__i386__)
	unsigned lo, hi;
	__asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
	return ((u64)hi << 32) | lo;
#elif defined(__aarch64__)
	u64 v;
	__asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(v));
	return v;
#else
	return ns_now();
#endif
}

struct bench {
	u64 ns;
	u64 cycles;
};

static inline void
bench_begin(struct bench *b)
{
	b->ns     = ns_now();
	b->cycles = rdtsc_now();
}

static inline void
bench_end(struct bench *b)
{
	b->cycles = rdtsc_now() - b->cycles;
	b->ns     = ns_now()    - b->ns;
}

static inline double
bench_ms(const struct bench *b)
{
	return (double)b->ns / 1.0e6;
}

/* xorshift64* - fast, deterministic, good enough for sort input */
static u64 rng_state;

static inline void
rng_seed(u64 seed)
{
	rng_state = seed ? seed : 0x9e3779b97f4a7c15ull;
}

static inline u64
xrand(void)
{
	u64 x = rng_state;
	x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
	rng_state = x;
	return x * 2685821657736338717ull;
}

static inline void
fill_person(struct person *p)
{
	p->id  = xrand();
	p->age = (unsigned)(xrand() % 120u);
	snprintf(p->name, sizeof(p->name), "person-%08x", (unsigned)xrand());
}

static inline void *
xcalloc(size_t n, size_t size)
{
	void *p = calloc(n, size);
	if (!p) {
		fprintf(stderr, "out of memory (%zu x %zu bytes)\n", n, size);
		exit(1);
	}
	return p;
}

/* Default size sweep: L1 -> beyond L3 for a 96-byte record. */
static const unsigned sort_bench_sizes[] = {
	1000, 10000, 50000, 100000, 250000, 500000,
	1000000, 2000000, 5000000, 10000000
};

#define SORT_BENCH_NSIZES \
	(sizeof(sort_bench_sizes) / sizeof(sort_bench_sizes[0]))

#endif /* __HPC_SELFTESTS_PERF_SORT_BENCH_H__ */
