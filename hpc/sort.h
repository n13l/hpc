/*
 * Generic sort for containers
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
 */

#ifndef __CCE_GENERIC_SORT_H__
#define __CCE_GENERIC_SORT_H__

#include <hpc/compiler.h>

#define SORT_ORDER_RND 0x01
#define SORT_ORDER_ASC 0x02
#define SORT_ORDER_DSC 0x03

#ifndef SORT_MERGE_BOTTOM_UP_SHIFT
#define SORT_MERGE_BOTTOM_UP_SHIFT 9
#endif

/**
 * insert_sort
 *
 * sort container items in ascending order
 *
 * @self:       the instance
 * @prefix      the prefix of _first(), _next() and _move_before() methods
 * @cmp:        the type safe cmp
 * @type:       the optional structure type of container
 * @member:     the optional name of the node within the struct.
 *
 * container set requires _first(), _next(), _move_before() methods 
 *
 * Time complexity:  Θ(n^2) 
 * Space complexity: 0(1)
 *
 */

#define insert_sort(self,prefix, ...) \
	va_dispatch(insert_sort_asc,__VA_ARGS__)(self,prefix,__VA_ARGS__)

/**
 * insert_sort_asc
 *
 * sort container items in ascending order
 *
 * @self:       the instance
 * @prefix      the prefix of _first(), _next() and _move_before() methods
 * @cmp:        the type safe cmp
 * @type:       the optional structure type of container
 * @member:     the optional name of the node within the struct.
 *
 * container set requires defined _first(), _next(), _move_before() methods
 *
 */

#define insert_sort_asc(self,prefix,...) \
	va_dispatch(insert_sort_asc,__VA_ARGS__)(self,prefix,__VA_ARGS__)
#define insert_sort_asc1(self,prefix,cmp) \
({ \
	for (prefix##_node *z, *y, *x = prefix##_first(self); x; ) { \
		for (z = y = x; (y = prefix##_next(self, y)); )   \
			if (cmp(y, z) < 0) z = y; \
		if (x == z) \
			x = prefix##_next(self, x); \
		else \
			prefix##_move_before(z, x); \
	} \
})
#define insert_sort_asc3(self,prefix,cmp,type,member) \
({ \
	for (prefix##_node *z, *y, *x = prefix##_first(self); x; ) { \
		for (z = y = x; (y = prefix##_next(self, y)); )   \
			if (container_cmp(cmp, y, z, type, member) < 0) z = y;\
		if (x == z) \
			x = prefix##_next(self, x); \
		else \
			prefix##_move_before(z, x); \
	} \
})

/**
 * insert_sort_dsc
 *
 * sort container items in descending order
 *
 * @self:       the instance
 * @prefix      the prefix of _first(), _next() and _move_before() methods
 * @cmp:        the type safe cmp
 * @type:       the optional structure type of container
 * @member:     the optional name of the node within the struct.
 *
 * container set requires defined _first(), _next(), _move_before() methods
 * 
 */

#define insert_sort_dsc(self,prefix, ...) \
	va_dispatch(insert_sort_dsc,__VA_ARGS__)(self,prefix,__VA_ARGS__)
#define insert_sort_dsc1(self,prefix,cmp) \
({ \
	for (prefix##_node *z, *y, *x = prefix##_first(self); x; ) { \
		for (z = y = x; (y = prefix##_next(self, y)); )   \
			if (cmp(y, z) > 0) z = y; \
		if (x == z) \
			x = prefix##_next(self, x); \
		else \
			prefix##_move_before(z, x); \
	} \
})
#define insert_sort_dsc3(self,prefix,cmp,type,member) \
({ \
	for (prefix##_node *z, *y, *x = prefix##_first(self); x; ) { \
		for (z = y = x; (y = prefix##_next(self, y)); )   \
			if (container_cmp(cmp, y, z, type, member) > 0) z = y;\
		if (x == z) \
			x = prefix##_next(self, x); \
		else \
			prefix##_move_before(z, x); \
	} \
})

/**
 * select_sort
 *
 * sort container items in ascending order
 *
 * @self:       the instance
 * @prefix      the prefix of _first(), _next() and _move_before() methods
 * @cmp:        the type safe cmp
 * @type:       the optional structure type of container
 * @member:     the optional name of the node within the struct.
 *
 * container set requires _first(), _next(), _move_before() methods 
 *
 * Time complexity:  Θ(n^2) 
 * Space complexity: 0(1)
 *
 */

#define select_sort(self,prefix, ...) \
	va_dispatch(select_sort_asc,__VA_ARGS__)(self,prefix,__VA_ARGS__)

/**
 * select_sort_asc
 *
 * sort container items in ascending order
 *
 * @self:       the instance
 * @prefix      the prefix of _first(), _next() and _move_before() methods
 * @cmp:        the type safe cmp
 * @type:       the optional structure type of container
 * @member:     the optional name of the node within the struct.
 *
 * container set requires defined _first(), _next(), _move_before() methods
 *
 */

#define select_sort_asc(self,prefix,...) \
	va_dispatch(select_sort_asc,__VA_ARGS__)(self,prefix,__VA_ARGS__)
#define select_sort_asc1(self,prefix,cmp) \
({ \
	for (prefix##_node *z, *y, *x = prefix##_first(self); x; ) { \
		for (z = y = x; (y = prefix##_next(self, y)); )   \
			if (cmp(y, z) < 0) z = y; \
		if (x == z) \
			x = prefix##_next(self, x); \
		else \
			prefix##_move_before(z, x); \
	} \
})
#define select_sort_asc3(self,prefix,cmp,type,member) \
({ \
	for (prefix##_node *z, *y, *x = prefix##_first(self); x; ) { \
		for (z = y = x; (y = prefix##_next(self, y)); )   \
			if (container_cmp(cmp, y, z, type, member) < 0) z = y;\
		if (x == z) \
			x = prefix##_next(self, x); \
		else \
			prefix##_move_before(z, x); \
	} \
})

/**
 * select_sort_dsc
 *
 * sort container items in descending order
 *
 * @self:       the instance
 * @prefix      the prefix of _first(), _next() and _move_before() methods
 * @cmp:        the type safe cmp
 * @type:       the optional structure type of container
 * @member:     the optional name of the node within the struct.
 *
 * container set requires defined _first(), _next(), _move_before() methods
 * 
 */

#define select_sort_dsc(self,prefix, ...) \
	va_dispatch(select_sort_dsc,__VA_ARGS__)(self,prefix,__VA_ARGS__)
#define select_sort_dsc1(self,prefix,cmp) \
({ \
	for (prefix##_node *z, *y, *x = prefix##_first(self); x; ) { \
		for (z = y = x; (y = prefix##_next(self, y)); )   \
			if (cmp(y, z) > 0) z = y; \
		if (x == z) \
			x = prefix##_next(self, x); \
		else \
			prefix##_move_before(z, x); \
	} \
})
#define select_sort_dsc3(self,prefix,cmp,type,member) \
({ \
	for (prefix##_node *z, *y, *x = prefix##_first(self); x; ) { \
		for (z = y = x; (y = prefix##_next(self, y)); )   \
			if (container_cmp(cmp, y, z, type, member) > 0) z = y;\
		if (x == z) \
			x = prefix##_next(self, x); \
		else \
			prefix##_move_before(z, x); \
	} \
})

/**
 * bubble_sort
 *
 * sort container items in ascending order
 *
 * @self:       the instance
 * @prefix      the prefix of _first(), _next() and _move_before() methods
 * @cmp:        the type safe cmp
 * @type:       the optional structure type of container
 * @member:     the optional name of the node within the struct.
 *
 * container set requires _first(), _next(), _move_before() methods 
 *
 * Time complexity:  Θ(n^2) 
 * Space complexity: 0(1)
 *
 */

#define bubble_sort(self,prefix, ...) \
	va_dispatch(bubble_sort_asc,__VA_ARGS__)(self,prefix,__VA_ARGS__)

/**
 * bubble_sort_asc
 *
 * sort container items in ascending order
 *
 * @self:       the instance
 * @prefix      the prefix of _first(), _next() and _move_before() methods
 * @cmp:        the type safe cmp
 * @type:       the optional structure type of container
 * @member:     the optional name of the node within the struct.
 *
 * container set requires defined _first(), _next(), _move_before() methods
 *
 */

#define bubble_sort_asc(self,prefix,...) \
	va_dispatch(bubble_sort_asc,__VA_ARGS__)(self,prefix,__VA_ARGS__)
#define bubble_sort_asc1(self,prefix,cmp) \
({ \
	for (prefix##_node *z, *y, *x = prefix##_first(self); x; ) { \
		for (z = y = x; (y = prefix##_next(self, y)); )   \
			if (cmp(y, z) < 0) z = y; \
		if (x == z) \
			x = prefix##_next(self, x); \
		else \
			prefix##_move_before(z, x); \
	} \
})
#define bubble_sort_asc3(self,prefix,cmp,type,member) \
({ \
	for (prefix##_node *z, *y, *x = prefix##_first(self); x; ) { \
		for (z = y = x; (y = prefix##_next(self, y)); )   \
			if (container_cmp(cmp, y, z, type, member) < 0) z = y;\
		if (x == z) \
			x = prefix##_next(self, x); \
		else \
			prefix##_move_before(z, x); \
	} \
})

/**
 * bubble_sort_dsc
 *
 * sort container items in descending order
 *
 * @self:       the instance
 * @prefix      the prefix of _first(), _next() and _move_before() methods
 * @cmp:        the type safe cmp
 * @type:       the optional structure type of container
 * @member:     the optional name of the node within the struct.
 *
 * container set requires defined _first(), _next(), _move_before() methods
 * 
 */

#define bubble_sort_dsc(self,prefix, ...) \
	va_dispatch(bubble_sort_dsc,__VA_ARGS__)(self,prefix,__VA_ARGS__)
#define bubble_sort_dsc1(self,prefix,cmp) \
({ \
	for (prefix##_node *z, *y, *x = prefix##_first(self); x; ) { \
		for (z = y = x; (y = prefix##_next(self, y)); )   \
			if (cmp(y, z) > 0) z = y; \
		if (x == z) \
			x = prefix##_next(self, x); \
		else \
			prefix##_move_before(z, x); \
	} \
})
#define bubble_sort_dsc3(self,prefix,cmp,type,member) \
({ \
	for (prefix##_node *z, *y, *x = prefix##_first(self); x; ) { \
		for (z = y = x; (y = prefix##_next(self, y)); )   \
			if (container_cmp(cmp, y, z, type, member) > 0) z = y;\
		if (x == z) \
			x = prefix##_next(self, x); \
		else \
			prefix##_move_before(z, x); \
	} \
})

/* merge_sort lives in <hpc/sort/merge.h> and is included here for
 * backwards compatibility. The new implementation operates directly on
 * intrusive ->next/->prev fields and does not depend on container
 * accessor helpers (list_first(), list_next(), list_prev() etc.).
 */
#include <hpc/sort/merge.h>

/**
 * invers_asc - inversion count in ascending order
 *
 * If set is already sorted then inversion count is 0. If set is sorted in 
 * reverse order that inversion count is the maximum. 
 *
 * @self:       the instance
 * @prefix      the prefix of _first(), _next() and _move_before() methods
 * @cmp:        the type safe cmp
 * @type:       the optional structure type of container
 * @member:     the optional name of the node within the struct.
 *
 * container set requires _first(), _next()
 *
 */

#define invers_asc(self,prefix, ...) \
	va_dispatch(invers_asc,__VA_ARGS__)(self,prefix,__VA_ARGS__)
#define invers_asc1(self,prefix,cmp) \
({ \
 	prefix##_node *y, *x; \
 	unsigned inv = 0; \
	for (x = prefix##_first(self); x && x->next; x = prefix##_next(self, x)) \
		for (y = x; (y = prefix##_next(self, y)); ) \
			if (cmp(x, y) > 0) inv++; \
	inv; \
})
#define invers_asc3(self,prefix,cmp,type,member) \
({ \
  	prefix##_node *y, *x; \
 	unsigned inv = 0; \
	for (x = prefix##_first(self);x && x->next;x = prefix##_next(self, x))  \
		for (y = x; (y = prefix##_next(self, y)); )  \
			if (container_cmp(cmp, x, y, type, member) > 0) inv++;\
	inv; \
})

/**
 * invers_dsc - inversion count in descending order
 *
 * If set is already sorted then inversion count is 0. If set is sorted in 
 * reverse order that inversion count is the maximum. 
 *
 * @self:       the instance
 * @prefix      the prefix of _first(), _next() and _move_before() methods
 * @cmp:        the type safe cmp
 * @type:       the optional structure type of container
 * @member:     the optional name of the node within the struct.
 *
 * container set requires _first(), _next()
 *
 */

#define invers_dsc(self,prefix, ...) \
	va_dispatch(invers_dsc,__VA_ARGS__)(self,prefix,__VA_ARGS__)
#define invers_dsc1(self,prefix,cmp) \
({ \
 	prefix##_node *y, *x; \
 	unsigned inv = 0; \
	for (x = prefix##_first(self); x; x = prefix##_next(self, x)); ) \
		for (y = x; (y = prefix##_next(self, y)); ) \
			if (cmp(x, y) > 0) inv++; \
	inv; \
})
#define invers_dsc3(self,prefix,cmp,type,member) \
({ \
  	prefix##_node *y, *x; \
 	unsigned inv = 0; \
	for (x = prefix##_first(self); x; x = prefix##_next(self, x)); ) \
		for (y = x; (y = prefix##_next(self, y)); ) \
			if (container_cmp(cmp, x, y, type, member) > 0) inv++;\
	inv; \
})

#endif/*__CCE_GENERIC_SORT_H__*/
