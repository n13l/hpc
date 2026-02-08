#!/bin/sh
#
# bootstrap.sh — initialize submodules for a standalone hpc build.
#
# hpc is a Kbuild package: it owns its runtime (hpc/) and its selftests, and
# takes the build infrastructure (scripts/, arch/, os/ — all committed symlinks
# into vendor/kbuild) plus its vendor libraries from submodules. This script
# initializes exactly what a standalone `make defconfig && make check` needs:
#
#   vendor/kbuild           the build system itself; scripts/, arch/ and os/
#                           resolve through it, so nothing builds without it
#   vendor/userspace-rcu    liburcu, behind the RCU variants of the intrusive
#                           containers (CONFIG_RCU; see hpc/rcu.h). Opt-in in the
#                           configuration but initialized here, so turning RCU on
#                           in menuconfig does not need a second bootstrap
#   vendor/catch2           C++ unit-test framework, for the Catch2 suites
#
# The bats test framework lives in the KBUILD submodule
# (vendor/kbuild/vendor/bats-{core,assert,file,support}) — bats is test/build
# infra, so kbuild owns it. hpc reaches it through committed symlinks
# (vendor/bats-* -> vendor/kbuild/vendor/bats-*), so initializing kbuild's copies
# below is what puts `bats` on PATH for `make check`.
#
# Note for the integrated build: when hpc is consumed as un's vendor/hpc, un
# provides kbuild from the top and initializes hpc's liburcu itself, so this
# script is NOT run there — un's own bootstrap.sh covers it. It is for a
# standalone hpc checkout.
#
set -e
cd "$(dirname "$0")"

echo "hpc: initializing build infrastructure (kbuild) ..."
git submodule update --init vendor/kbuild

echo "hpc: initializing vendor libraries (liburcu, catch2) ..."
git submodule update --init vendor/userspace-rcu vendor/catch2

echo "hpc: initializing bats test framework (kbuild) ..."
git -C vendor/kbuild submodule update --init \
    vendor/bats-core vendor/bats-assert vendor/bats-file vendor/bats-support

echo "hpc: ready. Configure and build with:"
echo "    make menuconfig      # or: make defconfig"
echo "    make -j\$(nproc)"
echo "    make check"
