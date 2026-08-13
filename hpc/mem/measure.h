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

/*
 * Memory pool counters (<mem/mempool.h>), the same facility for the other
 * allocator. Counts are in grains where the slab's are in blocks.
 *
 * Counters:
 * - alloc:    runs handed out by mempool_alloc()
 * - free:     runs returned by mempool_free()
 * - grow:     runs made longer by mempool_grow(), in place or by moving
 * - move:     ...of which relocated - copied to a fresh run
 * - shrink:   runs cut shorter by mempool_shrink()
 * - fail:     requests that returned NULL (no run of that length)
 * - release:  gc passes that handed memory back to the host
 * - reclaim:  grains released, summed across all passes
 *
 * Gauges:
 * - used:     live grains
 * - resident: grains the pool holds from the host - live plus idle
 *
 * Ratio:
 * - usage:    used as a percent of resident
 */
#define MEMPOOL_METRICS(_ns, C, G, R) \
	C(_ns, alloc,    "Runs handed out by mempool_alloc") \
	C(_ns, free,     "Runs returned by mempool_free") \
	C(_ns, grow,     "Runs grown, in place or by moving") \
	C(_ns, move,     "Runs relocated by a grow") \
	C(_ns, shrink,   "Runs cut shorter by mempool_shrink") \
	C(_ns, fail,     "Requests that returned NULL") \
	C(_ns, release,  "GC passes that released memory") \
	C(_ns, reclaim,  "Grains released across all passes") \
	G(_ns, used,     "Live grains") \
	G(_ns, resident, "Grains held from the host: live plus idle") \
	R(_ns, usage, used, resident, "Live grains as percent of resident")

DEFINE_MEASURE(mempool, MEMPOOL_METRICS);

#endif/*__HPC_MEM_MEASURE_H__*/
