/*
 * The parsing context: the mm everything is allocated from, the registered
 * sections, and the flattened attribute index the getopt_long() codes decode
 * to. Nothing here depends on CONFIG_SECTION - an attribute keeps its long
 * name and short letter either way, and this is the part that only ever looks
 * at those.
 */

#include <hpc/compiler.h>
#include <hpc/conf.h>
#include <conf/internal.h>
#include <mem/alloc.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void *
conf_alloc_mem(struct conf_ctx *ctx, size_t size)
{
	struct conf_chunk *chunk = (struct conf_chunk *)
		mm_alloc(ctx->mm, sizeof(*chunk) + size);

	if (!chunk)
		return NULL;

	chunk->next = ctx->chunks;
	ctx->chunks = chunk;
	return chunk + 1;
}

char *
conf_alloc_str(struct conf_ctx *ctx, const char *str, size_t len)
{
	char *s = (char *)conf_alloc_mem(ctx, len + 1);

	memcpy(s, str, len);
	s[len] = 0;
	return s;
}

char *
conf_vprintf(struct conf_ctx *ctx, const char *fmt, va_list args)
{
	va_list copy;
	char probe[1];
	int len;

	va_copy(copy, args);
	len = vsnprintf(probe, sizeof(probe), fmt, copy);
	va_end(copy);

	if (len < 0)
		return NULL;

	char *s = (char *)conf_alloc_mem(ctx, (size_t)len + 1);
	vsnprintf(s, (size_t)len + 1, fmt, args);
	return s;
}

char *
conf_printf(struct conf_ctx *ctx, const char *fmt, ...)
{
	va_list args;
	char *s;

	va_start(args, fmt);
	s = conf_vprintf(ctx, fmt, args);
	va_end(args);
	return s;
}

struct conf_ctx *
conf_alloc(struct mm *mm)
{
	struct conf_chunk *chunk;
	struct conf_ctx *ctx;

	if (!mm)
		mm = mm_libc();

	/* The context is the first chunk on its own list, so conf_free() has
	 * exactly one thing to walk. */
	chunk = (struct conf_chunk *)mm_alloc(mm, sizeof(*chunk) + sizeof(*ctx));
	if (!chunk)
		return NULL;

	ctx = (struct conf_ctx *)(chunk + 1);
	memset(ctx, 0, sizeof(*ctx));
	chunk->next = NULL;

	ctx->mm = mm;
	ctx->chunks = chunk;
	ctx->progname = "program";
	return ctx;
}

void
conf_free(struct conf_ctx *ctx)
{
	struct conf_chunk *chunk, *next;
	struct mm *mm;

	if (!ctx)
		return;

	/* The context is on this list; take what we need off it first. */
	mm = ctx->mm;
	chunk = ctx->chunks;

	for (; chunk; chunk = next) {
		next = chunk->next;
		mm_free(mm, chunk);
	}
}

void
conf_progname(struct conf_ctx *ctx, const char *name)
{
	ctx->progname = name ? conf_alloc_str(ctx, name, strlen(name)) : "program";
}

const char *
conf_strerror(struct conf_ctx *ctx)
{
	return ctx->err;
}

void
conf_running(struct conf_ctx *ctx, int running)
{
	ctx->running = running;
}

unsigned int
conf_generation(struct conf_ctx *ctx)
{
	return ctx->generation;
}

int
conf_error(struct conf_ctx *ctx, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	ctx->err = conf_vprintf(ctx, fmt, args);
	va_end(args);
	return CONF_ERROR;
}

const char *
conf_item_name(struct conf_ctx *ctx, const struct conf_item *item)
{
	const struct conf_attr *attr = item->attr;

	if (attr->name)
		return conf_printf(ctx, "--%s", attr->name);
	return conf_printf(ctx, "-%c", attr->letter);
}

/*
 * Growing arrays without freeing the old one: allocate twice the room, copy,
 * and leave the previous block on the chunk list for conf_free(). Registration
 * happens once at startup with a handful of sections, so the copies are not
 * worth avoiding - and the alternative, a fixed cap, is a limit a program would
 * eventually hit.
 */
static void *
conf_grow(struct conf_ctx *ctx, void *old, unsigned int used,
          unsigned int *cap, size_t elem)
{
	if (used < *cap)
		return old;

	unsigned int want = *cap ? *cap * 2 : 8;
	void *fresh = conf_alloc_mem(ctx, want * elem);

	if (used)
		memcpy(fresh, old, used * elem);
	*cap = want;
	return fresh;
}

static int
conf_add_item(struct conf_ctx *ctx, const struct conf_section *sec,
              const struct conf_attr *attr)
{
	unsigned int i;

	if (attr->letter) {
		if (attr->letter < 0 || attr->letter > 255)
			return conf_error(ctx, "short option %d is not a "
			                  "character", attr->letter);
		if (attr->letter == ':' || attr->letter == '?' ||
		    attr->letter == '-')
			return conf_error(ctx, "'%c' cannot be a short option",
			                  attr->letter);
	}
	if (attr->cls == CONF_CL_USER) {
		if (attr->u.code >= CONF_CODE_BASE &&
		    attr->u.code <= CONF_CODE_LAST)
			return conf_error(ctx, "option code %#x is inside the "
			                  "range reserved by <hpc/conf.h>",
			                  attr->u.code);
		if (!attr->letter && !attr->u.code)
			return conf_error(ctx, "`--%s' needs a code to be "
			                  "reported as", attr->name);
		if (attr->letter && attr->u.code &&
		    attr->u.code != attr->letter)
			return conf_error(ctx, "`-%c' is its own code; drop the "
			                  "one given to CONF_USER",
			                  attr->letter);
	}

	for (i = 0; i < ctx->nitems; i++) {
		const struct conf_attr *other = ctx->items[i].attr;

		if (attr->letter && attr->letter == other->letter)
			return conf_error(ctx, "duplicate short option '-%c'",
			                  attr->letter);
		if (!attr->name)
			continue;
		if (other->name && !strcmp(attr->name, other->name))
			return conf_error(ctx, "duplicate long option '--%s'",
			                  attr->name);
		/* The generated twin of a boolean already registered occupies
		 * a name just as firmly as a declared one. */
		if (ctx->items[i].negname &&
		    !strcmp(attr->name, ctx->items[i].negname))
			return conf_error(ctx, "'--%s' collides with the "
			                  "negation of '--%s'", attr->name,
			                  other->name);
	}

	ctx->items = (struct conf_item *)
		conf_grow(ctx, ctx->items, ctx->nitems, &ctx->citems,
		          sizeof(*ctx->items));

	struct conf_item *item = &ctx->items[ctx->nitems];
	memset(item, 0, sizeof(*item));
	item->sec = sec;
	item->attr = attr;

	if (attr->cls == CONF_CL_USER)
		/* getopt_long() has no way to return anything but the letter
		 * for a short option, so where there is one it is the code and
		 * the long form is given the same value. */
		item->code = attr->letter ? attr->letter : attr->u.code;
	else if (attr->letter)
		item->code = attr->letter;
	else
		item->code = CONF_CODE_BASE + (int)ctx->nitems;

	/* Materialise --no-<name> now, so that its collisions are caught here
	 * with the rest and table assembly cannot fail. */
	if (conf_is_negatable(attr)) {
		item->negname = conf_printf(ctx, "no-%s", attr->name);
		for (i = 0; i < ctx->nitems; i++) {
			const struct conf_attr *other = ctx->items[i].attr;
			if (other->name && !strcmp(other->name, item->negname))
				return conf_error(ctx, "'--%s' collides with "
				                  "the negation of '--%s'",
				                  other->name, attr->name);
		}
	}

	ctx->nitems++;
	return CONF_OK;
}

int
conf_add(struct conf_ctx *ctx, const struct conf_section *sec)
{
	const struct conf_attr *attr;
	unsigned int nitems = ctx->nitems;

	if (!sec || !sec->attr)
		return conf_error(ctx, "section has no attributes");

	for (attr = sec->attr; attr->cls != CONF_CL_END; attr++) {
		if (!conf_is_option(attr))
			continue;
		if (conf_add_item(ctx, sec, attr) != CONF_OK) {
			/* Leave the index as it was: a rejected section must
			 * not be half-registered. */
			ctx->nitems = nitems;
			return CONF_ERROR;
		}
	}

	ctx->secs = (const struct conf_section **)
		conf_grow(ctx, ctx->secs, ctx->nsecs, &ctx->csecs,
		          sizeof(*ctx->secs));
	ctx->secs[ctx->nsecs++] = sec;

	/* A section added after the tables were built has to be in them. */
	ctx->longopts = NULL;
	ctx->shortopts = NULL;
	return CONF_OK;
}

struct conf_item *
conf_decode(struct conf_ctx *ctx, int code, int *negated)
{
	unsigned int idx;

	*negated = 0;

	if (code >= CONF_CODE_NEG && code <= CONF_CODE_LAST) {
		*negated = 1;
		idx = (unsigned int)(code - CONF_CODE_NEG);
	} else if (code >= CONF_CODE_BASE && code < CONF_CODE_NEG) {
		idx = (unsigned int)(code - CONF_CODE_BASE);
	} else if (code > 0 && code < 256 && ctx->smap[code]) {
		idx = ctx->smap[code] - 1u;
	} else {
		return NULL;
	}

	return idx < ctx->nitems ? &ctx->items[idx] : NULL;
}

int
conf_seen(struct conf_ctx *ctx, const char *name)
{
	unsigned int i;

	for (i = 0; i < ctx->nitems; i++) {
		const struct conf_attr *attr = ctx->items[i].attr;
		if (attr->name && !strcmp(attr->name, name))
			return ctx->items[i].count > 0;
	}
	return 0;
}

int
conf_check(struct conf_ctx *ctx)
{
	unsigned int i;

	for (i = 0; i < ctx->nitems; i++) {
		struct conf_item *item = &ctx->items[i];
		if ((item->attr->flags & CONF_REQUIRED) && !item->count)
			return conf_error(ctx, "%s is required",
			                  conf_item_name(ctx, item));
	}
	return CONF_OK;
}
