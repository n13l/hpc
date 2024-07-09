#ifndef __HPC_MEM_SLAB_H__
#define __HPC_MEM_SLAB_H__

#include <hpc/compiler.h>
#include <assert.h>
#include <stdint.h>
#include <math.h>
#include <sys/mman.h>
#include <string.h>

#ifndef CPU_PAGE_SIZE
#define CPU_PAGE_SIZE 4096
#endif

#define VM_PAGE_PROT (PROT_READ | PROT_WRITE)
#define VM_PAGE_MODE (MAP_PRIVATE | MAP_ANON)

struct slab_vm {
	u64 length;           /* page-aligned vm region length                */
	u32 list;             /* list of free pages                           */
	u32 avail;            /* number of free pages in list                 */
	u32 total;            /* number of pages in map                       */
	u32 shift;            /* bits object size aligned to power of 2       */
	void *page;
};

struct slab_stat {
	u64 grows;
	u64 shrinks;
	u64 minimum;
	u64 maximum;
};

struct slab {
	u32 avail;            /* linked list of available pages            */
	u32 reserved;
	u8 obj[];
};

void
slab_init(struct slab_vm *vm, unsigned int size, unsigned int total)
{
	vm->shift = log2(size + sizeof(struct slab)) + 1;
	vm->length = align_page((1 << vm->shift) * total);
	vm->total = total;
	vm->page = mmap(NULL, vm->length, VM_PAGE_PROT, VM_PAGE_MODE, -1, 0);

	trace1("slab_init (shift: %u, total: %u, length: %lu): %p",
		vm->shift, total, vm->length, vm->page);
	if (vm->page == (void*)MAP_FAILED)
		die("Cannot mmap virtual memory: %s", strerror(errno));
}

void
slab_fini(struct slab_vm *vm)
{
	trace1("slab_fini (shift: %u, total: %u, length: %lu): %p",
		vm->shift, vm->total, vm->length, vm->page);
	if (vm->page == (void*)MAP_FAILED)
		munmap(vm->page, vm->length);
}

static inline void
slab_estimate(struct slab_vm *vm)
{
}

static inline unsigned int
get_slab_size(struct slab_vm *vm)
{
	return 1 << vm->shift;
}

static inline struct slab *
get_slab_safe(struct slab_vm *vm, unsigned int index)
{
	if (index == (u32)~0U)
		return NULL;
	return (struct slab*)((u8*)vm->page + ((size_t)index << vm->shift));
}

static inline unsigned int
slab_index(struct slab_vm *vm, struct slab *slab)
{
	return (size_t)((u8*)slab - (u8*)vm->page) >> vm->shift;
}

static inline unsigned long
slab_offset(struct slab_vm *vm, struct slab *slab)
{
	return (unsigned long)((u8*)slab - (u8*)vm->page) >> vm->shift;
}

static inline struct slab *
slab_alloc(struct slab_vm *vm)
{
	struct slab *slab;
	if (!(slab = get_slab_safe(vm, vm->list)))
		return NULL;
	vm->list = slab->avail;
	vm->avail--;
	return slab;
}

static inline void
slab_free(struct slab_vm *vm, struct slab *slab)
{
	slab->avail = vm->list;
	vm->list = slab_index(vm, slab);
	vm->avail++;
}

static inline void
slab_grow(struct slab_vm *vm)
{
}

static inline void
slab_shrink(struct slab_vm *vm)
{
}

#endif
