subdir-y += hpc
subdir-y += tools/testing/selftests

# Header roots. hpc's own sources reach each other by their public spelling
# (<hpc/compiler.h>, <hpc/mem/pool.h>), which resolves against the srctree root;
# the second root is for the short spelling used inside the runtime (<mem/...>,
# <log.h>). un's top-level Kbuild sets up the same pair, which is why the
# integrated build works with only the second one named here - a standalone hpc
# build needs both.
subdir-ccflags-y += -I$(srctree)
subdir-ccflags-y += -I$(srctree)/hpc

# The RCU variants of the intrusive containers bind to the vendored liburcu
# through <hpc/rcu.h>; the flags below are empty unless CONFIG_RCU is set.
include $(srctree)/vendor/Kbuild.urcu
subdir-ccflags-y += $(URCU_CFLAGS)

# selftests link against hpc/built-in.o
tools/testing/selftests: | hpc
