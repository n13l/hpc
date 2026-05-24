#!/usr/bin/env bats
#
# Smoke tests for the hpc build tree.  These are deliberately cheap so they
# can act as a first-line "did the tree build at all?" gate in CI.
#

setup_file() {
    HPC_ROOT="$(cd "${BATS_TEST_DIRNAME}/.." && pwd)"
    export HPC_ROOT
}

@test "build: top-level Kbuild exists" {
    [ -f "${HPC_ROOT}/Kbuild" ]
}

@test "build: hpc/ headers are present" {
    [ -d "${HPC_ROOT}/hpc" ]
    [ -f "${HPC_ROOT}/hpc/compiler.h" ]
}

@test "build: bats vendor submodule is initialised" {
    [ -x "${HPC_ROOT}/vendor/bats-core/bin/bats" ] \
        || skip "vendor/bats-core not initialised (run: git submodule update --init)"
}

@test "build: defconfig is readable" {
    [ -f "${HPC_ROOT}/defconfig" ]
    run cat "${HPC_ROOT}/defconfig"
    [ "${status}" -eq 0 ]
}
