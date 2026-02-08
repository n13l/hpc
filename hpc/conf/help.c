/*
 * The usage text.
 *
 * With CONFIG_SECTION this is the whole point of declaring sections: the help
 * is grouped by section, in registration order, with each attribute's own
 * description beside its spelling - and it cannot fall out of step with the
 * options, because it is generated from the same array getopt_long() is.
 *
 * Without CONFIG_SECTION there are no descriptions to print, because there are
 * none in the binary: the declaration macros never named the strings. What is
 * still knowable is which options exist and whether they take a value, so that
 * is what --help prints.
 */

#include <hpc/compiler.h>
#include <hpc/conf.h>
#include <conf/internal.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONF_HELP_MIN_COL 22
#define CONF_HELP_MAX_COL 34

/*
 * "-l, --listen=ADDR", "    --backlog=N", "-v, --verbose". @arg is the metavar
 * to show, or NULL for an option that takes no value.
 */
static size_t
conf_spell(char *buf, size_t size, const struct conf_attr *attr,
           const char *arg)
{
	size_t n = 0;

	if (attr->letter && attr->name)
		n = (size_t)snprintf(buf, size, "-%c, --%s", attr->letter,
		                     attr->name);
	else if (attr->letter)
		n = (size_t)snprintf(buf, size, "-%c", attr->letter);
	else
		n = (size_t)snprintf(buf, size, "    --%s", attr->name);

	if (arg && n < size) {
		int optional = conf_has_arg(attr) == optional_argument;
		const char *fmt = attr->name
			? (optional ? "[=%s]" : "=%s")
			: (optional ? "[%s]" : " %s");
		n += (size_t)snprintf(buf + n, size - n, fmt, arg);
	}

	return n;
}

#ifdef CONFIG_SECTION

/* Pad to @col and write @text, indenting any continuation lines to match. */
static void
conf_help_text(FILE *f, size_t written, unsigned int col, const char *text)
{
	const char *nl;

	if (written >= col) {
		fputc('\n', f);
		written = 0;
	}
	fprintf(f, "%*s", (int)(col - written), "");

	while ((nl = strchr(text, '\n'))) {
		fprintf(f, "%.*s\n%*s", (int)(nl - text), text, (int)col, "");
		text = nl + 1;
	}
	fprintf(f, "%s\n", text);
}

/*
 * The metavar to show. Where the declaration left it out, stand in for a value
 * the option insists on - not for one it merely tolerates, since a boolean's
 * `--daemon[=VALUE]` is noise around the spelling anyone actually types.
 */
static const char *
conf_metavar(const struct conf_attr *attr)
{
	if (attr->arg)
		return attr->arg;
	return conf_has_arg(attr) == required_argument ? "VALUE" : NULL;
}

static int
conf_visible(const struct conf_attr *attr)
{
	return conf_is_option(attr) && attr->help;
}

void
conf_help(struct conf_ctx *ctx, FILE *f)
{
	char buf[128];
	unsigned int col = CONF_HELP_MIN_COL, i;
	const struct conf_attr *attr;

	/* One pass to find how wide the spellings run, so the descriptions
	 * line up across every section rather than per section. */
	for (i = 0; i < ctx->nsecs; i++)
		for (attr = ctx->secs[i]->attr; attr->cls != CONF_CL_END; attr++) {
			if (!conf_visible(attr))
				continue;
			size_t n = conf_spell(buf, sizeof(buf), attr,
			                      conf_metavar(attr));
			if (n + 4 > col)
				col = (unsigned int)(n + 4);
		}

	if (col > CONF_HELP_MAX_COL)
		col = CONF_HELP_MAX_COL;

	fprintf(f, "usage: %s [options]\n", ctx->progname);

	for (i = 0; i < ctx->nsecs; i++) {
		const struct conf_section *sec = ctx->secs[i];
		int headed = 0;

		for (attr = sec->attr; attr->cls != CONF_CL_END; attr++) {
			if (attr->cls == CONF_CL_HELP) {
				if (attr->help)
					fprintf(f, "\n%s\n", attr->help);
				continue;
			}
			if (!conf_visible(attr))
				continue;

			if (!headed) {
				const char *title = sec->help ? sec->help
				                              : sec->name;
				fprintf(f, "\n%s\n", title ? title : "options");
				headed = 1;
			}

			size_t n = conf_spell(buf, sizeof(buf), attr,
			                      conf_metavar(attr));
			fprintf(f, "  %s", buf);
			conf_help_text(f, n + 2, col, attr->help);
		}
	}
}

#else /* !CONFIG_SECTION */

void
conf_help(struct conf_ctx *ctx, FILE *f)
{
	char buf[128];
	unsigned int i;

	conf_build(ctx);
	fprintf(f, "usage: %s [options]\n\n", ctx->progname);

	for (i = 0; i < ctx->nitems; i++) {
		const struct conf_attr *attr = ctx->items[i].attr;
		const char *arg = conf_has_arg(attr) == no_argument ? NULL
		                                                    : "VALUE";

		conf_spell(buf, sizeof(buf), attr, arg);
		fprintf(f, "  %s\n", buf);
	}
}

#endif /* CONFIG_SECTION */

int
conf_handle_help(struct conf_ctx *ctx, const struct conf_attr *attr,
                 const char *value, void *data)
{
	(void)attr; (void)value; (void)data;

	conf_help(ctx, stdout);
	exit(0);
}
