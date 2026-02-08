# Kernel-style selftests entry point (see the crypto/un equivalents).
PKGROOT := $(realpath $(CURDIR)/../../..)
O       ?= $(PKGROOT)/obj

.PHONY: run_tests
run_tests:
	$(CONFIG_SHELL) $(PKGROOT)/scripts/run-check.sh "$(O)" "$(PKGROOT)"
