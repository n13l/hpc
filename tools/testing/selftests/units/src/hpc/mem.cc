#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_predicate.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <hpc/compiler.h>
#include <hpc/log.h>
#include <hpc/mem/slab.h>

class Session {
public:
	Session() {}
	~Session() {}
	unsigned char m_buffer[8192];
};

TEST_CASE("slab shrink operation works as expected", "hpc/mem") {
}


