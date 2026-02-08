#ifndef __HPC_MODULE_H__
#define __HPC_MODULE_H__

#include <hpc/compiler.h>

__BEGIN_DECLS

struct module {
	const char *name;
	int (*init)(void);
	void (*exit)(void);
};

#ifdef CONFIG_MODULES

#define module_init(fn) \
	static void __attribute__((constructor)) __mod_init__(void) { fn(); }

#define module_exit(fn) \
	static void __attribute__((destructor)) __mod_exit__(void) { fn(); }

static inline void modules_init(void) {}
static inline void modules_exit(void) {}

#else

extern struct module __start___modules[];
extern struct module __stop___modules[];

#define DEFINE_MODULE(modname, initfn, exitfn) \
	static struct module _used \
	__attribute__((section("__modules"))) \
	__mod_desc__ = { \
		.name = modname, \
		.init = initfn, \
		.exit = exitfn, \
	}

#define module_init(fn) \
	static int _used __mod_initfn__(void) { return fn(); }

#define module_exit(fn) \
	static void _used __mod_exitfn__(void) { fn(); }

static inline void
modules_init(void)
{
	struct module *m;
	for (m = __start___modules; m < __stop___modules; m++)
		if (m->init)
			m->init();
}

static inline void
modules_exit(void)
{
	struct module *m = __stop___modules;
	while (m-- > __start___modules)
		if (m->exit)
			m->exit();
}

#endif

__END_DECLS

#endif
