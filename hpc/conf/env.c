/*
 * The environment as a source of configuration.
 *
 * The whole of this file is CONFIG_ENVIRONMENT - without it there is nothing
 * here to build, the readers are inline stubs handing back their defaults, and
 * <hpc/conf.h> says so.
 *
 * What it is, is the parsers of parse.c with getenv() in front of them, which
 * is deliberate: `--backlog 4K`, `-Snet.backlog=4K`, `backlog 4K` in a file and
 * BACKLOG=4K in the environment cannot disagree about what 4K means, because
 * one function decides.
 *
 * A value that does not parse is not an error here. The command line is typed
 * at a program by whoever is running it, and answering a bad option with a
 * diagnostic and a non-zero exit is right; an environment is inherited, often
 * from something several execs away that has no idea this program is reading
 * it, and a program that refuses to start over it fails where nobody is
 * looking. So a value that is unset, empty or malformed is the default the
 * caller passed, and a caller that wants to know the difference calls
 * conf_getenv() and parses it itself.
 */

#include <hpc/compiler.h>
#include <hpc/conf.h>

#include <stdlib.h>

const char *
conf_env_str(const char *name, const char *def)
{
	const char *v = conf_getenv(name);

	return v && *v ? v : def;
}

int
conf_env_int(const char *name, int def)
{
	const char *v = conf_getenv(name);
	int val;

	if (!v || !*v || conf_parse_int(v, &val))
		return def;
	return val;
}

unsigned int
conf_env_uint(const char *name, unsigned int def)
{
	const char *v = conf_getenv(name);
	unsigned int val;

	if (!v || !*v || conf_parse_uint(v, &val))
		return def;
	return val;
}

uint64_t
conf_env_u64(const char *name, uint64_t def)
{
	const char *v = conf_getenv(name);
	uint64_t val;

	if (!v || !*v || conf_parse_u64(v, &val))
		return def;
	return val;
}

double
conf_env_double(const char *name, double def)
{
	const char *v = conf_getenv(name);
	double val;

	if (!v || !*v || conf_parse_double(v, &val))
		return def;
	return val;
}

int
conf_env_bool(const char *name, int def)
{
	const char *v = conf_getenv(name);
	int val;

	if (!v || !*v || conf_parse_bool(v, &val))
		return def;
	return val;
}
