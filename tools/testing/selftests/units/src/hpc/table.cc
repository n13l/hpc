#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_predicate.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <hpc/compiler.h>
#include <hpc/log.h>
#include <hpc/hash/fn.h>
#include <hpc/hash/table.h>
#include <tests/src/hpc/category.h>
#include <array>

TEST_CASE("Run checks related hashtable", "bsd/hashtable") {
	DECLARE_HASHTABLE(table, 9);
	hash_init_table(table, 9);
	REQUIRE(hash_empty(table, 0));

	struct data { unsigned int id, hash; struct qnode q; };
	std::vector<struct data> data {
		{1, 0}, {2, 0}, {3, 0}, {4, 0}, {5, 0}, {6, 0}
	};

	REQUIRE(hash_empty(table, 0));
	for (auto& x: data)
		hash_add(table, &x.q, x.hash);
	REQUIRE(!hash_empty(table, 0));

	unsigned depth_max = 0;
	hash_for_each(table, 0, it, struct data, q)
		depth_max++;
	REQUIRE(depth_max == 6);
}

TEST_CASE("Uniform hash sequence distribution works", "bsd/hashtable") {
	DECLARE_HASHTABLE(table, 8);
	hash_init_table(table, 8);

	struct data { unsigned int id, hash, seqno; struct qnode q; };
	std::vector<struct data> data(512);
	unsigned seqno = 0, hits = 0;
	for (auto& x: data) {
		x.seqno = seqno++;
		x.hash = hash_seq(x.seqno, 8);
		hash_add(table, &x.q, x.hash);
	}

	hits = 0;
	for (auto& x: data) {
		unsigned depth_max = 0;
		hash_for_each(table, x.hash, it, struct data, q) {
			depth_max++; hits++;
		}
		REQUIRE(depth_max == 2);
	}
	REQUIRE(hits == 1024);

	hits = 0;
	for (auto& x: data) {
		unsigned depth_max = 0;
		hash_for_each_delsafe(table, x.hash, it, struct data, q) {
			depth_max++; hits++;
		}
		REQUIRE(depth_max == 2);
	}
	REQUIRE(hits == 1024);
}
