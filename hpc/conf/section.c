/*
 * The section namespace: reaching an attribute by "<section>.<attribute>"
 * rather than by its option letter, which is what -S, a configuration file and
 * --dumpconfig all do.
 *
 * And the other direction, which the same namespace is what makes possible:
 * saying what the names are (conf_walk), what one of them holds (conf_value)
 * and what the declaration said about it (conf_describe). A program that can be
 * asked to change an attribute by name can be asked what its attributes are, and
 * the answer is built from the same array getopt_long() runs on rather than
 * written down a second time.
 *
 * This whole file is CONFIG_SECTION - without it there are no section names
 * and no attribute descriptions in the binary to address, and <hpc/conf.h>
 * turns conf_set(), conf_load(), conf_dump() and the three above into stubs
 * instead.
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

/*
 * The value as a reader would be shown it: what conf_dump() writes below, minus
 * the quoting a file needs and a reader does not. Returns what was written, or 0
 * for an attribute with no value behind it.
 */
static size_t
conf_render(const struct conf_attr *attr, char *buf, size_t size)
{
	int n;

	if (!size)
		return 0;
	buf[0] = '\0';

	if (!conf_dump_readable(attr))
		return 0;

	if (attr->cls != CONF_CL_STATIC) {
		n = snprintf(buf, size, "%d", *(int *)attr->ptr);
		goto done;	/* a switch or a counter, both ints */
	}

	switch (attr->type) {
	case CONF_T_BOOL:
		n = snprintf(buf, size, "%s", *(int *)attr->ptr ? "yes" : "no");
		break;
	case CONF_T_INT:
		n = snprintf(buf, size, "%d", *(int *)attr->ptr);
		break;
	case CONF_T_UINT:
		n = snprintf(buf, size, "%u", *(unsigned int *)attr->ptr);
		break;
	case CONF_T_U64:
		n = snprintf(buf, size, "%llu",
		             (unsigned long long)*(uint64_t *)attr->ptr);
		break;
	case CONF_T_DOUBLE:
		n = snprintf(buf, size, "%g", *(double *)attr->ptr);
		break;
	case CONF_T_STRING:
		n = snprintf(buf, size, "%s", *(char **)attr->ptr ?
		             *(char **)attr->ptr : "");
		break;
	case CONF_T_LOOKUP: {
		int i = *(int *)attr->ptr;
		const char * const *tab = attr->u.lookup;
		unsigned int count = 0;

		while (tab && tab[count])
			count++;
		/* out of range: say what it is rather than nothing */
		if (i >= 0 && (unsigned int)i < count)
			n = snprintf(buf, size, "%s", tab[i]);
		else
			n = snprintf(buf, size, "%d", i);
		break;
	}
	default:
		return 0;
	}

done:
	if (n < 0)
		return 0;
	return (size_t)n < size ? (size_t)n : size - 1;
}

/*
 * The same value in the file's own syntax, which differs in one thing: the two
 * shapes that are words rather than numbers are quoted, so that what is written
 * reads back as one value however much space is in it.
 */
static void
conf_dump_value(FILE *f, const struct conf_attr *attr)
{
	char buf[512];

	if (attr->cls == CONF_CL_STATIC && attr->type == CONF_T_STRING) {
		conf_dump_string(f, *(char **)attr->ptr);
		return;
	}
	if (attr->cls == CONF_CL_STATIC && attr->type == CONF_T_LOOKUP) {
		int i = *(int *)attr->ptr;
		const char * const *tab = attr->u.lookup;
		unsigned int count = 0;

		while (tab && tab[count])
			count++;
		/* a name is quoted; an index that named nothing is written as
		 * the number it is, so a reader can see what it was */
		if (i >= 0 && (unsigned int)i < count)
			conf_dump_string(f, tab[i]);
		else
			fprintf(f, "%d", i);
		return;
	}

	if (conf_render(attr, buf, sizeof(buf)))
		fputs(buf, f);
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

/*** Introspection: what the names are, and what they mean ***/

/*
 * Fill @d in from one item. @buf holds the rendered value and has to outlive
 * @d, which is why it is the caller's and not a static: a walk that rendered
 * into shared storage would hand a visitor that kept a description a value
 * belonging to the next attribute.
 */
static void
conf_desc_fill(struct conf_ctx *ctx, const struct conf_item *item,
               struct conf_desc *d, char *buf, size_t size)
{
	const struct conf_attr *attr = item->attr;

	d->section	= item->sec->name;
	d->sechelp	= item->sec->help;
	d->name		= attr->name;
	d->arg		= attr->arg;
	d->help		= attr->help;
	d->lookup	= attr->type == CONF_T_LOOKUP ? attr->u.lookup : NULL;
	d->letter	= attr->letter;
	d->flags	= attr->flags;
	d->count	= item->count;
	d->runtime	= (attr->flags & CONF_RUNTIME) != 0;
	/*
	 * What a `set` would do with it right now, which is the question an
	 * operator is actually asking. Before conf_running() everything is
	 * settable; after it, only what said so — and a CONF_USER attribute
	 * never was, its effect belonging to whoever drives getopt_long().
	 */
	d->settable	= attr->cls != CONF_CL_USER &&
	                  (!ctx->running || d->runtime);
	d->readable	= conf_render(attr, buf, size) > 0;
	d->value	= buf;
}

/*
 * Does @prefix select this item? A whole name, a section, or a stem of one:
 * "net.tls.record-max" is that attribute, "net.tls" is its section, and "net"
 * is every section under it. The dot has to be there for a stem — "net.t"
 * naming net.tls would be a surprise, and a section is free to be a prefix of
 * another's name.
 */
static int
conf_prefix_match(const struct conf_item *item, const char *prefix, size_t len)
{
	const char *sec = item->sec->name;
	size_t seclen;

	if (!len)
		return 1;
	if (!sec)
		return 0;

	seclen = strlen(sec);

	if (len == seclen && !strncmp(sec, prefix, len))
		return 1;			/* the section itself */

	if (len > seclen && prefix[seclen] == '.' &&
	    !strncmp(sec, prefix, seclen))	/* section.attribute */
		return !strcmp(item->attr->name, prefix + seclen + 1);

	if (len < seclen && sec[len] == '.' && !strncmp(sec, prefix, len))
		return 1;			/* a stem of the section */

	/* and a bare attribute name, the spelling conf_set() also takes */
	return !strcmp(item->attr->name, prefix);
}

int
conf_walk(struct conf_ctx *ctx, const char *prefix, conf_visitor *fn, void *arg)
{
	size_t len = prefix ? strlen(prefix) : 0;
	unsigned int i;

	if (!fn)
		return CONF_ERROR;

	for (i = 0; i < ctx->nitems; i++) {
		const struct conf_item *item = &ctx->items[i];
		char buf[512];
		struct conf_desc d;
		int rc;

		if (!item->attr->name || !conf_is_option(item->attr))
			continue;
		if (!conf_prefix_match(item, prefix, len))
			continue;

		conf_desc_fill(ctx, item, &d, buf, sizeof(buf));
		if ((rc = fn(&d, arg)))
			return rc;
	}

	return CONF_OK;
}

int
conf_describe(struct conf_ctx *ctx, const char *name, struct conf_desc *desc,
              char *buf, size_t size)
{
	int ambiguous;
	struct conf_item *item = conf_find(ctx, name, &ambiguous);

	if (ambiguous)
		return conf_error(ctx, "`%s' is ambiguous, qualify it with a "
		                  "section name", name);
	if (!item)
		return CONF_UNKNOWN;

	conf_desc_fill(ctx, item, desc, buf, size);
	return CONF_OK;
}

int
conf_value(struct conf_ctx *ctx, const char *name, char *buf, size_t size)
{
	int ambiguous;
	struct conf_item *item = conf_find(ctx, name, &ambiguous);

	if (ambiguous)
		return conf_error(ctx, "`%s' is ambiguous, qualify it with a "
		                  "section name", name);
	if (!item)
		return CONF_UNKNOWN;
	if (!conf_render(item->attr, buf, size))
		return conf_error(ctx, "`%s' has no value to read: it is "
		                  "handled where it is given", name);
	return CONF_OK;
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
