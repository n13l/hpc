/*
 * The MIT License (MIT)                          Red-Black Tree Measurements
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
 * Red-black tree counters - the introspectable set of events the tree tracks.
 * Built on the value-counter facility in <hpc/measure.h>: the single list below
 * generates struct rbtree_measure (one u64 per stored field), the parallel
 * name/description table and rbtree_measure_count.
 *
 * The tree keeps a pointer to a caller-owned struct rbtree_measure (declared by
 * measure_member(rbtree) in <hpc/rbtree.h>); events are counted directly
 * through the generic measure_inc()/measure_add() from <hpc/measure.h>, which
 * no-op on a NULL measure and vanish entirely without CONFIG_MEASURE. Attach a
 * struct with rbtree_measure_attach() to start counting - point several trees at
 * one struct to aggregate, each hold their own, or leave it NULL to not measure.
 *
 * Walk every metric with measure_for_each(rbtree, i) / measure_name() /
 * measure_at(), read one stored field as a struct member (m->insert) or via
 * measure_of(rbtree, m, "insert"), and read the ratio - always recomputed from
 * its two fields, so it stays correct on an aggregated struct - with
 * measure_at(rbtree, m, i).
 */

#ifndef __HPC_RBTREE_MEASURE_H__
#define __HPC_RBTREE_MEASURE_H__

#include <hpc/measure.h>

/*
 * Counters (monotonic event totals):
 * - insert:  nodes linked into the tree (one per rbtree_insert_color)
 * - erase:   nodes removed from the tree (rbtree_erase / rbtree_erase_init)
 * - rotate:  single rotations performed while rebalancing, from both insertion
 *            colour-fixing and deletion colour-fixing - the tree's headline
 *            cost metric (a red-black tree does O(1) amortised rotations per
 *            update, at most 2 per insert and 3 per erase)
 *
 * Gauge (current level, moves up and down):
 * - entries: nodes currently in the tree (insert - erase; replace is a swap and
 *            leaves it unchanged)
 *
 * Ratio (percentage, aggregation-safe):
 * - rebalance: rotations as a percent of insertions - the rebalancing
 *              intensity. Summing the rotate and insert counters of several
 *              trees and then reading rebalance gives the true combined rate,
 *              never the mean of per-tree percentages.
 */
#define RBTREE_METRICS(_ns, C, G, R) \
	C(_ns, insert,    "Nodes linked into the tree") \
	C(_ns, erase,     "Nodes removed from the tree") \
	C(_ns, rotate,    "Rotations performed while rebalancing") \
	G(_ns, entries,   "Nodes currently in the tree") \
	R(_ns, rebalance, rotate, insert, "Rotations per 100 insertions")

DEFINE_MEASURE(rbtree, RBTREE_METRICS);

#endif/*__HPC_RBTREE_MEASURE_H__*/
