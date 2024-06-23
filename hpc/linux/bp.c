#include <hpc/compiler.h>
#include <hpc/log.h>
#include <sys/ptrace.h>
#include <sys/reg.h>
#include <sys/wait.h>

#include <hpc/linux/proc.h>

#include <stdio.h>

#if defined(__i386)
#define REGISTER_IP EIP
#define TRAP_LEN    1
#define TRAP_INST   0xCC
#define TRAP_MASK   0xFFFFFF00

#elif defined(__x86_64)
#define REGISTER_IP RIP
#define TRAP_LEN    1
#define TRAP_INST   0xCC
#define TRAP_MASK   0xFFFFFFFFFFFFFF00

#else
#error Unsupported architecture
#endif

void
breakpoint_enable(struct breakpoint *bp)
{
	if (bp->enabled)
		return;

	long data = ptrace(PTRACE_PEEKDATA, bp->pid, bp->addr, NULL);
	bp->saved = (u8)(data & 0xff);
	u64 int3 = 0xcc;
	u64 data_with_int3 = ((data & ~0xff) | int3);
	long result = ptrace(PTRACE_POKEDATA, bp->pid, bp->addr, data_with_int3);
	bp->enabled = 1;
	debug1("trap at 0x%.16lx", (long)bp->addr);
}

void
breakpoint_disable(struct breakpoint *bp)
{
	long data = ptrace(PTRACE_PEEKDATA, bp->pid, bp->addr, NULL);
	u64 restored = ((data & ~0xff) | bp->saved);
	ptrace(PTRACE_POKEDATA, bp->pid, bp->addr, restored);
	bp->enabled = 0;
}
