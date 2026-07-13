#!/usr/bin/env bats
#
# Integration tests for hpc/sort primitives.
#
# These tests are intentionally thin wrappers around the cmocka binary that
# is produced by tests/sort/test_sort.c.  Driving cmocka through bats keeps
# all test results in a single TAP stream, regardless of whether the actual
# check was written in C (cmocka), shell, or anything else.
#

setup_file() {
    # tools/testing/bats -> package root is three levels up.
    HPC_ROOT="$(cd "${BATS_TEST_DIRNAME}/../../.." && pwd)"
    export HPC_ROOT
    # Prefer the binary path exported by run-check.sh (TEST_SORT_BIN); fall
    # back to the usual kbuild output locations.
    for candidate in \
        "${TEST_SORT_BIN:-}" \
        "${HPC_ROOT}/obj/tools/testing/selftests/units/test_sort" \
        "${HPC_ROOT}/output/tools/testing/selftests/units/test_sort" \
        "${HPC_ROOT}/build/tools/testing/selftests/units/test_sort"
    do
        [ -n "${candidate}" ] || continue
        if [[ -x "${candidate}" ]]; then
            TEST_SORT_BIN="${candidate}"
            break
        fi
    done
    export TEST_SORT_BIN="${TEST_SORT_BIN:-}"
}

@test "sort: test_sort binary is built" {
    [ -n "${TEST_SORT_BIN}" ] || skip "test_sort binary not built (run: make tests)"
    [ -x "${TEST_SORT_BIN}" ]
}

@test "sort: cmocka group passes" {
    [ -n "${TEST_SORT_BIN}" ] || skip "test_sort binary not built"
    run "${TEST_SORT_BIN}"
    [ "${status}" -eq 0 ]
    # cmocka prints the group name on success; surface its TAP-ish line.
    [[ "${output}" == *"PASSED"* || "${output}" == *"ok"* ]]
}

@test "sort: merge_range_asc produces a monotonic sequence" {
    [ -n "${TEST_SORT_BIN}" ] || skip "test_sort binary not built"
    run "${TEST_SORT_BIN}"
    [ "${status}" -eq 0 ]
    # The cmocka test logs each merge step; ensure none failed.
    [[ "${output}" != *"FAILED"* ]]
}
