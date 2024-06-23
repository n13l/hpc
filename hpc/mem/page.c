#include <hpc/compiler.h>
#include <hpc/log.h>
#include <hpc/cpu.h>
#include <hpc/list.h>
#include <mem/page.h>

#ifndef MEM_MALLOC_FREE
static inline u64
pages_total_bytes(unsigned int bits, unsigned int page_bits, unsigned int total)
{
	return ((1 << bits) + align_to(((1 << page_bits) * total), 1 << bits));
}
#endif

int
pages_alloc(struct pages *pages, int prot, int mode,
            int vm_bits, int page_bits, int total)
{
#ifdef MEM_MALLOC_FREE
	pages->shift = page_bits;
	pages->page = 0;
	return 0;
#else
	pages->size  = pages_total_bytes(vm_bits, page_bits, total);
	pages->list  = 0;
	pages->total = pages->avail = total;
	pages->shift = page_bits;

	pages->page = mmap(NULL, pages->size, prot, mode, -1, 0);
	if (pages->page == MAP_FAILED)
		goto failed;
	pages_reset(pages);
	return 0;
failed:
	if (pages->page != MAP_FAILED)
		munmap(pages->page, pages->size);
	return -1;
#endif
}

void
pages_reset(struct pages *pages)
{
#ifndef MEM_MALLOC_FREE
	page_for_each(pages, struct page *, page) {
		u32 index = page_index(pages, page);
		page->avail = index + 1 >= pages->total ? (u32)~0U : index + 1;
	};
#endif
}

int
pages_free(struct pages *pages)
{
#ifndef MEM_MALLOC_FREE
	return munmap(pages->page, pages->size);
#else
	return 0;
#endif
}
