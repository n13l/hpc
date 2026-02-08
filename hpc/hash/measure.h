/*
 * The MIT License (MIT)                             Hash Table Measurements
 *
 * Copyright (c) 2013 - 2019                        Daniel Kubec <niel@rtfm.cz>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"),to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 **/

/*
 * Hash table counters - the introspectable set of events the hash table
 * tracks. Built on the value-counter facility in <hpc/measure.h>: the single
 * list below generates struct hash_measure (one u64 per counter), the parallel
 * name/description table and hash_measure_count.
 *
 * The table keeps a pointer to a caller-owned struct hash_measure (see
 * <hpc/hash/table.h>), so tables can share one struct (global or local) or
 * leave it NULL to not measure. Walk every counter with
 * measure_for_each(hash, i) / measure_name() / measure_value(), or read one by
 * name as a field (m->add) or via measure_of(hash, m, "add").
 */

#ifndef __HPC_HASH_MEASURE_H__
#define __HPC_HASH_MEASURE_H__

#include <hpc/measure.h>

/*
 * Counters (monotonic event totals):
 * - add:       elements inserted (hash_add)
 * - del:       elements removed (hash_del / hash_del_init)
 * - collision: insertions into an already non-empty bucket
 * - ruc:       recently-used-cache promotions (hash_ruc)
 *
 * Gauge (current level):
 * - entries:   elements currently in the table (add - del; ruc is a move and
 *              leaves it unchanged)
 */
#define HASH_METRICS(_ns, C, G, R) \
	C(_ns, add,       "Elements inserted into the table") \
	C(_ns, del,       "Elements removed from the table") \
	C(_ns, collision, "Insertions into a non-empty bucket") \
	C(_ns, ruc,       "Recently-used-cache promotions") \
	G(_ns, entries,   "Elements currently in the table")

DEFINE_MEASURE(hash, HASH_METRICS);

#endif/*__HPC_HASH_MEASURE_H__*/
