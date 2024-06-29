include vendor/build/arch.mk
include vendor/build/options.mk
include vendor/build/rules.mk
include vendor/build/catch2.mk
include vendor/build/config.mk

BUILD_CFLAGS=$(CFLAGS) -I. -I$(s)/hpc -std=gnu99 -DCONFIG_LINUX
BUILD_CXXFLAGS=$(CXXFLAGS) -I. -I$(s)/hpc -std=gnu++17 -DCONFIG_LINUX 
BUILD_LDFLAGS+=$(LDFLAGS) 

$(o)/hpc/built-in.o: \
	$(o)/hpc/mem/page.o $(o)/hpc/mem/block.o $(o)/hpc/mem/vm.o \
	$(o)/hpc/mem/alloc.o $(o)/hpc/log/write.o 

$(o)/hpc/libhpc.a: $(o)/hpc/built-in.o
$(o)/hpc/libhpc.nopic.a: $(o)/hpc/built-in.nopic.o

test_ldflags=$(o)/hpc/built-in.o


build_deps: build_catch2 
build_libs: $(o)/hpc/libhpc.a
build_binaries: build_deps

all: build_deps build_libs build_binaries build_tests

test: all units
	$(Q)$(s)/tests/run-tests.sh 
	$(Q)$(o)/tests/src/main

clean:
	$(Q)rm -rf obj

install: install_headers install_libs
install_libs: build_libs

.PHONY: all build_deps build_libs build_binaries test clean build_tests
