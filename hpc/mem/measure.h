/*
 * The MIT License (MIT)                                  Slab Measurements
 *
 * Copyright (c) 2012-2026                          Daniel Kubec <niel@rtfm.cz>
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/*
 * Slab allocator counters - the introspectable set of events the slab tracks.
 * Built on the value-counter facility in <hpc/measure.h>: the single list
 * below generates struct slab_measure (one u64 per counter), the parallel
 * name/description table and slab_measure_count.
 *
 * The slab keeps a pointer to a caller-owned struct slab_measure (see
 * <mem/slab.h>), so many slabs can share one struct (global or local), each
 * hold their own, or leave it NULL to not measure. Walk every counter with
 * measure_for_each(slab, i) / measure_name()/ measure_value(), or read one by
 * name as a field (m->alloc) or via measure_of(slab, m, "alloc").
 */

#ifndef __HPC_MEM_MEASURE_H__
#define __HPC_MEM_MEASURE_H__

#include <hpc/measure.h>

/*
 * Counters (monotonic event totals):
 * - alloc:   blocks handed out by slab_alloc()
 * - free:    blocks returned by slab_free()
 * - fail:    allocations that returned NULL (reservation exhausted)
 * - grow:    grow steps taken (working set committed)
 * - shrink:  shrink steps taken (tail memory returned to the OS)
 * - gc:      slab_gc() passes that changed the committed set
 * - commit:  blocks committed, summed across all grow steps
 * - reclaim: blocks reclaimed, summed across all shrink steps
 *
 * Gauges (current levels, move up and down):
 * - used:      live (allocated) blocks right now
 * - committed: blocks currently committed
 *
 * Ratio (percentage, aggregation-safe):
 * - usage:     used as a percent of committed - the "slab usage". Summing the
 *              used and committed gauges of several per-thread slabs and then
 *              reading usage gives the true global utilisation.
 */
#define SLAB_METRICS(_ns, C, G, R) \
	C(_ns, alloc,     "Blocks handed out by slab_alloc") \
	C(_ns, free,      "Blocks returned by slab_free") \
	C(_ns, fail,      "Allocations that returned NULL") \
	C(_ns, grow,      "Grow steps - working set committed") \
	C(_ns, shrink,    "Shrink steps - tail memory released") \
	C(_ns, gc,        "GC passes that changed the committed set") \
	C(_ns, commit,    "Blocks committed across all grows") \
	C(_ns, reclaim,   "Blocks reclaimed across all shrinks") \
	G(_ns, used,      "Live (allocated) blocks") \
	G(_ns, committed, "Blocks currently committed") \
	R(_ns, usage, used, committed, "Live blocks as percent of committed")

DEFINE_MEASURE(slab, SLAB_METRICS);

#endif/*__HPC_MEM_MEASURE_H__*/
