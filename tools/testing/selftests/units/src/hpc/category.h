#ifndef __TEST_BSD_CATEGORY_H__
#define __TEST_BSD_CATEGORY_H__

#include <hpc/compiler.h>
#include <hpc/array.h>

__BEGIN_DECLS

#define USTAT_EXAMPLE_0                  0
#define USTAT_EXAMPLE_1                  1
#define USTAT_EXAMPLE_2                  2
#define USTAT_EXAMPLE_3                  3
#define USTAT_EXAMPLE_4                  4
#define USTAT_EXAMPLE_5                  5
#define USTAT_EXAMPLE_6                  6
#define USTAT_EXAMPLE_7                  7
#define USTAT_EXAMPLE_8                  8
#define USTAT_EXAMPLE_9                  9
#define USTAT_EXAMPLE_10                 10
#define USTAT_EXAMPLE_11                 11
#define USTAT_EXAMPLE_12                 12
#define USTAT_EXAMPLE_13                 13
#define USTAT_EXAMPLE_14                 14
#define USTAT_EXAMPLE_15                 15
#define USTAT_EXAMPLE_16                 16
#define USTAT_EXAMPLE_17                 17

DECLARE_CONST_ARRAY(const char *, test_names1);
DECLARE_CONST_ARRAY(const char *, test_names3);
DECLARE_CONST_ARRAY(const char *, test_names7);
DECLARE_CONST_ARRAY(const char *, test_names6);
DECLARE_CONST_ARRAY(const char *, test_names15);
DECLARE_CONST_1_BASED_ARRAY(const char *, test_1b_names6);

__END_DECLS

#endif
