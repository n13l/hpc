/*
 * Test and benchmark for hpc/sort/introsort.h
 *
 * Apples-to-apples comparison of two array sorts over the SAME data set and
 * the SAME memory layout:
 *
 *   1. intro_sort  from <hpc/sort/introsort.h>, comparator inlined
 *   2. qsort       from libc, comparator through a function pointer
 *
 * Both sorts receive a copy of the identical input array, so the only
 * variable is the algorithm and the cost of the indirect compare call.
 * Two layouts are swept because they stress different things:
 *
 *   - u64 keys      : 8-byte elements, compare is a register op, every
 *                     element move is cheap; this isolates the call
 *                     overhead qsort pays per compare.
 *   - person *      : 8-byte pointers to 96-byte records; each compare
 *                     dereferences two pointers into a large working set,
 *                     so cache misses dominate and the algorithms' access
 *                     patterns matter more than the compare itself.
 *
 * Reports wall-clock milliseconds for each and the qsort/intro ratio.
 * With one argument it runs a single N, otherwise the default sweep.
 */

#include "sort_bench.h"
#include <hpc/sort/introsort.h>

/* ---- comparators ------------------------------------------------------- */

static inline int
cmp_u64(u64 a, u64 b)
{
	return (a > b) - (a < b);
}

static int
cmp_u64_qsort(const void *a, const void *b)
{
	u64 x = *(const u64 *)a, y = *(const u64 *)b;
	return (x > y) - (x < y);
}

static inline int
cmp_person_ptr(struct person *a, struct person *b)
{
	return (a->id > b->id) - (a->id < b->id);
}

static int
cmp_person_qsort(const void *a, const void *b)
{
	const struct person *pa = *(const struct person * const *)a;
	const struct person *pb = *(const struct person * const *)b;
	return (pa->id > pb->id) - (pa->id < pb->id);
}

DEFINE_INTRO_SORT(intro_sort_u64,     u64,             cmp_u64)
DEFINE_INTRO_SORT(intro_sort_persons, struct person *, cmp_person_ptr)

/* ---- checks ------------------------------------------------------------ */

static int
u64_is_sorted(const u64 *a, unsigned n)
{
	for (unsigned i = 1; i < n; i++)
		if (a[i] < a[i - 1])
			return 0;
	return 1;
}

static int
ptr_is_sorted(struct person * const *a, unsigned n)
{
	for (unsigned i = 1; i < n; i++)
		if (a[i]->id < a[i - 1]->id)
			return 0;
	return 1;
}

/* ---- correctness ------------------------------------------------------- */

static int
test_intro_sort_u64(void)
{
	enum { N = 4096 };
	static u64 a[N];

	intro_sort_u64(a, 0);
	a[0] = 42;
	intro_sort_u64(a, 1);
	if (a[0] != 42) return -1;

	{
		u64 input[] = { 7, 3, 5, 1, 6, 2, 4, 0, 9, 8 };
		unsigned n = sizeof(input) / sizeof(input[0]);
		memcpy(a, input, sizeof(input));
		intro_sort_u64(a, n);
		if (!u64_is_sorted(a, n)) return -1;
		for (unsigned i = 0; i < n; i++)
			if (a[i] != i) return -1;
	}

	/* ascending, descending, all-equal, random: the introsort branches
	 * (insertion leaves, quicksort body, heapsort fallback) all fire */
	for (unsigned i = 0; i < N; i++) a[i] = i;
	intro_sort_u64(a, N);
	if (!u64_is_sorted(a, N)) return -1;

	for (unsigned i = 0; i < N; i++) a[i] = N - i;
	intro_sort_u64(a, N);
	if (!u64_is_sorted(a, N)) return -1;

	for (unsigned i = 0; i < N; i++) a[i] = 7;
	intro_sort_u64(a, N);
	if (!u64_is_sorted(a, N)) return -1;

	rng_seed(0x123456789abcdef0ull);
	for (unsigned i = 0; i < N; i++) a[i] = xrand();
	intro_sort_u64(a, N);
	if (!u64_is_sorted(a, N)) return -1;

	/* organ pipe and sawtooth are the classic quicksort killers */
	for (unsigned i = 0; i < N; i++) a[i] = (i < N / 2) ? i : N - i;
	intro_sort_u64(a, N);
	if (!u64_is_sorted(a, N)) return -1;

	for (unsigned i = 0; i < N; i++) a[i] = i % 16;
	intro_sort_u64(a, N);
	if (!u64_is_sorted(a, N)) return -1;

	return 0;
}

static int
test_intro_sort_ptr(void)
{
	enum { N = 1024 };
	static struct person  pool[N];
	static struct person *arr[N];

	intro_sort_persons(arr, 0);
	pool[0].id = 42;
	arr[0] = &pool[0];
	intro_sort_persons(arr, 1);
	if (arr[0]->id != 42) return -1;

	{
		u64 input[] = { 7, 3, 5, 1, 6, 2, 4, 0, 9, 8 };
		unsigned n = sizeof(input) / sizeof(input[0]);
		for (unsigned i = 0; i < n; i++) {
			pool[i].id = input[i];
			arr[i]     = &pool[i];
		}
		intro_sort_persons(arr, n);
		if (!ptr_is_sorted(arr, n)) return -1;
	}

	for (unsigned i = 0; i < N; i++) { pool[i].id = i; arr[i] = &pool[i]; }
	intro_sort_persons(arr, N);
	if (!ptr_is_sorted(arr, N)) return -1;

	for (unsigned i = 0; i < N; i++) { pool[i].id = N - i; arr[i] = &pool[i]; }
	intro_sort_persons(arr, N);
	if (!ptr_is_sorted(arr, N)) return -1;

	for (unsigned i = 0; i < N; i++) { pool[i].id = 7; arr[i] = &pool[i]; }
	intro_sort_persons(arr, N);
	if (!ptr_is_sorted(arr, N)) return -1;

	rng_seed(0x123456789abcdef0ull);
	for (unsigned i = 0; i < N; i++) {
		pool[i].id = xrand();
		arr[i]     = &pool[i];
	}
	intro_sort_persons(arr, N);
	if (!ptr_is_sorted(arr, N)) return -1;

	return 0;
}

/* ---- benchmark --------------------------------------------------------- */

static void
run_benchmark_at(unsigned n)
{
	u64            *keys_q = xcalloc(n, sizeof(*keys_q));
	u64            *keys_i = xcalloc(n, sizeof(*keys_i));
	struct person  *pool   = xcalloc(n, sizeof(*pool));
	struct person **arr_q  = xcalloc(n, sizeof(*arr_q));
	struct person **arr_i  = xcalloc(n, sizeof(*arr_i));
	struct bench bq, bi;

	/* identical input for every run */
	rng_seed(0xdeadbeefcafef00dull);
	for (unsigned i = 0; i < n; i++)
		fill_person(&pool[i]);
	for (unsigned i = 0; i < n; i++) {
		keys_q[i] = keys_i[i] = pool[i].id;
		arr_q[i]  = arr_i[i]  = &pool[i];
	}

	/* layout 1: flat u64 keys */
	bench_begin(&bq);
	qsort(keys_q, n, sizeof(*keys_q), cmp_u64_qsort);
	bench_end(&bq);
	if (!u64_is_sorted(keys_q, n)) {
		fprintf(stderr, "qsort produced unsorted u64 array at n=%u\n", n);
		exit(1);
	}

	bench_begin(&bi);
	intro_sort_u64(keys_i, n);
	bench_end(&bi);
	if (!u64_is_sorted(keys_i, n)) {
		fprintf(stderr, "intro_sort produced unsorted u64 array at n=%u\n", n);
		exit(1);
	}
	if (memcmp(keys_q, keys_i, (size_t)n * sizeof(*keys_q))) {
		fprintf(stderr, "u64 sorts disagree at n=%u\n", n);
		exit(1);
	}

	double u64_q = bench_ms(&bq), u64_i = bench_ms(&bi);

	/* layout 2: pointers into a 96-byte record pool */
	bench_begin(&bq);
	qsort(arr_q, n, sizeof(*arr_q), cmp_person_qsort);
	bench_end(&bq);
	if (!ptr_is_sorted(arr_q, n)) {
		fprintf(stderr, "qsort produced unsorted pointer array at n=%u\n", n);
		exit(1);
	}

	bench_begin(&bi);
	intro_sort_persons(arr_i, n);
	bench_end(&bi);
	if (!ptr_is_sorted(arr_i, n)) {
		fprintf(stderr, "intro_sort produced unsorted pointer array at n=%u\n", n);
		exit(1);
	}

	double ptr_q = bench_ms(&bq), ptr_i = bench_ms(&bi);

	printf(" %9u  %10.3f  %10.3f  %6.2fx  %10.3f  %10.3f  %6.2fx\n",
	       n, u64_q, u64_i, u64_i > 0 ? u64_q / u64_i : 0.0,
	          ptr_q, ptr_i, ptr_i > 0 ? ptr_q / ptr_i : 0.0);

	free(arr_i);
	free(arr_q);
	free(pool);
	free(keys_i);
	free(keys_q);
}

int
main(int argc, char **argv)
{
	if (test_intro_sort_u64() < 0) {
		fprintf(stderr, "intro_sort u64        FAIL\n");
		return 1;
	}
	if (test_intro_sort_ptr() < 0) {
		fprintf(stderr, "intro_sort person *   FAIL\n");
		return 1;
	}

	printf("                   ---- u64 keys (ms) ----    ---- person * (ms) ----\n");
	printf("        N        qsort       intro   ratio       qsort       intro   ratio\n");

	if (argc > 1) {
		run_benchmark_at((unsigned)strtoul(argv[1], NULL, 10));
		return 0;
	}
	for (unsigned i = 0; i < SORT_BENCH_NSIZES; i++)
		run_benchmark_at(sort_bench_sizes[i]);
	return 0;
}
