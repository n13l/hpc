#include <hpc/compiler.h>
#include <hpc/log.h>
#include <hpc/array.h>

#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/reg.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <elf.h>

/* /usr/include/asm/unistd_x32.h /usr/include/asm/unistd_64.h  */
STATIC_CONST_ARRAY_STREAMLINED(const char *, syscall_names, "n/a",
	[__NR_read]       = "read",
	[__NR_write]      = "write",
	[__NR_socket]     = "socket",
	[__NR_connect]    = "connect",
	[__NR_fork]       = "fork",
	[__NR_clone]      = "clone",
	[__NR_execve]     = "execve",
);

STATIC_ARRAY(unsigned long long, syscall_offsets, 0, 8);
