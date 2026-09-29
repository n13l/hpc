/*
 * Test and benchmark for hpc/sort/merge.h
 *
 * The question this answers: "I have a linked list of records and I want
 * it sorted" - what does each approach cost end to end?
 *
 *   1. merge_sort  from <hpc/sort/merge.h> over an INTRUSIVE doubly linked
 *                  list. The records carry their own struct node, the sort
 *                  relinks them in place, no allocation, no copy.
 *
 *   2. qsort       from libc over a NON-INTRUSIVE list: a chain of small
 *                  nodes that each point at a record. qsort cannot sort a
 *                  list, so the caller has to gather the record pointers
 *                  into an array, sort that, and rewrite the node chain in
 *                  the sorted order. All three steps are timed, because all
 *                  three are what the caller pays; the pure qsort() call is
 *                  reported separately so the list overhead is visible.
 *
 * Both lists hold the SAME records in the SAME order, built from the same
 * deterministic random stream. Reports wall-clock milliseconds and the
 * qsort/merge ratio. With one argument it runs a single N, otherwise the
 * default sweep from L1 to beyond L3.
 */

#include "sort_bench.h"
#include <hpc/sort/merge.h>

/* The non-intrusive list: the record does not know it is on a list. */
struct pnode {
	struct pnode  *next;
	struct person *item;
};

/* ---- comparators ------------------------------------------------------- */

static int
cmp_person(struct person *a, struct person *b)
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

/* ---- checks ------------------------------------------------------------ */

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
plist_is_sorted(const struct pnode *head)
{
	for (const struct pnode *p = head; p && p->next; p = p->next)
		if (p->next->item->id < p->item->id)
			return 0;
	return 1;
}

/* Both lists must end up as the same sequence of ids. */
static int
lists_agree(struct list *list, const struct pnode *head)
{
	const struct pnode *p = head;
	list_for_each(*list, it, struct person, link) {
		if (!p || p->item->id != it->id)
			return 0;
		p = p->next;
	}
	return p == NULL;
}

/* ---- correctness ------------------------------------------------------- */

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
		if (i >= 6 || it->id != expected[i++])
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
	unsigned seen = 0;
	for (struct node *p = head; p; p = p->next) {
		struct person *cur = container_of(p, struct person, link);
		if (!first && cur->id < last)
			return -1;
		last = cur->id;
		first = 0;
		seen++;
	}
	return seen == n ? 0 : -1;
}

/* Whole-list sort through the public entry point, including the cases a
 * circular list makes special: empty, single, already sorted, reversed,
 * all equal, and random. After each sort the prev links must still form
 * a ring, or list_del on a sorted list would corrupt it. */
static int
list_ring_ok(struct list *list, unsigned expect)
{
	unsigned n = 0;
	struct node *head = &list->head;
	for (struct node *p = head->next; p != head; p = p->next) {
		if (p->next->prev != p || p->prev->next != p)
			return 0;
		n++;
	}
	return n == expect && head->next->prev == head && head->prev->next == head;
}

static int
test_merge_sort_list(void)
{
	enum { N = 1024 };
	struct person *pool = xcalloc(N, sizeof(*pool));
	int rc = -1;
	DEFINE_LIST(list);

	merge_sort(&list, cmp_person, struct person, link);
	if (!list_ring_ok(&list, 0)) goto out;

	pool[0].id = 42;
	list_add(&list, &pool[0].link);
	merge_sort(&list, cmp_person, struct person, link);
	if (!list_ring_ok(&list, 1) || !list_is_sorted(&list)) goto out;

	static const char *pattern[] = { "asc", "desc", "equal", "random" };
	for (unsigned k = 0; k < 4; k++) {
		DEFINE_LIST(l);
		rng_seed(0x123456789abcdef0ull);
		for (unsigned i = 0; i < N; i++) {
			switch (k) {
			case 0:  pool[i].id = i;       break;
			case 1:  pool[i].id = N - i;   break;
			case 2:  pool[i].id = 7;       break;
			default: pool[i].id = xrand(); break;
			}
			list_add(&l, &pool[i].link);
		}
		merge_sort(&l, cmp_person, struct person, link);
		if (!list_ring_ok(&l, N) || !list_is_sorted(&l)) {
			fprintf(stderr, "merge_sort list pattern %s\n", pattern[k]);
			goto out;
		}
	}
	rc = 0;
out:
	free(pool);
	return rc;
}

/* ---- benchmark --------------------------------------------------------- */

static void
run_benchmark_at(unsigned n)
{
	struct person  *pool_a = xcalloc(n, sizeof(*pool_a));
	struct person  *pool_b = xcalloc(n, sizeof(*pool_b));
	struct pnode   *nodes  = xcalloc(n, sizeof(*nodes));
	struct person **arr    = xcalloc(n, sizeof(*arr));
	struct bench bm, bq_all, bq_sort;

	/* identical records for both lists */
	rng_seed(0xdeadbeefcafef00dull);
	for (unsigned i = 0; i < n; i++)
		fill_person(&pool_a[i]);
	memcpy(pool_b, pool_a, (size_t)n * sizeof(*pool_a));

	/* run 1: merge_sort over the intrusive list - sort in place */
	DEFINE_LIST(list);
	for (unsigned i = 0; i < n; i++)
		list_add(&list, &pool_a[i].link);

	bench_begin(&bm);
	merge_sort(&list, cmp_person, struct person, link);
	bench_end(&bm);
	if (!list_is_sorted(&list)) {
		fprintf(stderr, "merge_sort produced unsorted list at n=%u\n", n);
		exit(1);
	}

	/* run 2: qsort over the non-intrusive list
	 *
	 * list_add pushes at the front, so the intrusive list runs from
	 * pool_a[n-1] down to pool_a[0]; chain the pnodes the same way so
	 * both sorts see the identical input sequence. */
	struct pnode *head = NULL;
	for (unsigned i = 0; i < n; i++) {
		nodes[i].item = &pool_b[i];
		nodes[i].next = head;
		head = &nodes[i];
	}

	bench_begin(&bq_all);
	unsigned k = 0;
	for (struct pnode *p = head; p; p = p->next)	/* gather */
		arr[k++] = p->item;
	bench_begin(&bq_sort);
	qsort(arr, n, sizeof(*arr), cmp_person_qsort);	/* sort */
	bench_end(&bq_sort);
	k = 0;
	for (struct pnode *p = head; p; p = p->next)	/* relink */
		p->item = arr[k++];
	bench_end(&bq_all);

	if (!plist_is_sorted(head)) {
		fprintf(stderr, "qsort produced unsorted list at n=%u\n", n);
		exit(1);
	}
	if (!lists_agree(&list, head)) {
		fprintf(stderr, "merge_sort and qsort disagree at n=%u\n", n);
		exit(1);
	}

	double kb = (double)n * sizeof(struct person) / 1024.0;
	double m = bench_ms(&bm), qa = bench_ms(&bq_all), qs = bench_ms(&bq_sort);
	printf(" %9u  %10.1f  %10.3f  %11.3f  %11.3f  %6.2fx\n",
	       n, kb, m, qa, qs, m > 0 ? qa / m : 0.0);

	free(arr);
	free(nodes);
	free(pool_b);
	free(pool_a);
}

int
main(int argc, char **argv)
{
	if (test_merge_range() < 0) {
		fprintf(stderr, "merge_range_asc       FAIL\n");
		return 1;
	}
	if (test_merge_sort_range() < 0) {
		fprintf(stderr, "merge_sort_range_asc  FAIL\n");
		return 1;
	}
	if (test_merge_sort_list() < 0) {
		fprintf(stderr, "merge_sort            FAIL\n");
		return 1;
	}

	printf("        N           KB    merge (ms)   qsort (ms)   sort only    ratio\n");

	if (argc > 1) {
		run_benchmark_at((unsigned)strtoul(argv[1], NULL, 10));
		return 0;
	}
	for (unsigned i = 0; i < SORT_BENCH_NSIZES; i++)
		run_benchmark_at(sort_bench_sizes[i]);
	return 0;
}
