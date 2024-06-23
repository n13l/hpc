#ifndef __PTRACE_HELPERS_H__
#define __PTRACE_HELPERS_H__

#ifdef ARM
	#define REG_TYPE user_regs
#else
	#define REG_TYPE user_regs_struct
#endif

void ptrace_attach(pid_t target);
void ptrace_detach(pid_t target);
void ptrace_getregs(pid_t target, struct REG_TYPE* regs);
void ptrace_cont(pid_t target);
void ptrace_setregs(pid_t target, struct REG_TYPE* regs);
void ptrace_read(int pid, unsigned long addr, void *vptr, int len);
void ptrace_write(int pid, unsigned long addr, void *vptr, int len);
void ptrace_dump(pid_t pid, unsigned from_addr, unsigned to_addr);
unsigned long long ptrace_getip(pid_t pid);
int ptrace_poke(pid_t pid, void *addr, void *ntext, void *otext, size_t len);
int ptrace_peek_str(pid_t child, unsigned char *addr, char *str, unsigned int size);

#endif
