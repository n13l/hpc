#ifndef __LINUX_PROC_H__
#define __LINUX_PROC_H__

#include <sys/types.h>
#include <sys/user.h>
#include <sys/ptrace.h>
#include <assert.h>

#define BP_TRAP_LEN 1
#define INTEL_RET_INSTRUCTION 0xc3
#define INTEL_INT3_INSTRUCTION 0xcc

struct breakpoint {
	pid_t pid;
	intptr_t addr;
	uint8_t saved;
        unsigned enabled:1;
};

void
breakpoint_enable(struct breakpoint *bp);

void
breakpoint_disable(struct breakpoint *bp);

typedef enum {
    ARCH_X86_64,
    ARCH_X86_64_X32,
    ARCH_I386,
    ARCH_ARM64,
    ARCH_ARM_EABI_THUMB,
    ARCH_ARM_EABI,
} libc_arch_t;

typedef union {
    uint8_t u8[8];
    uint16_t u16[4];
    uint32_t u32[2];
} libc_code_t;

struct libc_info {
    pid_t pid;
    uint8_t attached;
    uint8_t mmapped;
    libc_arch_t arch;
    struct user_regs_struct regs;
    unsigned long long start_addr;
    unsigned long long dlopen_addr;
    unsigned long long dlclose_addr;
    unsigned long long dlsym_addr;
    unsigned long long code_addr; 
    libc_code_t backup_code;
    long sys_mmap;
    long sys_mprotect;
    long sys_munmap;
    size_t text; /* read only region */
    size_t text_size;
    size_t stack; /* stack area */
    size_t stack_size;
};

void
proc_status(int id, int status);

int
proc_fullpath(pid_t pid, char *path, size_t size);

int
libc_lookup_path(pid_t pid, char *path, unsigned int max_size);

long
libc_lookup_base(pid_t pid, const char *file);

long
libc_lookup_fn(const char *path, const char *fn);

long
proc_lookup_base(pid_t pid);

int
proc_is_mem_mapped(pid_t pid, void *addr);

long
proc_lookup_mem_cave(pid_t pid);

unsigned long
proc_lookup_main_offset(const char *path);

int
get_libc_info(struct libc_info *info, pid_t pid);

int
process_dso_load(pid_t target, char *path, char *libname);

void aslr_disable(void);

#endif
