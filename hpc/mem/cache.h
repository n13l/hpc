/*
 * High performance, generic and type-safe memory management
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2012-2018                            OpenAAA <openaaa@rtfm.cz>
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
 
#ifndef __MM_CACHE_GENERIC_H__
#define __MM_CACHE_GENERIC_H__

#include <hpc/compiler.h>
#include <hpc/cpu.h>
#include <hpc/log.h>
#include <mem/alloc.h>
#include <mem/savep.h>
#include <mem/debug.h>
#include <mem/generic.h>
#include <assert.h>

struct page;

struct mm_cache {
	int unused;
};

struct page *
mm_cache_alloc(struct mm_cache *cache, size_t size, int flags);

void
mm_cache_free(struct mm_cache *cache, struct page *page);

void
mm_cache_expires(struct page *page);

#endif
