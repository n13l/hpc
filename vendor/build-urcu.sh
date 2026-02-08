#!/bin/sh
#
# Configure and build the vendored liburcu (userspace-rcu) for hpc's RCU
# containers.
#
# Usage: build-urcu.sh <srctree> <objtree> <auto.conf>
#
# Called from a package's vendor/post-config.sh during the kbuild vendor-prepare
# step, so the library is in place before the first object is compiled. hpc calls
# it for its own srctree; un calls it for hpc's, reaching the source through the
# committed vendor/userspace-rcu symlink (the same shape as un's
# vendor/{openssl,aws-lc} symlinks into the crypto submodule). Nothing outside
# this script needs to know how liburcu is built.
#
# Gated on CONFIG_THREADS && CONFIG_RCU: with either off, hpc's containers are
# compiled without their RCU variants, nothing includes a urcu header, and this
# is a no-op. Otherwise:
#
#   vendor/userspace-rcu             the pinned source (untouched, except for
#                                    the one-time autoreconf below)
#   $objtree/vendor/userspace-rcu    out-of-tree build; the arch-specific
#                                    generated headers land here, not in the
#                                    submodule
#   $objtree/vendor/userspace-rcu/install
#                                    staged prefix: one self-contained include/
#                                    tree plus the static archives, which is
#                                    what the Kbuild files point at
#
# Only src/ is built and staged - liburcu's benchmarks and torture tests are
# plain (noinst) programs that a bare `make` would otherwise build too.
#
# The archives are static: the RCU flavour is a compile-time contract between the
# reader inlines in <hpc/rcu.h> and the grace-period implementation they are
# paired with, so it is pinned per build rather than resolved at load time.

set -e

srctree="$1"
objtree="$2"
autoconf="$3"

if [ -z "$srctree" ] || [ -z "$objtree" ] || [ -z "$autoconf" ]; then
	echo "Usage: $0 <srctree> <objtree> <auto.conf>" >&2
	exit 1
fi

srctree=$(cd "$srctree" && pwd)
objtree=$(cd "$objtree" && pwd)
case "$autoconf" in
	/*) ;;
	 *) autoconf="$objtree/$autoconf" ;;
esac

. "$autoconf" 2>/dev/null || true

# Not a threaded RCU build: nothing to do.
[ "$CONFIG_THREADS" = "y" ] || exit 0
[ "$CONFIG_RCU" = "y" ]     || exit 0

URCU_SRC="$srctree/vendor/userspace-rcu"
URCU_OUT="$objtree/vendor/userspace-rcu"
URCU_PREFIX="$URCU_OUT/install"

if [ ! -f "$URCU_SRC/configure.ac" ]; then
	echo >&2 "  ERROR: CONFIG_RCU is enabled but the liburcu source is missing"
	echo >&2 "         at vendor/userspace-rcu. The RCU variants of the hpc"
	echo >&2 "         containers bind to it (see <hpc/rcu.h>)."
	echo >&2 "         git submodule update --init vendor/userspace-rcu"
	echo >&2 "         (in the hpc tree), then rebuild."
	exit 1
fi

# The flavour chosen in the configuration (CONFIG_RCU_* in hpc/Kconfig) decides
# which archive is the linkable one; <hpc/rcu.h> includes the matching header. A
# configuration predating the choice gets the default flavour.
flavour=memb
[ "$CONFIG_RCU_QSBR" = "y" ] && flavour=qsbr
[ "$CONFIG_RCU_BP" = "y" ]   && flavour=bp

case "$flavour" in
	memb) urcu_lib=liburcu.a      ;;
	qsbr) urcu_lib=liburcu-qsbr.a ;;
	bp)   urcu_lib=liburcu-bp.a   ;;
esac

# liburcu's own make is an automake sub-build: run it with kbuild's make
# environment cleared, as the rustls/aws-lc builds do, so it does not inherit
# the jobserver fds, -rR or the KBUILD_* include dirs.
urcu_make() {
	env -u MAKEFLAGS -u MAKELEVEL -u MFLAGS -u MAKEFILES make "$@"
}

# --- one-time autoreconf --------------------------------------------------- #
# liburcu ships no configure script (it is generated); its bootstrap runs
# autoreconf in the source directory, which is the only write this script makes
# there. The generated files are covered by liburcu's own .gitignore, so the
# submodule stays clean.
if [ ! -x "$URCU_SRC/configure" ]; then
	if ! command -v autoreconf >/dev/null 2>&1; then
		echo >&2 "  ERROR: autoreconf not found; it is needed once to generate"
		echo >&2 "         vendor/userspace-rcu/configure (liburcu ships none)."
		echo >&2 "         Install autoconf, automake and libtool, then rebuild."
		exit 1
	fi
	echo "  BOOTSTRAP vendor/userspace-rcu"
	if [ "$KBUILD_VERBOSE" = "1" ]; then
		(cd "$URCU_SRC" && env -u MAKEFLAGS -u MAKELEVEL -u MFLAGS ./bootstrap)
	else
		(cd "$URCU_SRC" && env -u MAKEFLAGS -u MAKELEVEL -u MFLAGS ./bootstrap) \
			>/dev/null 2>&1
	fi
fi

# --- configure ------------------------------------------------------------- #
URCU_ARGS="--disable-shared --enable-static --disable-dependency-tracking"

# A static archive linked into a PIC binary needs PIC objects.
[ "$CONFIG_CC_PIC" = "y" ] && URCU_ARGS="$URCU_ARGS --with-pic"

# Match the tree's optimisation and debug intent.
URCU_CFLAGS=""
if [ "$CONFIG_CC_OPTIMIZE" = "y" ]; then
	if [ "$CONFIG_CC_OPTIMIZE_FOR_SIZE" = "y" ]; then
		URCU_CFLAGS="$URCU_CFLAGS -Os"
	else
		URCU_CFLAGS="$URCU_CFLAGS -O2"
	fi
fi
[ "$CONFIG_DEBUG_INFO" = "y" ] && URCU_CFLAGS="$URCU_CFLAGS -g"

# Cross build: hand liburcu the same toolchain kbuild uses, so its configure
# tests run against the target compiler rather than the build one.
if [ -n "$CROSS_COMPILE" ]; then
	URCU_ARGS="$URCU_ARGS --host=${CROSS_COMPILE%-}"
fi

mkdir -p "$URCU_OUT"

# Reconfigure when the kbuild configuration changed (flavour, optimisation,
# toolchain); otherwise reuse the build directory as it stands.
configured="$URCU_OUT/.configured"
if [ ! -f "$URCU_OUT/Makefile" ] || [ ! -f "$configured" ] \
	|| [ "$autoconf" -nt "$configured" ]; then
	echo "  CONFIG  vendor/userspace-rcu ($flavour)"
	configure_urcu() {
		(cd "$URCU_OUT" && env -u MAKEFLAGS -u MAKELEVEL -u MFLAGS \
			"$URCU_SRC/configure" --prefix="$URCU_PREFIX" $URCU_ARGS \
			${CC:+CC="$CC"} ${URCU_CFLAGS:+CFLAGS="$URCU_CFLAGS"})
	}
	if [ "$KBUILD_VERBOSE" = "1" ]; then
		configure_urcu
	else
		# On failure, liburcu's configure leaves its reasons in config.log.
		configure_urcu >/dev/null 2>&1 || {
			echo >&2 "  ERROR: liburcu configure failed; see $URCU_OUT/config.log"
			exit 1
		}
	fi
	touch "$configured"
fi

# --- build and stage ------------------------------------------------------- #
# src/ carries the archives and the flavour headers (urcu.h, urcu-qsbr.h, ...);
# include/ carries the urcu/ tree, generated arch headers included. Installing
# both leaves one include root and one lib root for the Kbuild files to name.
#
# The staged archive is part of the up-to-date check, not just the stamp: a
# sweep over $objtree that removes object files by extension (kbuild's `make
# clean` used to reach in here) takes the .o/.a but leaves libtool's .lo/.la,
# and those are newer than their objects - so `make` would report nothing to do
# and `make install` would fail on the archive that is no longer there. When the
# staged archive is gone but the build directory still claims to hold one,
# discard the stale bookkeeping and build from scratch.
built="$URCU_OUT/.built"
if [ ! -f "$built" ] || [ "$configured" -nt "$built" ] \
	|| [ ! -f "$URCU_PREFIX/lib/$urcu_lib" ]; then
	echo "  BUILD   vendor/userspace-rcu ($urcu_lib)"
	build_urcu() {
		if [ -f "$URCU_OUT/src/liburcu-common.la" ] \
			&& [ ! -f "$URCU_OUT/src/.libs/liburcu-common.a" ]; then
			urcu_make -C "$URCU_OUT/src" clean
		fi
		urcu_make -C "$URCU_OUT/src" -j"$(nproc)"
		urcu_make -C "$URCU_OUT/src" install
		urcu_make -C "$URCU_OUT/include" install
	}
	if [ "$KBUILD_VERBOSE" = "1" ]; then
		build_urcu
	else
		build_urcu >/dev/null 2>&1
	fi
	if [ ! -f "$URCU_PREFIX/lib/$urcu_lib" ]; then
		echo >&2 "  ERROR: liburcu built but $urcu_lib is missing from"
		echo >&2 "         $URCU_PREFIX/lib"
		exit 1
	fi
	touch "$built"
fi

exit 0
