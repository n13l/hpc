ifdef CONFIG_MODULES
objs-m += $(patsubst %/,%,$(dir $(wildcard modules/*/Kbuild)))
else
ifneq ($(wildcard modules/*/Kbuild),)
$(shell $(CONFIG_SHELL) $(srctree)/scripts/gen-builtin-modules.sh $(srctree))
objs-y += modules
endif
endif
