/*
 * Turning a string into an attribute's value.
 *
 * The parsers are the same ones the command line, -S and the configuration
 * file all go through, which is the point: `--backlog 4K`, `-Snet.backlog=4K`
 * and `backlog 4K` in a file cannot disagree about what 4K means.
 */

#include <hpc/compiler.h>
#include <hpc/conf.h>
#include <conf/internal.h>
#include <mem/alloc.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/*
 * A trailing K, M, G or T multiplies by a power of 1024. Returns the
 * multiplier and moves @end past the suffix, or 0 if what follows the number
 * is not one.
 */
static uint64_t
conf_unit(const char **end)
{
	const char *p = *end;
	uint64_t mul;

	switch (*p) {
	case 'k': case 'K': mul = 1024ull; break;
	case 'm': case 'M': mul = 1024ull * 1024; break;
	case 'g': case 'G': mul = 1024ull * 1024 * 1024; break;
	case 't': case 'T': mul = 1024ull * 1024 * 1024 * 1024; break;
	default: return 0;
	}

	*end = p + 1;
	return mul;
}

static const char *
conf_parse_ull(const char *str, uint64_t *ptr, int *neg)
{
	const char *end;
	char *stop;
	uint64_t val, mul;

	while (*str == ' ' || *str == '\t')
		str++;
	if (!*str)
		return "expected a number";

	*neg = (*str == '-');
	if (*neg || *str == '+')
		str++;

	errno = 0;
	val = strtoull(str, &stop, 0);
	if (stop == str)
		return "expected a number";
	if (errno == ERANGE)
		return "number out of range";

	end = stop;
	if ((mul = conf_unit(&end))) {
		if (val > UINT64_MAX / mul)
			return "number out of range";
		val *= mul;
	}

	while (*end == ' ' || *end == '\t')
		end++;
	if (*end)
		return "trailing garbage after a number";

	*ptr = val;
	return NULL;
}

const char *
conf_parse_u64(const char *str, uint64_t *ptr)
{
	uint64_t val;
	int neg;
	const char *err = conf_parse_ull(str, &val, &neg);

	if (err)
		return err;
	if (neg)
		return "expected a non-negative number";

	*ptr = val;
	return NULL;
}

const char *
conf_parse_int(const char *str, int *ptr)
{
	uint64_t val;
	int neg;
	const char *err = conf_parse_ull(str, &val, &neg);

	if (err)
		return err;
	if (neg ? val > 0x80000000ull : val > 0x7fffffffull)
		return "number out of range for an int";

	*ptr = neg ? -(int)val : (int)val;
	return NULL;
}

const char *
conf_parse_uint(const char *str, unsigned int *ptr)
{
	uint64_t val;
	int neg;
	const char *err = conf_parse_ull(str, &val, &neg);

	if (err)
		return err;
	if (neg)
		return "expected a non-negative number";
	if (val > 0xffffffffull)
		return "number out of range for an unsigned int";

	*ptr = (unsigned int)val;
	return NULL;
}

const char *
conf_parse_double(const char *str, double *ptr)
{
	char *end;
	double val;

	while (*str == ' ' || *str == '\t')
		str++;
	if (!*str)
		return "expected a number";

	errno = 0;
	val = strtod(str, &end);
	if (end == str)
		return "expected a number";
	if (errno == ERANGE)
		return "number out of range";

	while (*end == ' ' || *end == '\t')
		end++;
	if (*end)
		return "trailing garbage after a number";

	*ptr = val;
	return NULL;
}

static int
conf_streq_nocase(const char *a, const char *b)
{
	for (; *a && *b; a++, b++) {
		int x = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
		int y = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
		if (x != y)
			return 0;
	}
	return *a == *b;
}

const char *
conf_parse_bool(const char *str, int *ptr)
{
	static const char *const yes[] = { "1", "y", "yes", "true", "on", NULL };
	static const char *const no[]  = { "0", "n", "no", "false", "off", NULL };
	unsigned int i;

	for (i = 0; yes[i]; i++)
		if (conf_streq_nocase(str, yes[i])) {
			*ptr = 1;
			return NULL;
		}
	for (i = 0; no[i]; i++)
		if (conf_streq_nocase(str, no[i])) {
			*ptr = 0;
			return NULL;
		}

	return "expected one of yes/no, true/false, on/off, 1/0";
}

const char *
conf_parse_lookup(const char *str, const char * const *tab, int *ptr)
{
	unsigned int i;

	if (!tab)
		return "no set of values was declared for this attribute";

	for (i = 0; tab[i]; i++)
		if (conf_streq_nocase(str, tab[i])) {
			*ptr = (int)i;
			return NULL;
		}

	return "not one of the allowed values";
}

int
conf_has_arg(const struct conf_attr *attr)
{
	if (attr->flags & CONF_NO_VALUE)
		return no_argument;
	if (attr->flags & CONF_REQUIRED_VALUE)
		return required_argument;
	if (attr->flags & CONF_MAYBE_VALUE)
		return optional_argument;

	switch (attr->cls) {
	case CONF_CL_STATIC:
		/* A boolean stands alone (--verbose) or carries a word
		 * (--verbose=no); everything else needs its value. */
		return attr->type == CONF_T_BOOL ? optional_argument
		                                 : required_argument;
	case CONF_CL_SWITCH:
	case CONF_CL_INC:
		return no_argument;
	default:
		return required_argument;
	}
}

static int
conf_store_static(struct conf_ctx *ctx, struct conf_item *item,
                  const char *value, int negated)
{
	const struct conf_attr *attr = item->attr;
	const char *err = NULL;

	switch (attr->type) {
	case CONF_T_BOOL: {
		int val;
		if (!value) {
			/* Bare --flag asserts it, bare --no-flag denies it,
			 * and CONF_NEGATIVE swaps which is which. */
			val = negated ? 0 : 1;
			if (attr->flags & CONF_NEGATIVE)
				val = !val;
		} else if (!(err = conf_parse_bool(value, &val))) {
			if (negated)
				val = !val;
		}
		if (!err)
			*(int *)attr->ptr = val;
		break;
	}
	case CONF_T_INT:
		err = conf_parse_int(value, (int *)attr->ptr);
		break;
	case CONF_T_UINT:
		err = conf_parse_uint(value, (unsigned int *)attr->ptr);
		break;
	case CONF_T_U64:
		err = conf_parse_u64(value, (uint64_t *)attr->ptr);
		break;
	case CONF_T_DOUBLE:
		err = conf_parse_double(value, (double *)attr->ptr);
		break;
	case CONF_T_STRING:
		/* Copied into the context: the caller's variable stays valid
		 * for as long as the context, not only as long as argv. */
		*(char **)attr->ptr = conf_alloc_str(ctx, value, strlen(value));
		break;
	case CONF_T_LOOKUP:
		err = conf_parse_lookup(value, attr->u.lookup,
		                        (int *)attr->ptr);
		break;
	default:
		err = "attribute has no value type";
		break;
	}

	if (err)
		return conf_error(ctx, "%s: %s", conf_item_name(ctx, item), err);
	return CONF_OK;
}

int
conf_store(struct conf_ctx *ctx, struct conf_item *item, const char *value,
           int negated)
{
	const struct conf_attr *attr = item->attr;
	int rc = CONF_OK;

	if ((attr->flags & CONF_SINGLE) && item->count)
		return conf_error(ctx, "%s may be given only once",
		                  conf_item_name(ctx, item));

	/* Startup is over and this one sized something already built; see
	 * conf_running(). */
	if (ctx->running && !(attr->flags & CONF_RUNTIME))
		return conf_error(ctx, "%s cannot be changed while running",
		                  conf_item_name(ctx, item));

	/*
	 * A parsed value is the one thing that cannot be conjured from
	 * nothing - a boolean stands for itself, a switch and a counter ignore
	 * what they are given, and a handler is told to expect NULL. Refuse
	 * here rather than in each parser, and refuse it whatever the
	 * declaration claimed about arity: getopt_long() guarantees a value
	 * only for required_argument, and conf_set() answers to no one.
	 */
	if (!value && attr->cls == CONF_CL_STATIC &&
	    attr->type != CONF_T_BOOL)
		return conf_error(ctx, "%s requires a value",
		                  conf_item_name(ctx, item));

	item->count++;

	switch (attr->cls) {
	case CONF_CL_STATIC:
		rc = conf_store_static(ctx, item, value, negated);
		break;
	case CONF_CL_SWITCH:
		*(int *)attr->ptr = attr->u.value;
		break;
	case CONF_CL_INC:
		*(int *)attr->ptr += (attr->flags & CONF_NEGATIVE) ? -1 : 1;
		break;
	case CONF_CL_CALL:
		rc = attr->u.call(ctx, attr, value, attr->ptr);
		break;
	case CONF_CL_USER:
		return CONF_UNKNOWN;
	default:
		return conf_error(ctx, "%s cannot be set",
		                  conf_item_name(ctx, item));
	}

	/* One counter for everything that actually moved, so whoever holds a
	 * copy has one thing to compare (conf_generation()). A handler that
	 * failed changed nothing worth telling anyone about. */
	if (rc == CONF_OK)
		ctx->generation++;
	return rc;
}
