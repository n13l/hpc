#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/reg.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/mman.h>


#include <wait.h>
#include <time.h>

#include "ptrace.h"

#include <hpc/compiler.h>
#include <hpc/log.h>

#include <hpc/linux/proc.h>

#define CHECK_ERR(cond) \
	if (cond) die("%s (%d:%s)", #cond, errno, strerror(errno));

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

unsigned long long
ptrace_getip(pid_t pid)
{
	long v = ptrace(PTRACE_PEEKUSER, pid, sizeof(long)*REGISTER_IP);
	return (unsigned long long) (v - TRAP_LEN);
}

void
ptrace_attach(pid_t pid)
{
	int status;
	CHECK_ERR(ptrace(PTRACE_ATTACH, pid, NULL, NULL) == -1);
	trace1("attach(pid=%u) Ok", pid);
	CHECK_ERR(waitpid(pid, &status, WUNTRACED) != pid);
	trace1("waitpid(pid=%u) Ok", pid);
}

void
ptrace_detach(pid_t pid)
{
	CHECK_ERR(ptrace(PTRACE_DETACH, pid, NULL, NULL) == -1);
	trace1("detach(pid=%u) Ok", pid);

}

void
ptrace_getregs(pid_t pid, struct REG_TYPE* regs)
{
	CHECK_ERR(ptrace(PTRACE_GETREGS, pid, NULL, regs) == -1);
	trace1("getregs(pid=%u) Ok", pid);
}

void
ptrace_cont(pid_t pid)
{
	struct timespec* sleeptime = malloc(sizeof(struct timespec));
	sleeptime->tv_sec = 0;
	sleeptime->tv_nsec = 5000000;

	CHECK_ERR(ptrace(PTRACE_CONT, pid, NULL, NULL) == -1);
	nanosleep(sleeptime, NULL);
}

void
ptrace_setregs(pid_t pid, struct REG_TYPE* regs)
{
	CHECK_ERR(ptrace(PTRACE_SETREGS, pid, NULL, regs) == -1);
	trace1("setregs(pid=%u) Ok", pid);
}

siginfo_t ptrace_getsiginfo(pid_t pid)
{
	siginfo_t pidsig;
	CHECK_ERR(ptrace(PTRACE_GETSIGINFO, pid, NULL, &pidsig) == -1);
	return pidsig;
}

void
ptrace_read(int pid, unsigned long addr, void *vptr, int len)
{
	int bytes = 0;
	int i = 0;
	long word = 0;
	long *ptr = (long *) vptr;

	while (bytes < len) {
		word = ptrace(PTRACE_PEEKTEXT, pid, addr + bytes, NULL);
		CHECK_ERR(word == -1);
		bytes += sizeof(word);
		ptr[i++] = word;
	}
}

void
ptrace_write(int pid, unsigned long addr, void *vptr, int len)
{
	int bytes = 0;
	long word = 0;

	while (bytes < len) {
		memcpy(&word, vptr + bytes, sizeof(word));
		word = ptrace(PTRACE_POKETEXT, pid, addr + bytes, word);
		CHECK_ERR(word == -1);
		bytes += sizeof(word);
	}
}

void
ptrace_dump(pid_t pid, unsigned start, unsigned stop)
{
	trace1("Dump of %d's memory [0x%08X : 0x%08X]", pid, start, stop);
	for (unsigned addr = start; addr <= stop; ++addr) {
		unsigned word = ptrace(PTRACE_PEEKTEXT, pid, addr, 0);
		trace1("  0x%08X:  %02x", addr, word & 0xFF);
	}
}

void *
ptrace_page_alloc(pid_t pid)
{
	int status = 0;
	pid_t rv;
	struct user_regs_struct regs;
	CHECK_ERR(ptrace(PTRACE_GETREGS, pid, NULL, &regs));
	struct user_regs_struct original_regs = regs;

	regs.rax = __NR_mmap;
	regs.rdi = 0;
	regs.rsi = 4096;
	regs.rdx = PROT_READ | PROT_EXEC;
	regs.r10 = MAP_PRIVATE | MAP_ANONYMOUS;
	regs.r8  = -1;
	regs.r9  = 0;

	CHECK_ERR(ptrace(PTRACE_SETREGS, pid, NULL, &regs) == -1);
	CHECK_ERR(ptrace(PTRACE_SINGLESTEP, pid, NULL, NULL) == -1);
	
	rv = waitpid(pid, &status, 0);
	proc_status(pid, status);

	CHECK_ERR(ptrace(PTRACE_GETREGS, pid, NULL, &regs) == -1);

	long addr = (long)regs.rax;
	debug1("page_alloc(pid: %d): %s", pid, addr < 0 ? "failed": "succeeded");
	CHECK_ERR(ptrace(PTRACE_SETREGS, pid, NULL, &original_regs) == -1);
	return (void *)addr;
}

int
ptrace_page_protect(pid_t pid, void *addr, int prot)
{
	struct user_regs_struct regs;
	CHECK_ERR(ptrace(PTRACE_GETREGS, pid, NULL, &regs));

	struct user_regs_struct original_regs = regs;
	regs.rax = __NR_mprotect;
	regs.rdi = (long) addr;
	regs.rsi = 4096;
	regs.rdx = prot;

	CHECK_ERR(ptrace(PTRACE_SETREGS, pid, NULL, &regs));
	CHECK_ERR(ptrace(PTRACE_SINGLESTEP, pid, NULL, NULL) == -1);

	int status = 0;
	pid_t rv = waitpid(pid, &status, 0);
	proc_status(pid, status);

	CHECK_ERR(ptrace(PTRACE_GETREGS, pid, NULL, &regs));

	int result = (int)regs.rax;
	debug1("page_protect(pid: %d): %s", pid,
               result ? "failed": "succeeded");
	CHECK_ERR(ptrace(PTRACE_SETREGS, pid, NULL, &original_regs));
	return result;
}

int
ptrace_poke(pid_t pid, void *addr, void *ntext, void *otext, size_t len)
{
	if (len % sizeof(void *) != 0)
		return -1;

	long poke;
	for (size_t copied = 0; copied < len; copied += sizeof(poke)) {
		memmove(&poke, ntext + copied, sizeof(poke));
		if (otext != NULL) {
			errno = 0;
			long peek = ptrace(PTRACE_PEEKTEXT, pid, addr + copied, NULL);
			if (peek == -1 && errno)
				return -1;
			memmove(otext + copied, &peek, sizeof(peek));
		}
		if (ptrace(PTRACE_POKETEXT, pid, addr + copied, (void *)poke) < 0)
			return -1;
	}
	return 0;
}

int
ptrace_peek_str(pid_t child, unsigned char *addr, char *str, unsigned int size)
{
	int read = 0;
	unsigned long tmp;

	memset(str, 0, size);

	while (1) {
		if (read + sizeof(tmp) > size)
			return -1;
		tmp = ptrace(PTRACE_PEEKDATA, child, addr + read);
		memcpy(str + read, &tmp, sizeof(tmp));
		if (memchr(&tmp, 0, sizeof(tmp)) != NULL) {
			return 0;
		}
		read += sizeof(tmp);
	}
	return 1;
}


