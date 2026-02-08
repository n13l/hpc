/*
 * Generic introsort for arrays with inlined comparator
 *
 * The MIT License (MIT)         Copyright (c) 2025 Daniel Kubec <niel@rtfm.cz>
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
 *
 * Introsort = quicksort with a depth limit; heapsort kicks in when the
 * recursion depth would exceed 2 * floor(log2(n)); insertion sort handles
 * the small partitions at the leaves.
 *
 * `cmp` may be a static inline function name OR a function-style macro;
 * it must take two `type` values (not pointers) and return <0 / 0 / >0.
 *
 * Recursion is unrolled into an explicit stack of (lo, hi, depth) triples
 * so the function never recurses; pushing the *larger* partition first
 * keeps the stack within log2(n) entries.
 */

#ifndef __CCE_GENERIC_SORT_INTROSORT_H__
#define __CCE_GENERIC_SORT_INTROSORT_H__

#include <hpc/compiler.h>
#include <stddef.h>

#ifndef INTROSORT_INSERTION_THRESHOLD
#define INTROSORT_INSERTION_THRESHOLD 24
#endif

#ifndef INTROSORT_MAX_DEPTH
#define INTROSORT_MAX_DEPTH 48
#endif

#define DEFINE_INTRO_SORT(name, type, cmp) \
\
static inline void name##_swap(type *a, type *b) \
{ \
	type t = *a; *a = *b; *b = t; \
} \
\
static inline void name##_insertion(type *a, size_t lo, size_t hi) \
{ \
	for (size_t i = lo + 1; i <= hi; i++) { \
		type v = a[i]; \
		size_t j = i; \
		while (j > lo && cmp(v, a[j - 1]) < 0) { \
			a[j] = a[j - 1]; \
			j--; \
		} \
		a[j] = v; \
	} \
} \
\
static inline void name##_heap_sift(type *a, size_t n, size_t root) \
{ \
	type v = a[root]; \
	size_t child; \
	while ((child = 2 * root + 1) < n) { \
		if (child + 1 < n && cmp(a[child], a[child + 1]) < 0) \
			child++; \
		if (cmp(v, a[child]) >= 0) break; \
		a[root] = a[child]; \
		root = child; \
	} \
	a[root] = v; \
} \
\
static inline void name##_heap(type *a, size_t lo, size_t hi) \
{ \
	size_t n = hi - lo + 1; \
	type *base = a + lo; \
	for (size_t i = n / 2; i-- > 0; ) \
		name##_heap_sift(base, n, i); \
	while (n > 1) { \
		name##_swap(&base[0], &base[n - 1]); \
		n--; \
		name##_heap_sift(base, n, 0); \
	} \
} \
\
static inline size_t name##_partition(type *a, size_t lo, size_t hi) \
{ \
	size_t mid = lo + (hi - lo) / 2; \
	if (cmp(a[mid], a[lo]) < 0) name##_swap(&a[lo],  &a[mid]); \
	if (cmp(a[hi],  a[lo]) < 0) name##_swap(&a[lo],  &a[hi]);  \
	if (cmp(a[mid], a[hi]) < 0) name##_swap(&a[mid], &a[hi]);  \
	type pivot = a[hi]; \
	size_t i = lo; \
	for (size_t j = lo; j < hi; j++) { \
		if (cmp(a[j], pivot) < 0) { \
			name##_swap(&a[i], &a[j]); \
			i++; \
		} \
	} \
	name##_swap(&a[i], &a[hi]); \
	return i; \
} \
\
static void name(type *arr, size_t count) \
{ \
	if (count < 2) return; \
	int depth_limit = 0; \
	for (size_t x = count; x > 0; x >>= 1) \
		depth_limit++; \
	depth_limit *= 2; \
	struct { size_t lo, hi; int depth; } stack[INTROSORT_MAX_DEPTH]; \
	int sp = 0; \
	stack[sp].lo = 0; stack[sp].hi = count - 1; \
	stack[sp].depth = depth_limit; sp++; \
	while (sp > 0) { \
		sp--; \
		size_t lo = stack[sp].lo; \
		size_t hi = stack[sp].hi; \
		int depth = stack[sp].depth; \
		if (lo >= hi) continue; \
		if (hi - lo + 1 <= INTROSORT_INSERTION_THRESHOLD) { \
			name##_insertion(arr, lo, hi); \
			continue; \
		} \
		if (depth == 0) { \
			name##_heap(arr, lo, hi); \
			continue; \
		} \
		size_t p = name##_partition(arr, lo, hi); \
		size_t left_size  = (p > lo) ? (p - lo) : 0; \
		size_t right_size = (hi > p) ? (hi - p) : 0; \
		if (left_size >= right_size) { \
			if (p > lo) { \
				stack[sp].lo = lo; stack[sp].hi = p - 1; \
				stack[sp].depth = depth - 1; sp++; \
			} \
			if (hi > p) { \
				stack[sp].lo = p + 1; stack[sp].hi = hi; \
				stack[sp].depth = depth - 1; sp++; \
			} \
		} else { \
			if (hi > p) { \
				stack[sp].lo = p + 1; stack[sp].hi = hi; \
				stack[sp].depth = depth - 1; sp++; \
			} \
			if (p > lo) { \
				stack[sp].lo = lo; stack[sp].hi = p - 1; \
				stack[sp].depth = depth - 1; sp++; \
			} \
		} \
	} \
}

#endif/*__CCE_GENERIC_SORT_INTROSORT_H__*/
