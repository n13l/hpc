subdir-y += hpc
subdir-y += tools/testing/selftests
subdir-ccflags-y += -I$(srctree)/hpc

# selftests link against hpc/built-in.o
tools/testing/selftests: | hpc
