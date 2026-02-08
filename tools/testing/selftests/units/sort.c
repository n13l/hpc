#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>
#include <string.h>

#include <hpc/compiler.h>
#include <hpc/list.h>
#include <hpc/sort/merge.h>
#include <hpc/sort/introsort.h>

struct item {
	u64         key;
	struct node link;
};

static int
cmp_item(struct item *a, struct item *b)
{
	if (a->key < b->key) return -1;
	if (a->key > b->key) return  1;
	return 0;
}

static inline int
cmp_item_ptr(struct item *a, struct item *b)
{
	if (a->key < b->key) return -1;
	if (a->key > b->key) return  1;
	return 0;
}

DEFINE_INTRO_SORT(intro_sort_items, struct item *, cmp_item_ptr)

static void
test_merge_range_asc(void **state)
{
	(void)state;
	struct item a0 = { .key = 1 }, a1 = { .key = 3 }, a2 = { .key = 5 };
	struct item b0 = { .key = 2 }, b1 = { .key = 4 }, b2 = { .key = 6 };

	a0.link.next = &a1.link; a1.link.next = &a2.link; a2.link.next = NULL;
	b0.link.next = &b1.link; b1.link.next = &b2.link; b2.link.next = NULL;

	struct node *m = merge_range_asc(&a0.link, &b0.link,
	                                 cmp_item, struct item, link);
	u64 expected[] = { 1, 2, 3, 4, 5, 6 };
	unsigned i = 0;
	for (struct node *p = m; p; p = p->next) {
		struct item *it = container_of(p, struct item, link);
		assert_int_equal(it->key, expected[i]);
		i++;
	}
	assert_int_equal(i, 6);
}

static void
test_merge_sort_range(void **state)
{
	(void)state;
	struct item it[8];
	u64 input[] = { 7, 3, 5, 1, 6, 2, 4, 0 };
	unsigned n = sizeof(input) / sizeof(input[0]);

	for (unsigned i = 0; i < n; i++) {
		memset(&it[i], 0, sizeof(it[i]));
		it[i].key = input[i];
		it[i].link.next = (i + 1 < n) ? &it[i + 1].link : NULL;
	}

	struct node *head = &it[0].link;
	merge_sort_range_asc(head, cmp_item, struct item, link);

	u64 last = 0;
	int first = 1;
	for (struct node *p = head; p; p = p->next) {
		struct item *cur = container_of(p, struct item, link);
		if (!first)
			assert_true(cur->key >= last);
		last = cur->key;
		first = 0;
	}
}

static void
test_merge_sort_list(void **state)
{
	(void)state;
	DEFINE_LIST(list);

	struct item items[8];
	u64 input[] = { 7, 3, 5, 1, 6, 2, 4, 0 };
	unsigned n = sizeof(input) / sizeof(input[0]);

	for (unsigned i = 0; i < n; i++) {
		items[i].key = input[i];
		list_add(&list, &items[i].link);
	}

	merge_sort(&list, cmp_item, struct item, link);

	u64 last = 0;
	int first = 1;
	struct node *it;
	list_walk(list, it) {
		struct item *cur = container_of(it, struct item, link);
		if (!first)
			assert_true(cur->key >= last);
		last = cur->key;
		first = 0;
	}
}

static void
test_intro_sort_basic(void **state)
{
	(void)state;
	enum { N = 64 };
	struct item  pool[N];
	struct item *arr[N];

	/* reverse-sorted input */
	for (unsigned i = 0; i < N; i++) {
		pool[i].key = N - i;
		arr[i] = &pool[i];
	}

	intro_sort_items(arr, N);

	for (unsigned i = 1; i < N; i++)
		assert_true(arr[i]->key >= arr[i - 1]->key);
}

int
main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_merge_range_asc),
		cmocka_unit_test(test_merge_sort_range),
		cmocka_unit_test(test_merge_sort_list),
		cmocka_unit_test(test_intro_sort_basic),
	};
	return cmocka_run_group_tests_name("sort", tests, NULL, NULL);
}
