#!/usr/bin/env bats
#
# One bats case per cmocka unit binary (selftests/units/Kbuild).
#
# Driving the units through bats keeps every result in the single TAP stream
# the shared runner (kbuild scripts/run-check.sh) aggregates: the runner
# discovers the built binaries and exports each as <NAME>_BIN; this suite is
# where they are run, reported and counted. A unit that is not built skips,
# with the config knob that would build it.
#
# test_sort and the two _rcu_stress units are not repeated here - they have
# suites of their own (test_sort.bats, test_stress.bats) that also assert on
# their output, not just their exit status.
#

setup_file() {
    # tools/testing/bats -> package root is three levels up.
    HPC_ROOT="$(cd "${BATS_TEST_DIRNAME}/../../.." && pwd)"
    export HPC_ROOT
}

# unit_bin <TEST_NAME> - echo the path to a built unit binary, or nothing.
#
# Prefer what run-check.sh exported (<NAME>_BIN, pointing into the real object
# tree, which under the integrated un build is not below HPC_ROOT at all); fall
# back to the usual standalone output locations.
unit_bin() {
    local name="$1"
    local var="$(echo "${name}" | tr '[:lower:]' '[:upper:]')_BIN"
    local candidate
    for candidate in \
        "${!var:-}" \
        "${HPC_ROOT}/obj/tools/testing/selftests/units/${name}" \
        "${HPC_ROOT}/output/tools/testing/selftests/units/${name}" \
        "${HPC_ROOT}/build/tools/testing/selftests/units/${name}"
    do
        [ -n "${candidate}" ] || continue
        if [[ -x "${candidate}" ]]; then
            echo "${candidate}"
            return 0
        fi
    done
    return 0
}

# run_unit <TEST_NAME> [skip-reason] - run one cmocka group binary.
#
# On failure the cmocka output is forwarded to fd 3, so the failing group and
# assertion reach the check log; a passing unit stays quiet.
run_unit() {
    local name="$1" why="${2:-not built (run: make tests)}"
    local bin
    bin="$(unit_bin "${name}")"
    [ -n "${bin}" ] || skip "${name} ${why}"

    run "${bin}"
    if [ "${status}" -ne 0 ]; then
        echo "${output}" >&3
    fi
    [ "${status}" -eq 0 ]
    [[ "${output}" != *"FAILED"* ]]
}

@test "units: conf cmocka group" {
    run_unit test_conf
}

@test "units: hashtable cmocka group" {
    run_unit test_hashtable
}

@test "units: hashtable_cache cmocka group" {
    run_unit test_hashtable_cache
}

@test "units: measure cmocka group" {
    run_unit test_measure
}

@test "units: queue cmocka group" {
    run_unit test_queue
}

@test "units: rbtree cmocka group" {
    run_unit test_rbtree
}

@test "units: slab cmocka group" {
    run_unit test_slab
}

@test "units: slab_cache cmocka group" {
    run_unit test_slab_cache
}

# The lockless container variants only exist in an RCU build; see the
# rcutest-$(CONFIG_RCU) gate in selftests/units/Kbuild.

@test "units: hashtable_rcu cmocka group" {
    run_unit test_hashtable_rcu "not built (needs an RCU build: CONFIG_RCU=y)"
}

@test "units: queue_rcu cmocka group" {
    run_unit test_queue_rcu "not built (needs an RCU build: CONFIG_RCU=y)"
}

@test "units: rbtree_rcu cmocka group" {
    run_unit test_rbtree_rcu "not built (needs an RCU build: CONFIG_RCU=y)"
}

@test "units: slab_rcu cmocka group" {
    run_unit test_slab_rcu "not built (needs an RCU build: CONFIG_RCU=y)"
}
