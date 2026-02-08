/*
 * Slab reservation backend                              Build-time selectable
 *
 * The MIT License (MIT)
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
 * Where a slab's reservation comes from, chosen at build time.
 *
 * Shared by the dynamic <mem/slab.h> and the build-time <mem/slab_class.h> so
 * one setting covers both, and so neither has to include the other.
 *
 * A slab asks the host for three things: reserve @len bytes once
 * (SLAB_VM_ALLOC), give it back (SLAB_VM_FREE), and hand the physical pages of
 * a range back while keeping the addresses mapped (SLAB_VM_RELEASE, used by
 * shrink). There is deliberately no expand hook: the reservation is sized once
 * at init and never resized, which is exactly what keeps block addresses stable
 * for the lifetime of the slab. SLAB_VM_FAILED is what SLAB_VM_ALLOC returns on
 * failure.
 *
 * Backends
 * --------
 * default            mmap(MAP_ANON | MAP_NORESERVE), munmap(),
 *                    madvise(MADV_DONTNEED) to release pages
 * -DSLAB_MALLOC_FREE the libc heap, mirroring MEM_MALLOC_FREE in <mem/page.h>
 * custom             define SLAB_VM_ALLOC/FREE/RELEASE/FAILED yourself - a
 *                    static arena, a freestanding target with no mmap(), a
 *                    shared-memory or hugetlbfs segment
 *
 * Know what a backend with no working SLAB_VM_RELEASE costs before choosing
 * one: shrink still drops blocks from the committed prefix and rebuilds the
 * free list, it just returns no memory to the host, so RSS stays at its
 * high-water mark for the life of the slab.
 *
 * One other difference between backends is worth knowing: a fresh anonymous
 * mapping reads as zeroes, the libc heap and a recycled arena do not. The slab
 * itself never relies on that - its bitmap and cache entries come zeroed from
 * SLAB_MEM_CALLOC, and a block is payload the caller initialises - but code that
 * grew up on the mmap backend and quietly assumed a zeroed block will not
 * survive the switch.
 *
 * Note that realloc() has no place among these hooks. The whole design rests on
 * the reservation never moving; realloc() may move it, and a move invalidates
 * every block pointer the caller is holding. A slab that can be reallocated is a
 * different data structure with a different contract (index-only access, no raw
 * pointers held across a grow).
 *
 * Hints
 * -----
 * SLAB_VM_HUGEPAGE (over the whole reservation at init) and SLAB_VM_POPULATE
 * (over a range about to be committed) are advisory and may do nothing. They
 * are wired to CONFIG_MEM_HUGEPAGE / CONFIG_MEM_POPULATE for the default mmap
 * backend and are no-ops for any other, since only a custom backend knows what
 * its memory supports; it may define them itself.
 *
 * Metadata
 * --------
 * The occupancy bitmap and the cache entry array in <mem/slab_cache.h> are
 * metadata that no caller holds a pointer into, so they get their own pair of
 * hooks - SLAB_MEM_CALLOC / SLAB_MEM_FREE - and are free to move.
 */

#ifndef __HPC_MEM_SLAB_VM_H__
#define __HPC_MEM_SLAB_VM_H__

#include <hpc/compiler.h>
#include <hpc/cpu.h>

#include <stdlib.h>
#include <sys/mman.h>

__BEGIN_DECLS

#ifndef CPU_PAGE_SIZE
#define CPU_PAGE_SIZE 4096
#endif

#ifndef VM_PAGE_PROT
#define VM_PAGE_PROT (PROT_READ | PROT_WRITE)
#endif
#ifndef VM_PAGE_MODE
#define VM_PAGE_MODE (MAP_PRIVATE | MAP_ANON | MAP_NORESERVE)
#endif

/* index sentinel for "no block" / end of free list */
#ifndef SLAB_NIL
#define SLAB_NIL ((u32)~0U)
#endif

#ifndef SLAB_VM_ALLOC
# ifdef SLAB_MALLOC_FREE
#  define SLAB_VM_ALLOC(len)        malloc((size_t)(len))
#  define SLAB_VM_FREE(ptr, len)    ((void)(len), free(ptr))
#  define SLAB_VM_RELEASE(ptr, len) ((void)(ptr), (void)(len))
#  define SLAB_VM_FAILED            NULL
# else
#  define SLAB_VM_ALLOC(len) \
	mmap(NULL, (size_t)(len), VM_PAGE_PROT, VM_PAGE_MODE, -1, 0)
#  define SLAB_VM_FREE(ptr, len)    munmap((ptr), (size_t)(len))
#  define SLAB_VM_RELEASE(ptr, len) madvise((ptr), (size_t)(len), MADV_DONTNEED)
#  define SLAB_VM_FAILED            ((void *)MAP_FAILED)
#  define SLAB_VM_MMAP              1  /* the madvise() hints below apply */
# endif
#endif
#ifndef SLAB_VM_FAILED
#define SLAB_VM_FAILED NULL
#endif

#ifndef SLAB_MEM_CALLOC
#define SLAB_MEM_CALLOC(n, size) calloc((n), (size))
#define SLAB_MEM_FREE(ptr)       free(ptr)
#endif

/*
 * The numeric fallbacks below let a build against older libc headers still
 * compile; an old kernel simply fails the madvise() and the slab carries on,
 * which is why neither return value is checked.
 */
#if defined(CONFIG_MEM_HUGEPAGE) && defined(SLAB_VM_MMAP) && \
    !defined(SLAB_VM_HUGEPAGE)
# ifndef MADV_HUGEPAGE
#  define MADV_HUGEPAGE 14
# endif
# define SLAB_VM_HUGEPAGE(ptr, len) \
	((void)madvise((ptr), (size_t)(len), MADV_HUGEPAGE))
#endif
#ifndef SLAB_VM_HUGEPAGE
#define SLAB_VM_HUGEPAGE(ptr, len) ((void)(ptr), (void)(len))
#endif

#if defined(CONFIG_MEM_POPULATE) && defined(SLAB_VM_MMAP) && \
    !defined(SLAB_VM_POPULATE)
# ifndef MADV_POPULATE_WRITE
#  define MADV_POPULATE_WRITE 23
# endif
# define SLAB_VM_POPULATE(ptr, len) \
	((void)madvise((ptr), (size_t)(len), MADV_POPULATE_WRITE))
#endif
#ifndef SLAB_VM_POPULATE
#define SLAB_VM_POPULATE(ptr, len) ((void)(ptr), (void)(len))
#endif

/*
 * Release granularity - the "grain".
 *
 * Memory comes back to the host in whole units, never in blocks. The unit is a
 * page for an ordinary mapping, and a huge page once the reservation is backed by
 * one, because that is the granularity madvise(MADV_DONTNEED) can act on: asking
 * to drop less than a grain drops nothing at all.
 *
 * So the grain, not the block, is what a slab grows and shrinks by. min, max and
 * grow_step are rounded up to it at init, and the committed prefix therefore
 * always ends on a grain boundary - which is what makes the block counts the slab
 * reports mean the same thing as the memory it is actually holding. Without that
 * rounding a slab with a 2048 byte block could shrink by one block, report the
 * shrink, and release nothing.
 *
 * Rounding is visible, and deliberately so: ask for a 64 block minimum with a
 * 2 MB grain and you get 1024, because a 2 MB huge page is what the kernel is
 * going to fault in whether the policy admits it or not. Read the rounded values
 * back with slab_policy_min()/slab_policy_max(), and the granularity itself with
 * slab_grain()/slab_grain_bytes().
 *
 * Override SLAB_GRAIN_BYTES for a backend whose unit is neither: hugetlbfs at
 * 1 MB or 1 GB, or a custom allocator with a coarse arena. It must be a power of
 * two and is not required to be a multiple of the block size.
 */
#ifndef SLAB_HUGE_BYTES
#define SLAB_HUGE_BYTES (1u << 21)      /* x86/arm64 THP pmd size, 2 MB */
#endif

#ifndef SLAB_GRAIN_BYTES
# if defined(CONFIG_MEM_HUGEPAGE) && defined(SLAB_VM_MMAP)
#  define SLAB_GRAIN_BYTES SLAB_HUGE_BYTES
# else
#  define SLAB_GRAIN_BYTES CPU_PAGE_SIZE
# endif
#endif

_Static_assert((SLAB_GRAIN_BYTES & (SLAB_GRAIN_BYTES - 1)) == 0,
	"SLAB_GRAIN_BYTES must be a power of two");

/* Blocks per grain for a block of 1 << @shift bytes; never zero. A block at
 * least as large as a grain already covers whole grains, so the unit is one
 * block. */
static inline u32
slab_grain_for(unsigned shift)
{
	size_t grain = (size_t)SLAB_GRAIN_BYTES;
	return ((size_t)1 << shift) >= grain ? 1u : (u32)(grain >> shift);
}

/* Round @n up to a whole number of grains, saturating instead of wrapping. */
static inline u32
slab_grain_round(u32 n, u32 grain)
{
	u32 rem;
	if (grain <= 1)
		return n;
	rem = n % grain;
	if (!rem)
		return n;
	if (n > (u32)~0U - (grain - rem))       /* would wrap: clamp down */
		return n - rem;
	return n + (grain - rem);
}

__END_DECLS

#endif/*__HPC_MEM_SLAB_VM_H__*/
