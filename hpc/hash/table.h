/*
 * Generic hash functions 
 *
 * The MIT License (MIT)
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

#ifndef __GENERIC_HASH_TABLE_H__
#define __GENERIC_HASH_TABLE_H__

#include <hpc/compiler.h>
#include <hpc/cpu.h>
#include <hpc/queue.h>
#include <hpc/hash/measure.h>
#include <math.h>
#include <limits.h>

__BEGIN_DECLS

/*
 * A hash table is an array of struct queue buckets: each bucket is a single
 * head pointer, so a table costs one pointer per slot, and any element is
 * removed in O(1) through the qnode **prev link (see <hpc/queue.h>).
 */

/*
 * Event measurement. The table is a bare bucket array with no handle to hang
 * per-table state on, so the current measurement target is a caller-owned
 * struct hash_measure (defined in <hpc/hash/measure.h>) addressed through the
 * @hash_measure pointer. Assign it to a global or local struct to start
 * counting - point several units at one struct to aggregate - and leave it
 * NULL to not measure. A NULL target is one predicted-not-taken branch per
 * event. The pointer is per translation unit.
 */
measure_ptr(hash, hash_measure)

#define DECLARE_HASHTABLE(name, bits) \
	struct queue name[1 << (bits)]
#define DEFINE_HASHTABLE(name, bits) DECLARE_HASHTABLE(name, bits) = { \
		[0 ... ((1<<(bits))-1)] = init_queue \
	}

#define hash_init_table(name, bits) \
({ \
	for (unsigned i = 0; i < (1 << bits); i++) \
		name[i] = init_queue; \
})

#define hash_bits(name) (unsigned)(log2(array_size(name)))
#define hash_add(table, node, hash) ({ \
	struct queue *__q = &(table[hash]); \
	measure_inc_if(hash_measure, !queue_empty(__q), collision); \
	queue_add(__q, node); \
	measure_inc(hash_measure, add); \
	measure_inc(hash_measure, entries);         /* gauge up */ \
})
#define hash_seq(seqno, bits) ({(seqno & ((1 << bits) - 1));})

/* (RUC) recently-used-cache - a move, so the entry count is unchanged */
#define hash_ruc(table, node, hash) ({ \
	queue_del(node); \
	queue_add(&(table[hash]), node); \
	measure_inc(hash_measure, ruc); \
})

#define hash_del(node) ({ \
	queue_del(node); \
	measure_inc(hash_measure, del); \
	measure_dec(hash_measure, entries);         /* gauge down */ \
})
#define hash_del_init(node) ({ \
	queue_del_init(node); \
	measure_inc(hash_measure, del); \
	measure_dec(hash_measure, entries);         /* gauge down */ \
})

#define hash_empty(table, hash) queue_empty(&(table[hash]))
#define hash_init(node) (node) = init_qnode

#define hash_hashed(qnode) ({ !qnode_unhashed(qnode); })

#define hash_for_each(table, hash, it, type, member) \
	queue_for_each(&(table[hash]), it, type, member)

#define hash_for_each_delsafe(table, hash, it, type, member) \
	queue_for_each_delsafe(&(table[hash]), it, type, member)

/*
 * RCU variant: lockless readers concurrent with a serialised writer. Gated on
 * CONFIG_RCU (which depends on CONFIG_THREADS); publishing uses store-release,
 * traversal dependency-ordered loads, both from liburcu (see <hpc/rcu.h>). The
 * bucket walk runs inside a read-side section the caller opens, and freeing a
 * removed node must wait for a grace period. See <hpc/queue.h> for the
 * underlying primitives.
 */
#ifdef CONFIG_RCU

#define hash_add_rcu(table, node, hash) \
	queue_add_head_rcu(&(table[hash]), node)

#define hash_del_rcu(node) queue_del_rcu(node)

#define hash_for_each_rcu(table, hash, it, type, member) \
	queue_for_each_rcu(&(table[hash]), it, type, member)

#endif/*CONFIG_RCU*/

__END_DECLS

#endif
