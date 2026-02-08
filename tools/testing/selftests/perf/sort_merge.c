/*
 * Test and benchmark for hpc/sort/merge.h
 *
 * Apples-to-apples comparison of two sort paths over the SAME data set:
 *
 *   1. merge_sort()  from <hpc/sort/merge.h> over an intrusive doubly
 *   2. qsort()       from libc over a pre-built pointer array
 *
 * Each algorithm is given the data structure it is natively designed for:
 *   - merge_sort gets the list it would sort in place,
 *   - qsort gets a flat pointer array prepared up-front.
 *
 * The pointer array population is NOT counted in qsort's time (it is a
 * one-off setup cost that the caller would amortise).
 *
 * Reports wall-clock time and CPU cycles for each, plus the merge/qsort
 * ratio. Sweep over a range of N covering L1 -> beyond L3.
 */

#include <hpc/compiler.h>
#include <hpc/list.h>
#include <hpc/sort/merge.h>
#include <hpc/sort/introsort.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

struct person {
	u64    id;
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

static int
cmp_person(struct person *a, struct person *b)
{
	if (a->id < b->id) return -1;
	if (a->id > b->id) return  1;
	return 0;
}

static int
cmp_person_qsort(const void *a, const void *b)
{
	const struct person *pa = *(const struct person * const *)a;
	const struct person *pb = *(const struct person * const *)b;
	if (pa->id < pb->id) return -1;
	if (pa->id > pb->id) return  1;
	return 0;
}

static inline int
cmp_person_ptr(struct person *a, struct person *b)
{
	if (a->id < b->id) return -1;
	if (a->id > b->id) return  1;
	return 0;
}

DEFINE_INTRO_SORT(intro_sort_persons, struct person *, cmp_person_ptr)

static u64 rng_state;

static inline u64
xrand(void)
{
	u64 x = rng_state;
	x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
	rng_state = x;
	return x * 2685821657736338717ull;
}

static void
fill_person(struct person *p)
{
	p->id  = xrand();
	p->age = (unsigned)(xrand() % 120u);
	snprintf(p->name, sizeof(p->name), "person-%08x", (unsigned)xrand());
}

static int
list_is_sorted(struct list *list)
{
	u64 last = 0;
	int first = 1;
	list_for_each(*list, it, struct person, link) {
		if (!first && it->id < last)
			return 0;
		last = it->id;
		first = 0;
	}
	return 1;
}

static int
array_is_sorted(struct person **a, unsigned n)
{
	for (unsigned i = 1; i < n; i++)
		if (a[i]->id < a[i - 1]->id)
			return 0;
	return 1;
}

struct bench {
	u64 ns;
	u64 cycles;
};

static struct bench
bench_merge_sort(struct list *list)
{
	struct bench b;
	u64 t0 = ns_now();
	u64 c0 = rdtsc_now();
	merge_sort(list, cmp_person, struct person, link);
	u64 c1 = rdtsc_now();
	u64 t1 = ns_now();
	b.ns     = t1 - t0;
	b.cycles = c1 - c0;
	return b;
}

static struct bench
bench_qsort(struct person **arr, unsigned n)
{
	struct bench b;
	u64 t0 = ns_now();
	u64 c0 = rdtsc_now();
	qsort(arr, n, sizeof(*arr), cmp_person_qsort);
	u64 c1 = rdtsc_now();
	u64 t1 = ns_now();
	b.ns     = t1 - t0;
	b.cycles = c1 - c0;
	return b;
}

static struct bench
bench_intro_sort(struct person **arr, unsigned n)
{
	struct bench b;
	u64 t0 = ns_now();
	u64 c0 = rdtsc_now();
	intro_sort_persons(arr, n);
	u64 c1 = rdtsc_now();
	u64 t1 = ns_now();
	b.ns     = t1 - t0;
	b.cycles = c1 - c0;
	return b;
}

static int
test_merge_range(void)
{
	struct person a0 = { .id = 1 }, a1 = { .id = 3 }, a2 = { .id = 5 };
	struct person b0 = { .id = 2 }, b1 = { .id = 4 }, b2 = { .id = 6 };

	a0.link.next = &a1.link; a1.link.next = &a2.link; a2.link.next = NULL;
	b0.link.next = &b1.link; b1.link.next = &b2.link; b2.link.next = NULL;

	struct node *m = merge_range_asc(&a0.link, &b0.link,
	                                 cmp_person, struct person, link);
	u64 expected[] = { 1, 2, 3, 4, 5, 6 };
	unsigned i = 0;
	for (struct node *p = m; p; p = p->next) {
		struct person *it = container_of(p, struct person, link);
		if (it->id != expected[i++])
			return -1;
	}
	return i == 6 ? 0 : -1;
}

static int
test_merge_sort_range(void)
{
	struct person it[8];
	u64 input[] = { 7, 3, 5, 1, 6, 2, 4, 0 };
	unsigned n = sizeof(input) / sizeof(input[0]);

	for (unsigned i = 0; i < n; i++) {
		memset(&it[i], 0, sizeof(it[i]));
		it[i].id = input[i];
		it[i].link.next = (i + 1 < n) ? &it[i + 1].link : NULL;
	}

	struct node *head = &it[0].link;
	merge_sort_range_asc(head, cmp_person, struct person, link);

	u64 last = 0;
	int first = 1;
	for (struct node *p = head; p; p = p->next) {
		struct person *cur = container_of(p, struct person, link);
		if (!first && cur->id < last)
			return -1;
		last = cur->id;
		first = 0;
	}
	return 0;
}

static int
check_sorted(struct person **arr, unsigned n)
{
	for (unsigned i = 1; i < n; i++)
		if (arr[i]->id < arr[i - 1]->id)
			return -1;
	return 0;
}

static int
test_intro_sort(void)
{
	enum { N = 1024 };
	struct person  pool[N];
	struct person *arr[N];

	/* n=0 / n=1 must not crash and must leave the array consistent */
	intro_sort_persons(arr, 0);
	pool[0].id = 42;
	arr[0] = &pool[0];
	intro_sort_persons(arr, 1);
	if (arr[0]->id != 42) return -1;

	{
		u64 input[] = { 7, 3, 5, 1, 6, 2, 4, 0, 9, 8 };
		unsigned int n = sizeof(input) / sizeof(input[0]);
		for (unsigned i = 0; i < n; i++) {
			pool[i].id = input[i];
			arr[i]     = &pool[i];
		}
		intro_sort_persons(arr, n);
		if (check_sorted(arr, n) < 0) return -1;
	}

	for (unsigned i = 0; i < N; i++) { pool[i].id = i; arr[i] = &pool[i]; }
	intro_sort_persons(arr, N);
	if (check_sorted(arr, N) < 0) return -1;

	for (unsigned i = 0; i < N; i++) { pool[i].id = N - i; arr[i] = &pool[i]; }
	intro_sort_persons(arr, N);
	if (check_sorted(arr, N) < 0) return -1;

	for (unsigned i = 0; i < N; i++) { pool[i].id = 7; arr[i] = &pool[i]; }
	intro_sort_persons(arr, N);
	if (check_sorted(arr, N) < 0) return -1;

	rng_state = 0x123456789abcdef0ull;
	for (unsigned i = 0; i < N; i++) {
		pool[i].id = xrand();
		arr[i]     = &pool[i];
	}
	intro_sort_persons(arr, N);
	if (check_sorted(arr, N) < 0) return -1;

	return 0;
}

static void run_benchmark_at(unsigned int n);

int
main(int argc, char **argv)
{
	if (test_merge_range() < 0) {
		fprintf(stderr, "merge_range_asc      FAIL\n");
		return 1;
	}
	if (test_merge_sort_range() < 0) {
		fprintf(stderr, "merge_sort_range_asc FAIL\n");
		return 1;
	}
	if (test_intro_sort() < 0) {
		fprintf(stderr, "intro_sort            FAIL\n");
		return 1;
	}
	if (argc > 1) {
		run_benchmark_at((unsigned)strtoul(argv[1], NULL, 10));
		return 0;
	}

	printf("        N           KB    merge (ms)   qsort (ms)   intro (ms)\n");

	static const unsigned sizes[] = {
		1000, 10000, 50000, 100000, 250000, 500000,
		1000000, 2000000, 5000000, 10000000
	};
	for (unsigned i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++)
		run_benchmark_at(sizes[i]);
	return 0;
}

static void
run_benchmark_at(unsigned int n)
{
	struct person  *pool_a = (struct person *)calloc(n, sizeof(*pool_a));
	struct person  *pool_b = (struct person *)calloc(n, sizeof(*pool_b));
	struct person **arr_q  = (struct person **)calloc(n, sizeof(*arr_q));
	struct person **arr_i  = (struct person **)calloc(n, sizeof(*arr_i));
	if (!pool_a || !pool_b || !arr_q || !arr_i) {
		fprintf(stderr, "out of memory at n=%u\n", n);
		exit(1);
	}

	/* identical input for all three runs */
	rng_state = 0xdeadbeefcafef00dull;
	for (unsigned i = 0; i < n; i++)
		fill_person(&pool_a[i]);
	memcpy(pool_b, pool_a, (size_t)n * sizeof(*pool_a));

	/* ---- run 1: merge_sort over an intrusive doubly linked list ---- */
	DEFINE_LIST(list);
	for (unsigned i = 0; i < n; i++)
		list_add(&list, &pool_a[i].link);
	struct bench bm = bench_merge_sort(&list);
	if (!list_is_sorted(&list)) {
		fprintf(stderr, "merge_sort produced unsorted list at n=%u\n", n);
		exit(1);
	}

	/* run 2: qsort over a prepared pointer array
	 *
	 * The pointer array is built BEFORE the timing window because that
	 * is what an application using qsort would have prepared once and
	 * then sorted many times. pool_b is the unsorted twin of pool_a so
	 * qsort really sees randomised input. */
	for (unsigned i = 0; i < n; i++)
		arr_q[i] = &pool_b[i];
	struct bench bq = bench_qsort(arr_q, n);
	if (!array_is_sorted(arr_q, n)) {
		fprintf(stderr, "qsort produced unsorted array at n=%u\n", n);
		exit(1);
	}

	/* run 3: intro_sort over the SAME prepared pointer array
	 *
	 * Same data, same layout as qsort -- the only difference is that
	 * the comparator is inlined into the sort body (no indirect call
	 * per compare). This isolates the cost of qsort's function-pointer
	 * indirection. */
	for (unsigned i = 0; i < n; i++)
		arr_i[i] = &pool_b[i];
	struct bench bi = bench_intro_sort(arr_i, n);
	if (!array_is_sorted(arr_i, n)) {
		fprintf(stderr, "intro_sort produced unsorted array at n=%u\n", n);
		exit(1);
	}

	double list_kb = (double)n * sizeof(struct person) / 1024.0;
	printf(" %9u  %10.1f  %10.3f  %11.3f  %11.3f\n", 
	       n, list_kb, bm.ns / 1.0e6, bq.ns / 1.0e6, bi.ns / 1.0e6);

	free(arr_i);
	free(arr_q);
	free(pool_a);
	free(pool_b);
}
