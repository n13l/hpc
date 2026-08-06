#!/usr/bin/env bats
#
# Surfaces the RCU container stress units' measurement report.
#
# test_units.bats runs a unit binary with its output discarded: a unit passes
# or it fails, and that is all `make check` wants from one. These two also
# *measure* - memory grown and handed back, objects expired and retired, grace
# periods waited for, read-side sections and integrity checks per thread - and
# a number nobody ever sees is a number nobody notices going wrong. So this
# suite owns them instead, and echoes each report to bats' fd 3, which is the
# channel whose text reaches a check log.
#
# The report lines are TAP comments (they start with '#'), so forwarding them
# keeps the stream valid in either bats output mode.
#
# Both binaries only exist in an RCU build (see selftests/units/Kbuild); without
# one these cases skip, exactly as the units themselves are simply absent.
#

setup_file() {
    # tools/testing/bats -> package root is three levels up.
    HPC_ROOT="$(cd "${BATS_TEST_DIRNAME}/../../.." && pwd)"
    export HPC_ROOT
}

# stress_bin <TEST_NAME> - echo the path to a built unit binary, or nothing.
#
# Prefer what run-check.sh exported (<NAME>_BIN, pointing into the real object
# tree, which under the integrated un build is not below HPC_ROOT at all); fall
# back to the usual standalone output locations.
stress_bin() {
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

# metric_rows <REPORT> <HEADER> - count the "#     <label> <number>" rows the
# report prints under <HEADER>, stopping at the next block header.
metric_rows() {
    echo "$1" | awk -v hdr="$2" '
        index($0, hdr) == 1        { in_block = 1; next }
        in_block && /^#   [^ ]/    { in_block = 0 }
        in_block && /^#     [^ ]+ +[0-9]+/ { n++ }
        END                        { print n + 0 }'
}

# run_stress <TEST_NAME> - run one stress unit and forward its report
run_stress() {
    local name="$1"
    local bin
    bin="$(stress_bin "${name}")"
    [ -n "${bin}" ] || skip "${name} not built (needs an RCU build: CONFIG_RCU=y)"

    run "${bin}"

    # The report goes out before the status is judged, so a run that failed says
    # what it was doing when it did - the numbers are the diagnostic.
    echo "${output}" | grep '^#' >&3 || true
    if [ "${status}" -ne 0 ]; then
        echo "${output}" | grep -vE '^#' >&3 || true
    fi

    [ "${status}" -eq 0 ]
    [[ "${output}" != *"FAILED"* ]]

    # A report that lost its numbers is a broken report - so check the report's
    # shape, which every build has: the two lines describing the arena, and the
    # writer and reader blocks with at least one counter row each.
    #
    # Not the counter names: those are CONFIG_MEASURE metadata, and without it
    # every row is labelled with its index instead (stress_label() in
    # units/stress_util.h). Naming them here would make this case a test of the
    # configuration rather than of the report.
    [[ "${output}" == *"#   layout: block "* ]]
    [[ "${output}" == *"#   memory: committed "* ]]
    [ "$(metric_rows "${output}" '#   writer:')" -gt 0 ]
    [ "$(metric_rows "${output}" '#   readers ')" -gt 0 ]

    # Where the names are compiled in, they are worth checking too: these three
    # are the run's verdict (integrity failures), the reason it exists (the slab
    # shrank while readers walked it) and the RCU it does that under.
    if [[ "${output}" != *"#     [0] "* ]]; then
        [[ "${output}" == *"torn"* ]]
        [[ "${output}" == *"shrinks"* ]]
        [[ "${output}" == *"Grace periods"* ]]
    fi
}

@test "stress: hashtable rcu + slab cache under readers" {
    run_stress test_hashtable_rcu_stress
}

@test "stress: rbtree rcu + slab cache under readers" {
    run_stress test_rbtree_rcu_stress
}
