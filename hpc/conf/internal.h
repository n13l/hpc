/*
 * Shared internals of the configuration module. Not a public header: programs
 * include <hpc/conf.h> and hold `struct conf_ctx *` opaquely.
 */

#ifndef __HPC_CONF_INTERNAL_H__
#define __HPC_CONF_INTERNAL_H__

#include <hpc/compiler.h>
#include <hpc/conf.h>
#include <mem/alloc.h>
#include <stdarg.h>

__BEGIN_DECLS

/*
 * Everything a context allocates is one chunk off its `struct mm`, carrying
 * the link that puts it on the context's list. conf_free() walks that list.
 *
 * The list is what makes the module work with any allocator hpc offers rather
 * than one in particular: an mm whose free() is a no-op - an arena, a pool -
 * gets the walk for nothing and reclaims in one go when it is destroyed, and
 * mm_libc(), the default, needs the walk to not leak.
 */
struct conf_chunk {
	struct conf_chunk *next;
};

/*
 * How an item index becomes a getopt_long() return code. An attribute with a
 * short letter uses the letter itself, so the generated table reads exactly
 * like a hand-written one; anything else takes a code out of the reserved
 * range, and the generated --no-<name> twin takes one from its upper half.
 */
#define CONF_CODE_NEG 0x30000000

/*
 * The flattened index: one entry per attribute of every registered section,
 * in registration order. It is what a getopt_long() return code decodes to,
 * and the only place per-run state lives - the declarations themselves stay
 * const, so the same tables can be shared by two contexts at once.
 */
struct conf_item {
	const struct conf_section *sec;
	const struct conf_attr *attr;
	const char *negname;		/* "no-<name>" for a boolean, else NULL */
	int code;			/* what getopt_long() returns for it */
	unsigned int count;		/* times it has been given */
};

struct conf_ctx {
	struct mm *mm;
	struct conf_chunk *chunks;	/* everything allocated, newest first */

	const struct conf_section **secs;
	unsigned int nsecs, csecs;	/* used, capacity */

	struct conf_item *items;
	unsigned int nitems, citems;

	struct option *longopts;	/* built lazily, NULL until then */
	char *shortopts;
	unsigned short smap[256];	/* short letter -> item index + 1 */

	const char *progname;
	const char *err;
	unsigned int depth;		/* conf_load() nesting, capped */
	unsigned int generation;	/* values changed since startup */
	int running;			/* startup is over: CONF_RUNTIME only */
};

/* Allocation from the context's mm, tracked for conf_free(). */
void *conf_alloc_mem(struct conf_ctx *ctx, size_t size);
char *conf_alloc_str(struct conf_ctx *ctx, const char *str, size_t len);
char *conf_printf(struct conf_ctx *ctx, const char *fmt, ...)
	_format_check(printf, 2, 3);
char *conf_vprintf(struct conf_ctx *ctx, const char *fmt, va_list args);

/* Build the getopt_long() tables if they are not built yet. Cannot fail:
 * everything that could be rejected was rejected by conf_add(). */
void conf_build(struct conf_ctx *ctx);

/* "--listen" or "-l", whichever the attribute has, for diagnostics. */
const char *conf_item_name(struct conf_ctx *ctx, const struct conf_item *item);

/* Apply @value to @item, as the option's declaration says to. @negated is set
 * for the generated --no-<name> form of a boolean. */
int conf_store(struct conf_ctx *ctx, struct conf_item *item,
               const char *value, int negated);

/* Decode a getopt_long() return code back to its item, or NULL. */
struct conf_item *conf_decode(struct conf_ctx *ctx, int code, int *negated);

/* Does this attribute carry a value on the command line? Returns one of
 * no_argument / required_argument / optional_argument. */
int conf_has_arg(const struct conf_attr *attr);

/* Is it an option at all, as opposed to a help line or an inert slot? */
static inline int
conf_is_option(const struct conf_attr *attr)
{
	return attr->cls != CONF_CL_HELP && attr->cls != CONF_CL_END &&
	       (attr->name || attr->letter);
}

/* Does the declaration ask for a --no-<name> twin? */
static inline int
conf_is_negatable(const struct conf_attr *attr)
{
	return attr->cls == CONF_CL_STATIC && attr->type == CONF_T_BOOL &&
	       attr->name && !(attr->flags & CONF_NO_NEGATION);
}

__END_DECLS

#endif
