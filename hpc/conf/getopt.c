/*
 * The getopt_long() side: assembling the tables, decoding what getopt_long()
 * hands back, and the ready-made loop for programs that want one.
 *
 * The generated tables are deliberately ordinary. An attribute with a short
 * letter is returned as that letter, so a program can keep its own
 *
 *   while ((c = getopt_long(argc, argv, conf_shortopts(ctx),
 *                           conf_longopts(ctx), NULL)) != -1) {
 *           if (conf_dispatch(ctx, c, optarg) != CONF_UNKNOWN)
 *                   continue;
 *           switch (c) { ... its own CONF_USER options ... }
 *   }
 *
 * and read exactly as it did with a hand-written table - the difference being
 * that the table, the help text and the section namespace can no longer drift
 * apart, because there is only one declaration behind all three.
 */

#include <hpc/compiler.h>
#include <hpc/conf.h>
#include <conf/internal.h>
#include <mem/alloc.h>

#include <stdio.h>
#include <string.h>

void
conf_build(struct conf_ctx *ctx)
{
	unsigned int i, nlong = 0, nshort = 0;

	if (ctx->longopts)
		return;

	for (i = 0; i < ctx->nitems; i++) {
		if (ctx->items[i].attr->name)
			nlong++;
		if (ctx->items[i].negname)
			nlong++;
		if (ctx->items[i].attr->letter)
			nshort++;
	}

	struct option *lo = (struct option *)
		conf_alloc_mem(ctx, (nlong + 1) * sizeof(*lo));
	/* ':' plus at most "x::" per short option, plus the terminator. */
	char *so = (char *)conf_alloc_mem(ctx, 2 + nshort * 3 + 1);
	char *p = so;

	memset(ctx->smap, 0, sizeof(ctx->smap));
	*p++ = ':';	/* tell a missing argument apart from a bad option */

	nlong = 0;
	for (i = 0; i < ctx->nitems; i++) {
		struct conf_item *item = &ctx->items[i];
		const struct conf_attr *attr = item->attr;
		int has_arg = conf_has_arg(attr);

		if (attr->name) {
			lo[nlong].name = attr->name;
			lo[nlong].has_arg = has_arg;
			lo[nlong].flag = NULL;
			lo[nlong].val = item->code;
			nlong++;
		}

		/* --no-<name> is the same attribute reached from the other
		 * side; it never takes a value. */
		if (item->negname) {
			lo[nlong].name = item->negname;
			lo[nlong].has_arg = no_argument;
			lo[nlong].flag = NULL;
			lo[nlong].val = CONF_CODE_NEG + (int)i;
			nlong++;
		}

		if (attr->letter) {
			ctx->smap[attr->letter] = (unsigned short)(i + 1);
			*p++ = (char)attr->letter;
			if (has_arg == required_argument)
				*p++ = ':';
			else if (has_arg == optional_argument) {
				*p++ = ':';
				*p++ = ':';
			}
		}
	}

	memset(&lo[nlong], 0, sizeof(lo[nlong]));
	*p = 0;

	ctx->longopts = lo;
	ctx->shortopts = so;
}

const struct option *
conf_longopts(struct conf_ctx *ctx)
{
	conf_build(ctx);
	return ctx->longopts;
}

const char *
conf_shortopts(struct conf_ctx *ctx)
{
	conf_build(ctx);
	return ctx->shortopts;
}

int
conf_dispatch(struct conf_ctx *ctx, int code, const char *value)
{
	int negated;
	struct conf_item *item;

	conf_build(ctx);

	item = conf_decode(ctx, code, &negated);
	if (!item)
		return CONF_UNKNOWN;
	if (item->attr->cls == CONF_CL_USER)
		return CONF_UNKNOWN;

	return conf_store(ctx, item, value, negated);
}

/* argv[0] without its directory, for usage and diagnostics. */
static const char *
conf_basename(const char *path)
{
	const char *slash = strrchr(path, '/');
	return (slash && slash[1]) ? slash + 1 : path;
}

/*
 * Name the option getopt_long() refused, from whatever it left behind. Say it
 * back the way it was typed: for a long option optopt holds the val the table
 * gave it, which for a long-and-short attribute is the letter - naming `-l'
 * when the user wrote `--listen' is a worse message than no message.
 */
static const char *
conf_bad_option(struct conf_ctx *ctx, char **argv, int argc)
{
	const char *arg = (optind > 1 && optind <= argc) ? argv[optind - 1]
	                                                 : NULL;

	if (arg && arg[0] == '-' && arg[1] == '-') {
		const char *eq = strchr(arg, '=');
		return eq ? conf_printf(ctx, "%.*s", (int)(eq - arg), arg) : arg;
	}
	if (optopt > 0 && optopt < 256)
		return conf_printf(ctx, "-%c", optopt);
	return arg ? arg : "option";
}

static int
conf_has_help(struct conf_ctx *ctx)
{
	unsigned int i;

	for (i = 0; i < ctx->nitems; i++)
		if (ctx->items[i].attr->name &&
		    !strcmp(ctx->items[i].attr->name, "help"))
			return 1;
	return 0;
}

static int
conf_fail(struct conf_ctx *ctx)
{
	fprintf(stderr, "%s: %s\n", ctx->progname, ctx->err);
	if (conf_has_help(ctx))
		fprintf(stderr, "Try `%s --help' for more information.\n",
		        ctx->progname);
	return CONF_ERROR;
}

int
conf_getopt(struct conf_ctx *ctx, int argc, char **argv)
{
	int c;

	conf_build(ctx);

	if (argc > 0 && argv[0])
		conf_progname(ctx, conf_basename(argv[0]));

	/* Report the failures ourselves - getopt's own messages know nothing
	 * about the program name we were given. */
	opterr = 0;
#if defined(__GLIBC__)
	optind = 0;	/* glibc's full reinitialisation */
#else
	optind = 1;
#endif

	while ((c = getopt_long(argc, argv, ctx->shortopts, ctx->longopts,
	                        NULL)) != -1) {
		if (c == '?') {
			conf_error(ctx, "unrecognized option `%s'",
			           conf_bad_option(ctx, argv, argc));
			return conf_fail(ctx);
		}
		if (c == ':') {
			conf_error(ctx, "option `%s' requires an argument",
			           conf_bad_option(ctx, argv, argc));
			return conf_fail(ctx);
		}

		switch (conf_dispatch(ctx, c, optarg)) {
		case CONF_OK:
			break;
		case CONF_UNKNOWN:
			/* A CONF_USER attribute, or a code from a table this
			 * loop did not build. Either way it is the caller's to
			 * handle, and this loop is not the caller. */
			conf_error(ctx, "option `%s' has to be handled by the "
			           "program's own getopt_long() loop",
			           conf_bad_option(ctx, argv, argc));
			return conf_fail(ctx);
		default:
			return conf_fail(ctx);
		}
	}

	if (conf_check(ctx) != CONF_OK)
		return conf_fail(ctx);

	return optind;
}
