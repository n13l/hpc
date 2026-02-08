/*
 * The section namespace: reaching an attribute by "<section>.<attribute>"
 * rather than by its option letter, which is what -S, a configuration file and
 * --dumpconfig all do.
 *
 * This whole file is CONFIG_SECTION - without it there are no section names
 * and no attribute descriptions in the binary to address, and <hpc/conf.h>
 * turns conf_set(), conf_load() and conf_dump() into stubs instead.
 */

#include <hpc/compiler.h>
#include <hpc/conf.h>
#include <conf/internal.h>
#include <mem/alloc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONF_MAX_INCLUDE 8	/* how deep --config may chain */

static int
conf_name_eq(const char *a, const char *b, size_t len)
{
	return a && strlen(a) == len && !strncmp(a, b, len);
}

/*
 * Look up "section.attribute", or a bare "attribute" if exactly one section
 * has one by that name. Returns the item, NULL if there is none, and sets
 * @ambiguous when a bare name matched more than once.
 *
 * The split is at the last dot, not the first, so a section may name itself
 * hierarchically - "net.ip", "net.tls" - and "net.ip.frag-timeout" still means
 * the attribute "frag-timeout" of the section "net.ip". An attribute name may
 * therefore not contain a dot, which is no loss: it is also a long option.
 */
static struct conf_item *
conf_find(struct conf_ctx *ctx, const char *name, int *ambiguous)
{
	const char *dot = strrchr(name, '.');
	struct conf_item *found = NULL;
	unsigned int i;

	*ambiguous = 0;

	for (i = 0; i < ctx->nitems; i++) {
		struct conf_item *item = &ctx->items[i];
		const char *attr = item->attr->name;

		if (!attr)
			continue;

		if (dot) {
			if (!conf_name_eq(item->sec->name, name,
			                  (size_t)(dot - name)))
				continue;
			if (strcmp(attr, dot + 1))
				continue;
			return item;
		}

		if (strcmp(attr, name))
			continue;
		if (found) {
			*ambiguous = 1;
			return NULL;
		}
		found = item;
	}

	return found;
}

int
conf_set(struct conf_ctx *ctx, const char *name, const char *value)
{
	int ambiguous;
	struct conf_item *item = conf_find(ctx, name, &ambiguous);

	if (ambiguous)
		return conf_error(ctx, "`%s' is ambiguous, qualify it with a "
		                  "section name", name);
	if (!item)
		return CONF_UNKNOWN;
	if (item->attr->cls == CONF_CL_USER)
		return conf_error(ctx, "`%s' can only be set on the command "
		                  "line", name);

	return conf_store(ctx, item, value, 0);
}

int
conf_set_assign(struct conf_ctx *ctx, const char *assignment)
{
	const char *eq = strchr(assignment, '=');
	char *name;
	int rc;

	if (!eq)
		return conf_error(ctx, "`%s' is not <section>.<attribute>=<value>",
		                  assignment);

	name = conf_alloc_str(ctx, assignment, (size_t)(eq - assignment));
	rc = conf_set(ctx, name, eq + 1);
	if (rc == CONF_UNKNOWN)
		return conf_error(ctx, "no such configuration attribute: `%s'",
		                  name);
	return rc;
}

/*** Configuration file ***/

static char *
conf_read_file(struct conf_ctx *ctx, const char *path, size_t *size)
{
	FILE *f = fopen(path, "rb");
	char *buf;
	size_t cap = 0, len = 0;

	if (!f) {
		conf_error(ctx, "cannot open `%s'", path);
		return NULL;
	}

	/* Read into a context buffer, doubling it: the size a stat() reports
	 * is only advice for anything that is not a plain file. */
	cap = 4096;
	buf = (char *)conf_alloc_mem(ctx, cap);

	for (;;) {
		size_t got = fread(buf + len, 1, cap - len - 1, f);
		len += got;
		if (got == 0)
			break;
		if (len + 1 < cap)
			continue;

		char *grown = (char *)conf_alloc_mem(ctx, cap * 2);
		memcpy(grown, buf, len);
		buf = grown;
		cap *= 2;
	}

	if (ferror(f)) {
		fclose(f);
		conf_error(ctx, "error reading `%s'", path);
		return NULL;
	}

	fclose(f);
	buf[len] = 0;
	*size = len;
	return buf;
}

static char *
conf_trim(char *s)
{
	char *end;

	while (*s == ' ' || *s == '\t')
		s++;
	end = s + strlen(s);
	while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
	                   end[-1] == '\r'))
		end--;
	*end = 0;
	return s;
}

/*
 * Unescape a double-quoted value in place. @p points just past the opening
 * quote; returns the value, or NULL if the quote is never closed.
 */
static char *
conf_unquote(char *p, char **rest)
{
	char *out = p, *val = p;

	while (*p && *p != '"') {
		if (*p == '\\' && p[1]) {
			p++;
			switch (*p) {
			case 'n': *out++ = '\n'; break;
			case 't': *out++ = '\t'; break;
			case 'r': *out++ = '\r'; break;
			case '0': *out++ = '\0'; break;
			default:  *out++ = *p;   break;
			}
			p++;
			continue;
		}
		*out++ = *p++;
	}

	if (*p != '"')
		return NULL;

	*out = 0;
	*rest = p + 1;
	return val;
}

int
conf_load(struct conf_ctx *ctx, const char *path)
{
	char *buf, *line, *next, *section = NULL;
	size_t size;
	unsigned int lineno = 0;
	int rc = CONF_OK;

	if (ctx->depth >= CONF_MAX_INCLUDE)
		return conf_error(ctx, "configuration files nest more than "
		                  "%d deep", CONF_MAX_INCLUDE);

	buf = conf_read_file(ctx, path, &size);
	if (!buf)
		return CONF_ERROR;

	ctx->depth++;

	for (line = buf; line && *line; line = next) {
		char *value = NULL, *key, *p;

		next = strchr(line, '\n');
		if (next)
			*next++ = 0;
		lineno++;

		line = conf_trim(line);
		if (!*line || *line == '#' || *line == ';')
			continue;

		/* [section] - everything below is that section's namespace */
		if (*line == '[') {
			p = strchr(line, ']');
			if (!p) {
				rc = conf_error(ctx, "%s:%u: unterminated "
				                "section header", path, lineno);
				break;
			}
			*p = 0;
			section = conf_trim(line + 1);
			if (!*section) {
				rc = conf_error(ctx, "%s:%u: empty section "
				                "name", path, lineno);
				break;
			}
			continue;
		}

		/* key, then an optional '=', then the value */
		key = line;
		for (p = line; *p && *p != ' ' && *p != '\t' && *p != '='; p++)
			;
		if (*p) {
			int had_eq = (*p == '=');
			*p++ = 0;
			while (*p == ' ' || *p == '\t')
				p++;
			if (!had_eq && *p == '=') {
				p++;
				while (*p == ' ' || *p == '\t')
					p++;
			}
		}

		if (*p == '"') {
			char *rest;
			value = conf_unquote(p + 1, &rest);
			if (!value) {
				rc = conf_error(ctx, "%s:%u: unterminated "
				                "string", path, lineno);
				break;
			}
			rest = conf_trim(rest);
			if (*rest && *rest != '#' && *rest != ';') {
				rc = conf_error(ctx, "%s:%u: trailing garbage "
				                "after a string", path, lineno);
				break;
			}
		} else {
			char *hash = strchr(p, '#');
			if (hash)
				*hash = 0;
			value = conf_trim(p);
		}

		/* A dotted key names its own section; a bare one belongs to
		 * the [section] it is under. */
		if (!strchr(key, '.') && section)
			key = conf_printf(ctx, "%s.%s", section, key);

		rc = conf_set(ctx, key, value);
		if (rc == CONF_UNKNOWN)
			rc = conf_error(ctx, "no such configuration attribute: "
			                "`%s'", key);
		if (rc != CONF_OK) {
			/* conf_set() already said what was wrong with the
			 * value; say where it was. */
			ctx->err = conf_printf(ctx, "%s:%u: %s", path,
			                          lineno, ctx->err);
			break;
		}
	}

	ctx->depth--;
	return rc;
}

/*** Dumping ***/

static void
conf_dump_string(FILE *f, const char *s)
{
	if (!s) {
		fputs("\"\"", f);
		return;
	}

	fputc('"', f);
	for (; *s; s++) {
		switch (*s) {
		case '"':  fputs("\\\"", f); break;
		case '\\': fputs("\\\\", f); break;
		case '\n': fputs("\\n", f); break;
		case '\t': fputs("\\t", f); break;
		case '\r': fputs("\\r", f); break;
		default:   fputc(*s, f);     break;
		}
	}
	fputc('"', f);
}

/* Whether there is a value behind this attribute to write down at all. */
static int
conf_dump_readable(const struct conf_attr *attr)
{
	return attr->cls == CONF_CL_SWITCH || attr->cls == CONF_CL_INC ||
	       (attr->cls == CONF_CL_STATIC && attr->type != CONF_T_NONE);
}

static void
conf_dump_value(FILE *f, const struct conf_attr *attr)
{
	if (attr->cls != CONF_CL_STATIC) {
		fprintf(f, "%d", *(int *)attr->ptr);	/* switch, counter */
		return;
	}

	switch (attr->type) {
	case CONF_T_BOOL:
		fputs(*(int *)attr->ptr ? "yes" : "no", f);
		break;
	case CONF_T_INT:
		fprintf(f, "%d", *(int *)attr->ptr);
		break;
	case CONF_T_UINT:
		fprintf(f, "%u", *(unsigned int *)attr->ptr);
		break;
	case CONF_T_U64:
		fprintf(f, "%llu", (unsigned long long)*(uint64_t *)attr->ptr);
		break;
	case CONF_T_DOUBLE:
		fprintf(f, "%g", *(double *)attr->ptr);
		break;
	case CONF_T_STRING:
		conf_dump_string(f, *(char **)attr->ptr);
		break;
	case CONF_T_LOOKUP: {
		int i = *(int *)attr->ptr;
		const char * const *tab = attr->u.lookup;
		unsigned int n = 0;

		while (tab && tab[n])
			n++;
		if (i >= 0 && (unsigned int)i < n)
			conf_dump_string(f, tab[i]);
		else
			fprintf(f, "%d", i);	/* out of range - say what it is */
		break;
	}
	default:
		break;
	}
}

void
conf_dump(struct conf_ctx *ctx, FILE *f)
{
	unsigned int i;

	for (i = 0; i < ctx->nsecs; i++) {
		const struct conf_section *sec = ctx->secs[i];
		const struct conf_attr *attr;
		int headed = 0;

		for (attr = sec->attr; attr->cls != CONF_CL_END; attr++) {
			if (!conf_is_option(attr) || !attr->name)
				continue;

			/* An attribute whose effect a handler carried off
			 * somewhere has no value here to write down. */
			if (!conf_dump_readable(attr))
				continue;

			if (!headed && sec->name) {
				fprintf(f, "%s[%s]\n", i ? "\n" : "", sec->name);
				headed = 1;
			}

			fprintf(f, "%s ", attr->name);
			conf_dump_value(f, attr);
			fputc('\n', f);
		}
	}
}

/*** Handlers behind CONF_SET_OPTION, CONF_FILE_OPTION, CONF_DUMP_OPTION ***/

int
conf_handle_set(struct conf_ctx *ctx, const struct conf_attr *attr,
                const char *value, void *data)
{
	(void)attr; (void)data;
	return conf_set_assign(ctx, value);
}

int
conf_handle_load(struct conf_ctx *ctx, const struct conf_attr *attr,
                 const char *value, void *data)
{
	(void)attr; (void)data;
	return conf_load(ctx, value);
}

int
conf_handle_dump(struct conf_ctx *ctx, const struct conf_attr *attr,
                 const char *value, void *data)
{
	(void)attr; (void)value; (void)data;

	conf_dump(ctx, stdout);
	exit(0);
}
