#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_predicate.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <hpc/compiler.h>
#include <hpc/log.h>
#include <hpc/list.h>
#include <hpc/slist.h>

TEST_CASE("Define double and circular linked list", "bsd/list") {
	DEFINE_LIST(double_linked);
	REQUIRE(double_linked.head.prev == &double_linked.head);
	REQUIRE(double_linked.head.next == &double_linked.head);
}

TEST_CASE("Declare double and circular linked list", "bsd/list") {
	DECLARE_LIST(double_linked);
	list_init(&double_linked);
	REQUIRE(double_linked.head.prev == &double_linked.head);
	REQUIRE(double_linked.head.next == &double_linked.head);
}

TEST_CASE("Double linked intrusive list works as expected", "bsd/list") {
	DEFINE_LIST(list);
	
	struct data { unsigned int id, num; struct node n; };

	std::vector<struct data> data {
		{1, 1}, {2, 1}, {3, 1}, {4, 1}, {5, 1}, {6, 1}
	};

	unsigned count = 0;
	struct node *it, *tmp;

	list_walk(list, it) { count++; }
	REQUIRE(count == 0);
	list_walk_delsafe(list, it, tmp) { count++; }
	REQUIRE(count == 0);
	list_for_each(list, it, struct data, n) { count++; }
	REQUIRE(count == 0);
	list_for_each_delsafe(list, it, struct data, n) { count++; }
	REQUIRE(count == 0);
	list_walk_reverse(list, it) { count++; }
	REQUIRE(count == 0);
	list_walk_reverse_delsafe(list, it, tmp) { count++; }
	REQUIRE(count == 0);
	REQUIRE(list_empty(&list));

	for (auto& x: data)
		list_add(&list, &x.n);

	count = 0; list_walk(list, it) {
		struct data *x = container_of(it, struct data, n);
		REQUIRE(x->id == data[data.size() - count - 1].id);
		REQUIRE(x->num == data[data.size() - count - 1].num);
		count++;
	}
	REQUIRE(count == 6);

	count = 0; list_walk_delsafe(list, it, tmp) {
		struct data *x = container_of(it, struct data, n);
		REQUIRE(x->id == data[data.size() - count - 1].id);
		REQUIRE(x->num == data[data.size() - count - 1].num);
		count++;
	}
	REQUIRE(count == 6);

	count = 0; list_for_each(list, it, struct data, n) {
		REQUIRE(it->id == data[data.size() - count - 1].id);
		REQUIRE(it->num == data[data.size() - count - 1].num);
		count++;
	}
	REQUIRE(count == 6);

	count = 0; list_for_each_delsafe(list, it, struct data, n) {
		REQUIRE(it->id == data[data.size() - count - 1].id);
		REQUIRE(it->num == data[data.size() - count - 1].num);
		count++;
	}
	REQUIRE(count == 6);

	count = 0; list_walk_reverse(list, it) {
		struct data *x = container_of(it, struct data, n);
		REQUIRE(x->id == data[count].id);
		REQUIRE(x->num == data[count].num);
		count++;
	}
	REQUIRE(count == 6);

	count = 0; list_walk_reverse_delsafe(list, it, tmp) {
		struct data *x = container_of(it, struct data, n);
		REQUIRE(x->id == data[count].id);
		REQUIRE(x->num == data[count].num);
		count++;
	}

	REQUIRE(count == 6);

	for (auto& x: data)
		list_del(&x.n);

	count = 0; list_walk(list, it) { count++; }
	REQUIRE(count == 0);
	count = 0; list_walk_delsafe(list, it, tmp) { count++; }
	REQUIRE(count == 0);
	count = 0; list_for_each(list, it, struct data, n) { count++; }
	REQUIRE(count == 0);
	count = 0; list_for_each_delsafe(list, it, struct data, n) { count++; }
	REQUIRE(count == 0);
	count = 0; list_walk_reverse(list, it) { count++; }
	REQUIRE(count == 0);
	count = 0; list_walk_reverse_delsafe(list, it, tmp) { count++; }
	REQUIRE(count == 0);
	REQUIRE(list_empty(&list));

	for (auto& x: data)
		list_add(&list, &x.n);
	REQUIRE(!list_empty(&list));
	count = 0; list_walk_delsafe(list, it, tmp) { list_del(it); count++; }
	REQUIRE(count == 6);
	REQUIRE(list_empty(&list));

	for (auto& x: data)
		list_add(&list, &x.n);
	REQUIRE(!list_empty(&list));
	count = 0; list_walk_reverse_delsafe(list, it, tmp) {
		list_del(it); count++;
	}
	REQUIRE(count == 6);
	REQUIRE(list_empty(&list));

	for (auto& x: data)
		list_add(&list, &x.n);
	REQUIRE(!list_empty(&list));
	count = 0; list_for_each_delsafe(list, it, struct data, n) {
		list_del(&it->n); count++;
	}
	REQUIRE(count == 6);
	REQUIRE(list_empty(&list));

	for (auto& x: data)
		list_add(&list, &x.n);
	REQUIRE(!list_empty(&list));
	count = 0; list_for_each_reverse_delsafe(list, it, struct data, n) {
		list_del(&it->n); count++;
	}
	REQUIRE(count == 6);
	REQUIRE(list_empty(&list));

}

