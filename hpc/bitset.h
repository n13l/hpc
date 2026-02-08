/*
 * bitset.h — compact bit set implementation for enum-indexed flags (C99)
 */

#ifndef HPC_BITSET_H
#define HPC_BITSET_H

#include <stdint.h>
#include <assert.h>

#define BITSET_SIZE(max_id) (((max_id) + 8) >> 3)
#define BITSET_DECLARE(name, max_id) uint8_t name[BITSET_SIZE(max_id)]
#define BITSET_CLEAR(bs) memset((bs), 0, sizeof(bs))
#define BITSET_SET(bs, id) ((bs)[(id) >> 3] |= (1u << ((id) & 7)))
#define BITSET_CLR(bs, id) ((bs)[(id) >> 3] &= ~(1u << ((id) & 7)))
#define BITSET_TOGGLE(bs, id) ((bs)[(id) >> 3] ^= (1u << ((id) & 7)))
#define BITSET_TEST(bs, id) ((bs)[(id) >> 3] & (1u << ((id) & 7)))
#define BITSET_BUG_ON(max_id, id) \
    ((void)sizeof(uint8_t[1 - 2 * !((id) >= 0 && (id) <= (max_id))]))

#endif /* BITSET_H */
