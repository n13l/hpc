/*
 * Generic merge sort for intrusive containers
 *
 * The MIT License (MIT)         Copyright (c) 2018 Daniel Kubec <niel@rtfm.cz>
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
 * Bottom-up merge sort that operates directly on the intrusive ->next /
 * ->prev fields of any node type. It does NOT depend on container accessor.
 *
 * The range sort follows the same lazy bottom-up scheme used by the Linux
 * kernel's list_sort(): elements are pushed onto a pending stack indexed by
 * power-of-two.
 *
 * Three entry points, all parameterised by (cmp, type, member):
 *
 *   merge_range_asc(a, b, cmp, type, member)
 *       merge two sorted NULL-terminated ranges (via ->next).
 *
 *   merge_sort_range_asc(head, cmp, type, member)
 *       sort a NULL-terminated range in place; @head is updated.
 *
 *   merge_sort(self, cmp, type, member)
 *   merge_sort_asc(self, cmp, type, member)
 *       sort a circular intrusive container whose sentinel is reachable as
 *       `&(self)->head` and whose nodes have `prev` and `next` fields.
 */

#ifndef __CCE_GENERIC_SORT_MERGE_H__
#define __CCE_GENERIC_SORT_MERGE_H__

#include <hpc/compiler.h>

#ifndef MERGE_SORT_MAX_LEVELS
#define MERGE_SORT_MAX_LEVELS 48
#endif

/* detach a circular container into a NULL-terminated range via ->next */
#define __merge_sort_detach(self) \
({ \
	__typeof__((self)->head.next) __first = (self)->head.next; \
	(self)->head.prev->next = NULL; \
	__first; \
})

/* Re-attach a NULL-terminated range (sorted) to the circular sentinel. */
#define __merge_sort_attach(self, first) \
({ \
	__typeof__((self)->head.next) __prev = \
		(__typeof__((self)->head.next))&(self)->head; \
	__typeof__((self)->head.next) __n = (first); \
	for (; __n; __n = __n->next) { \
		if (__n->next) __builtin_prefetch(__n->next, 1, 0); \
		__n->prev = __prev; \
		__prev = __n; \
	} \
	(self)->head.next = (first); \
	(self)->head.prev = __prev; \
	__prev->next = (__typeof__(__prev))&(self)->head; \
})

/**
 * merge_range_asc - merge two sorted NULL-terminated ranges
 *
 * @a:          first sorted range  (NULL-terminated via ->next)
 * @b:          second sorted range (NULL-terminated via ->next)
 * @cmp:        the type safe comparator
 * @type:       the structure type
 * @member:     the name of the node within the struct
 */

#define merge_range_asc(a, b, cmp, type, member) \
({ \
	__typeof__(a) __a = (a), __b = (b); \
	__typeof__(a) __h, __t; \
	if (!__a)      { __h = __b; } \
	else if (!__b) { __h = __a; } \
	else { \
		if (container_cmp(cmp, __b, __a, type, member) < 0) \
			{ __h = __t = __b; __b = __b->next; } \
		else \
			{ __h = __t = __a; __a = __a->next; } \
		while (__a && __b) { \
			/* Prefetch the next node on each side so its cache */ \
			/* line is on the way while we run this compare. We */ \
			/* keep it to one step ahead because each prefetch  */ \
			/* IS a DRAM transaction; doubling them up trades   */ \
			/* latency-hiding for bandwidth pressure, and at    */ \
			/* working sets > L3 we are already bandwidth-bound */ \
			/* on the pointer chase, not latency-bound.         */ \
			__typeof__(a) __an = __a->next, __bn = __b->next; \
			if (__an) __builtin_prefetch(__an, 0, 1); \
			if (__bn) __builtin_prefetch(__bn, 0, 1); \
			if (container_cmp(cmp, __b, __a, type, member) < 0) \
				{ __t->next = __b; __t = __b; __b = __bn; } \
			else \
				{ __t->next = __a; __t = __a; __a = __an; } \
		} \
		__t->next = __a ? __a : __b; \
	} \
	__h; \
})

/**
 * merge_sort_range_asc - sort a NULL-terminated range via ->next
 *
 * @head:       lvalue holding the range head; updated in place
 * @cmp:        the type safe comparator
 * @type:       the structure type
 * @member:     the name of the node within the struct
 */

#define merge_sort_range_asc(head, cmp, type, member) \
({ \
	__typeof__(head) __pending[MERGE_SORT_MAX_LEVELS]; \
	for (int __k = 0; __k < MERGE_SORT_MAX_LEVELS; __k++) \
		__pending[__k] = NULL; \
	__typeof__(head) __list = (head); \
	while (__list && __list->next) { \
		__typeof__(head) __a = __list; \
		__typeof__(head) __b = __list->next; \
		__list = __b->next; \
		__typeof__(head) __cur; \
		if (container_cmp(cmp, __b, __a, type, member) < 0) \
			{ __b->next = __a; __a->next = NULL; __cur = __b; } \
		else \
			{ __a->next = __b; __b->next = NULL; __cur = __a; } \
		int __i = 0; \
		while (__pending[__i]) { \
			__cur = merge_range_asc(__pending[__i], __cur, \
			                       cmp, type, member); \
			__pending[__i] = NULL; \
			__i++; \
		} \
		__pending[__i] = __cur; \
	} \
	/* If the range length was odd, push the final singleton as well. */ \
	if (__list) { \
		__list->next = NULL; \
		__typeof__(head) __cur = __list; \
		int __i = 0; \
		while (__pending[__i]) { \
			__cur = merge_range_asc(__pending[__i], __cur, \
			                       cmp, type, member); \
			__pending[__i] = NULL; \
			__i++; \
		} \
		__pending[__i] = __cur; \
	} \
	__typeof__(head) __res = NULL; \
	for (int __k = 0; __k < MERGE_SORT_MAX_LEVELS; __k++) { \
		if (!__pending[__k]) continue; \
		__res = __res \
		      ? merge_range_asc(__pending[__k], __res, \
		                        cmp, type, member) \
		      : __pending[__k]; \
	} \
	(head) = __res; \
})

/**
 * merge_sort - sort a circular intrusive container in ascending order
 *
 * @self:       the container; must expose `head` as the sentinel and nodes
 *              must have `prev` and `next` fields (e.g. struct list)
 * @cmp:        the type safe comparator
 * @type:       the structure type
 * @member:     the name of the node within the struct
 */
#define merge_sort(self, cmp, type, member) \
	merge_sort_asc(self, cmp, type, member)

#define merge_sort_asc(self, cmp, type, member) \
({ \
	if ((self)->head.next != (__typeof__((self)->head.next))&(self)->head && \
	    (self)->head.next->next != (__typeof__((self)->head.next))&(self)->head) { \
		__typeof__((self)->head.next) __range = __merge_sort_detach(self); \
		merge_sort_range_asc(__range, cmp, type, member); \
		__merge_sort_attach(self, __range); \
	} \
})

#endif/*__CCE_GENERIC_SORT_MERGE_H__*/
