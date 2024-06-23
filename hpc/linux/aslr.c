/*
 * Address Space Layout Randomization /proc/sys/kernel/randomize_va_space 
 */

#include <sys/personality.h>

/*
 * Linux supports different execution domains, or personalities, for
 * each process.  Among other things, execution domains tell Linux
 * how to map signal numbers into signal actions.  The execution
 * domain system allows Linux to provide limited support for
 * binaries compiled under other UNIX-like operating systems
 *
 */

void aslr_disable(void)
{
	/* Children inherit their parent's personality. */
	personality(ADDR_NO_RANDOMIZE);
}

/*
 * Linux tells a program things about its runtime environment through a thing 
 * called an "auxillary vector," which lives on the stack just above the 
 * environment variables, and that's how gdb knows where to find main in 
 * position-independent executables.
 *
 * LD_SHOW_AUXV=1 ls
 */
