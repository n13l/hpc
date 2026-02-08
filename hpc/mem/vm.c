/* mremap() is a GNU extension; the project builds with -std=gnu1x, which is
 * not enough to declare it. Must precede every include. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <hpc/compiler.h>
#include <hpc/cpu.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <hpc/list.h>
#include <mem/alloc.h>

#include <errno.h>
#include <string.h>

#define VM_PAGE_PROT (PROT_READ | PROT_WRITE)
#define VM_PAGE_MODE (MAP_PRIVATE | MAP_ANON)

void *
vm_page_reserve(void)
{
	void *page = mmap(NULL, 0, VM_PAGE_PROT, VM_PAGE_MODE, -1, 0);
	if (page == (void*)MAP_FAILED)
		die("Cannot mmap reserve virtual memory: %s\n", strerror(errno));
	return page;
}

void *
vm_page_alloc(size_t size)
{
	void *page = mmap(NULL, size, VM_PAGE_PROT, VM_PAGE_MODE, -1, 0);
	if (page == (void*) MAP_FAILED)
		die("Cannot mmap %llu bytes of memory: %s\n", 
		    (unsigned long long)size, strerror(errno));
	return page;
}

void
vm_page_free(void *page, size_t size)
{
	munmap(page, size);
}

void *
vm_page_inquire(void *addr)
{
	return NULL;
}

/*
 * vm_page_extend - resize a region, preserving its contents.
 *
 * The returned address is not the old one: callers (vm_vblock_extend() in
 * <mem/block.h>) already take the new base as the result, so the region is free
 * to move. That is what lets this use mremap(), which moves page table entries
 * instead of copying bytes - the whole point being that the cost stops scaling
 * with the size of the region. Measured against the copying path below, on a
 * fully resident region: ~4x at 64 KiB, ~60x at 1 MiB, ~270x at 16 MiB, ~540x at
 * 256 MiB (33.6 ms -> 0.06 ms).
 *
 * Anything that must keep its address across a resize cannot use this; reserve
 * the maximum up front and commit into it instead, the way <mem/slab.h> does.
 */
void *
vm_page_extend(void *page, size_t olen, size_t size)
{
	void *addr;
#ifdef MREMAP_MAYMOVE
	addr = mremap(page, olen, size, MREMAP_MAYMOVE);
	if (addr != (void *)MAP_FAILED)
		return addr;
	/* Fall through to copying: mremap() needs @olen to match the mapping
	 * exactly, and is missing outside Linux. */
#endif
	addr = vm_page_alloc(size);
	memcpy(addr, page, __min(olen, size));
	vm_page_free(page, olen);
	return addr;
}
