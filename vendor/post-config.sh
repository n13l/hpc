#!/bin/sh
#
# hpc post-configuration hook (invoked by the kbuild Makefile's vendor-prepare
# step and after each *config target, with: <srctree> <objtree> <config file>).
#
# hpc's one vendor dependency is liburcu (vendor/userspace-rcu), which backs the
# RCU variants of the intrusive containers. Building it is delegated to
# build-urcu.sh - the same script un calls for hpc's tree in the integrated
# build - and is a no-op unless CONFIG_THREADS && CONFIG_RCU. A plain
# single-threaded build therefore configures nothing at all.

set -e

srctree="$1"
objtree="$2"
config="$3"

if [ -z "$srctree" ] || [ -z "$objtree" ] || [ -z "$config" ]; then
	echo "Usage: $0 <srctree> <objtree> <config-file>" >&2
	exit 1
fi

if [ -x "$srctree/vendor/build-urcu.sh" ]; then
	"$srctree/vendor/build-urcu.sh" "$srctree" "$objtree" "$config"
fi
