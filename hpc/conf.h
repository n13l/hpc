/*
 * Configuration sections and attributes, and the getopt_long() tables built
 * from them.
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2012-2018                          Daniel Kubec <n13l@rtfm.cz>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"),to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * One declaration, three consumers
 * --------------------------------
 *
 * A program declares its knobs once, as attributes grouped into sections:
 *
 *   static char *listen = "0.0.0.0";
 *   static int   backlog = 128;
 *   static int   verbose;
 *
 *   DEFINE_CONF_SECTION(net_conf, "net", "Networking",
 *     CONF_STRING('l', "listen",  listen,  0, "ADDR", "Address to bind to"),
 *     CONF_INT   (0,   "backlog", backlog, 0, "N",    "Listen backlog"),
 *     CONF_INC   ('v', "verbose", verbose, 0, NULL,   "Increase verbosity"),
 *     CONF_HELP_OPTION
 *   );
 *
 * and the same declaration then drives all three of
 *
 *   - the getopt_long() tables: --listen, -l, and the `struct option[]` and
 *     short-option string that go with them, generated, never hand-kept;
 *   - the --help text, grouped under the section headline;
 *   - the `net.listen` namespace: -Snet.listen=..., a config file, --dumpconfig.
 *
 * The option names are the ones written above, verbatim: a section is a
 * namespace and a help grouping, never a prefix on the command line. The
 * generated table is exactly the table you would have written by hand, which
 * is what lets a program keep driving getopt_long() itself (see conf_dispatch()
 * and CONF_USER) instead of handing the loop over to conf_getopt().
 *
 * CONFIG_SECTION
 * --------------
 *
 * Everything in the paragraph above that is not needed to feed getopt_long()
 * is gated on CONFIG_SECTION, and gated in the *declaration*: with the symbol
 * off, the section name, the section headline, and every attribute's metavar
 * and description are not fields of the structures at all, so the macros never
 * name the string literals and the linker never sees them. What survives is
 * the long name, the short letter, the target pointer and its type - the
 * parameters getopt_long() and the value parser need, and nothing else.
 *
 * The API does not change shape: conf_set(), conf_load() and conf_dump() become
 * inline stubs returning CONF_UNKNOWN (there are no names left to look up),
 * conf_help() prints the bare option list with no descriptions, and
 * CONF_SET_OPTION, CONF_FILE_OPTION and CONF_DUMP_OPTION expand to an inert
 * slot, so the options themselves disappear from the command line. Source that
 * compiled with sections on still compiles with them off.
 */

#ifndef __HPC_CONF_H__
#define __HPC_CONF_H__

#include <hpc/compiler.h>
#include <getopt.h>
#include <stdio.h>
#include <stdint.h>

struct mm;
struct conf_ctx;
struct conf_attr;

__BEGIN_DECLS

/* Return codes shared by everything here that can half-fail. */
#define CONF_OK       0		/* handled */
#define CONF_UNKNOWN  1		/* not an attribute of ours - caller's turn */
#define CONF_ERROR  (-1)	/* failed, conf_strerror() says why */

/*
 * What an attribute does with the value it is given. The class is picked by
 * the declaration macros; only CONF_CL_END is meant to be spelled by hand,
 * and only through CONF_END.
 */
enum conf_class {
	CONF_CL_END = 0,	/* terminator - must stay 0 */
	CONF_CL_STATIC,		/* parse the value into *ptr by .type */
	CONF_CL_SWITCH,		/* store u.value into *(int *)ptr */
	CONF_CL_INC,		/* ++(*(int *)ptr), or -- with CONF_NEGATIVE */
	CONF_CL_CALL,		/* hand the value to u.call */
	CONF_CL_USER,		/* not ours: report u.code and let the caller do it */
	CONF_CL_HELP,		/* not an option at all - a line of help text */
};

/* Type of the value an attribute of class CONF_CL_STATIC parses. */
enum conf_type {
	CONF_T_NONE = 0,
	CONF_T_BOOL,		/* int:      1/0, y/n, yes/no, true/false, on/off */
	CONF_T_INT,		/* int */
	CONF_T_UINT,		/* unsigned int */
	CONF_T_U64,		/* uint64_t */
	CONF_T_DOUBLE,		/* double */
	CONF_T_STRING,		/* char *, copied into the context pool */
	CONF_T_LOOKUP,		/* int: index into u.lookup */
};

/*
 * Attribute flags. The three value flags override the default arity the class
 * would otherwise imply (required for a parsed value, optional for a bool,
 * none for a switch or a counter) and become has_arg in the generated
 * `struct option`.
 */
#define CONF_REQUIRED		0x0001	/* must be given, conf_check() enforces it */
#define CONF_REQUIRED_VALUE	0x0002	/* required_argument */
#define CONF_NO_VALUE		0x0004	/* no_argument */
#define CONF_MAYBE_VALUE	0x0008	/* optional_argument */
#define CONF_NEGATIVE		0x0010	/* CONF_CL_INC counts down; a bool defaults to false */
#define CONF_SINGLE		0x0020	/* may be given at most once */
#define CONF_NO_NEGATION	0x0040	/* a bool without the generated --no-<name> */
#define CONF_RUNTIME		0x0080	/* may be changed while the program runs */

#define CONF_VALUE_FLAGS (CONF_REQUIRED_VALUE | CONF_NO_VALUE | CONF_MAYBE_VALUE)

/*
 * A handler for CONF_CL_CALL. @data is the attribute's .ptr, @value the
 * argument (NULL where the option took none). Returns CONF_OK, or CONF_ERROR
 * after setting a message with conf_error().
 */
typedef int conf_handler(struct conf_ctx *ctx, const struct conf_attr *attr,
                         const char *value, void *data);

union conf_union {
	int value;			/* CONF_CL_SWITCH: what to store */
	int code;			/* CONF_CL_USER: what getopt_long() returns */
	const char * const *lookup;	/* CONF_T_LOOKUP: NULL-terminated names */
	conf_handler *call;		/* CONF_CL_CALL */
};

/*
 * A single attribute: one long option, one short option, or both.
 *
 * .arg and .help exist only under CONFIG_SECTION - see the file comment. The
 * fields below them are what getopt_long() and the value parser run on.
 */
struct conf_attr {
	const char *name;		/* long option name, NULL for short-only */
	void *ptr;			/* target variable, or handler data */
	union conf_union u;
#ifdef CONFIG_SECTION
	const char *arg;		/* metavar shown in --help, NULL if valueless */
	const char *help;		/* description, NULL hides the option */
#endif
	int letter;			/* short option, 0 for long-only */
	uint16_t flags;
	uint8_t cls;			/* enum conf_class */
	uint8_t type;			/* enum conf_type */
};

/*
 * A section: a namespace for <section>.<attribute> and a grouping in --help.
 * Under CONFIG_SECTION=n it degenerates to the attribute array it carries -
 * the attributes, and so the command line, are unchanged.
 */
struct conf_section {
#ifdef CONFIG_SECTION
	const char *name;		/* namespace; may be hierarchical, "net.ip" */
	const char *help;		/* headline in --help */
#endif
	const struct conf_attr *attr;	/* CONF_END-terminated */
};

/*
 * Declaration macros. Every attribute macro takes the same leading arguments:
 *
 *   @s   short option letter, 0 for none
 *   @l   long option name, NULL for none
 *   @t   the target variable itself (not its address); type-checked
 *   @fl  flags, 0 for the class default
 *   @a   metavar for --help ("ADDR", "N", ...), NULL if the option takes none
 *   @h   description, NULL to hide the option from --help
 *
 * @a and @h are compiled out entirely without CONFIG_SECTION, which is why
 * the arity of an option never depends on them.
 */
/*
 * Both expand to nothing without CONFIG_SECTION, which is why they come last
 * in an initializer: what is left then ends in the trailing comma C already
 * allows, and the declaration reads the same either way.
 */
#ifdef CONFIG_SECTION
#define CONF_NAME(n, h)		.name = n, .help = h
#define CONF_DESC(a, h)		.arg = a, .help = h
#else
#define CONF_NAME(n, h)
#define CONF_DESC(a, h)
#endif

/* Opens the attribute array of a section. */
#define CONF_ATTRS		.attr = (const struct conf_attr [])

/* Terminates it. */
#define CONF_END		{ .cls = CONF_CL_END }

/**
 * Declare a section named @n, titled @h in --help, holding the attributes
 * listed after it (no trailing comma, and no CONF_END - this adds it):
 *
 *   DEFINE_CONF_SECTION(net_conf, "net", "Networking",
 *     CONF_STRING('l', "listen", listen, 0, "ADDR", "Address to bind to"),
 *     CONF_INT(0, "backlog", backlog, 0, "N", "Listen backlog")
 *   );
 *
 * Writing the `struct conf_section` out by hand works too, as long as
 * CONF_NAME() comes last - it expands to nothing without CONFIG_SECTION, and
 * only in the last position does what remains still parse:
 *
 *   static const struct conf_section net_conf = {
 *     CONF_ATTRS { ..., CONF_END },
 *     CONF_NAME("net", "Networking")
 *   };
 */
#define DEFINE_CONF_SECTION(id, n, h, ...) \
	static const struct conf_section id = { \
		CONF_ATTRS { __VA_ARGS__, CONF_END }, \
		CONF_NAME(n, h) \
	}

/* Type-check @p against @type and yield it unchanged (pointer arithmetic
 * refuses to mix the two if they disagree). */
#define CONF_PTR(p, type)	((p) - (type)(p) + (p))

#define CONF_STATIC_(s, l, t, ty, ct, fl, a, h) \
	{ .letter = s, .name = l, .ptr = CONF_PTR(&(t), ty), .flags = fl, \
	  .cls = CONF_CL_STATIC, .type = ct, CONF_DESC(a, h) }

/** Boolean. @t is an `int`; --no-<name> is generated unless CONF_NO_NEGATION. */
#define CONF_BOOL(s, l, t, fl, a, h) \
	CONF_STATIC_(s, l, t, int *, CONF_T_BOOL, fl, a, h)
/** Signed integer, with an optional K/M/G/T suffix. @t is an `int`. */
#define CONF_INT(s, l, t, fl, a, h) \
	CONF_STATIC_(s, l, t, int *, CONF_T_INT, fl, a, h)
/** Unsigned integer. @t is an `unsigned int`. */
#define CONF_UINT(s, l, t, fl, a, h) \
	CONF_STATIC_(s, l, t, unsigned int *, CONF_T_UINT, fl, a, h)
/** 64-bit unsigned integer. @t is a `uint64_t`. */
#define CONF_U64(s, l, t, fl, a, h) \
	CONF_STATIC_(s, l, t, uint64_t *, CONF_T_U64, fl, a, h)
/** Floating point. @t is a `double`. */
#define CONF_DOUBLE(s, l, t, fl, a, h) \
	CONF_STATIC_(s, l, t, double *, CONF_T_DOUBLE, fl, a, h)
/** String. @t is a `char *`; the value is copied into the context pool. */
#define CONF_STRING(s, l, t, fl, a, h) \
	CONF_STATIC_(s, l, t, char **, CONF_T_STRING, fl, a, h)

/**
 * One name out of @tab, a NULL-terminated array of `const char *`. @t is an
 * `int` and receives the index of the name given.
 */
#define CONF_LOOKUP(s, l, t, tab, fl, a, h) \
	{ .letter = s, .name = l, .ptr = CONF_PTR(&(t), int *), \
	  .u.lookup = tab, .flags = fl, .cls = CONF_CL_STATIC, \
	  .type = CONF_T_LOOKUP, CONF_DESC(a, h) }

/** Stores the constant @v into the `int` @t. Takes no value. */
#define CONF_SWITCH(s, l, t, v, fl, a, h) \
	{ .letter = s, .name = l, .ptr = CONF_PTR(&(t), int *), .u.value = v, \
	  .flags = fl, .cls = CONF_CL_SWITCH, CONF_DESC(a, h) }

/** Counts occurrences into the `int` @t (down, with CONF_NEGATIVE). */
#define CONF_INC(s, l, t, fl, a, h) \
	{ .letter = s, .name = l, .ptr = CONF_PTR(&(t), int *), \
	  .flags = fl, .cls = CONF_CL_INC, CONF_DESC(a, h) }

/** Calls @fn with @data and the value. */
#define CONF_CALL(s, l, fn, data, fl, a, h) \
	{ .letter = s, .name = l, .ptr = data, .u.call = fn, \
	  .flags = fl, .cls = CONF_CL_CALL, CONF_DESC(a, h) }

/**
 * An option this module only puts in the tables and the help: conf_dispatch()
 * reports it as CONF_UNKNOWN and getopt_long() returns it, so the caller
 * handles it in its own switch exactly as with a hand-written table.
 *
 * What it is returned as follows getopt_long()'s own rule - a short option can
 * only ever come back as its letter, so with a letter given that letter is the
 * code for both spellings and @cd should be 0. Without one, @cd is the code,
 * and must lie outside [CONF_CODE_BASE, CONF_CODE_LAST]. conf_add() rejects a
 * declaration that leaves this ambiguous.
 */
/* @cd, not @code: a macro parameter spelled like the member it initialises
 * would be substituted into the designator too. */
#define CONF_USER(s, l, cd, fl, a, h) \
	{ .letter = s, .name = l, .u.code = cd, \
	  .flags = fl, .cls = CONF_CL_USER, CONF_DESC(a, h) }

/** A line of text in --help, between groups of options. */
#define CONF_HELP(text) \
	{ .cls = CONF_CL_HELP, CONF_DESC(NULL, text) }

/*
 * The private range conf_dispatch() decodes. Long-only attributes get a code
 * from it; a CONF_USER code must avoid it.
 */
#define CONF_CODE_BASE 0x20000000
#define CONF_CODE_LAST 0x3fffffff

/*** Ready-made options ***/

/** -h, --help: print the help to stdout and exit(0). */
#define CONF_HELP_OPTION \
	CONF_CALL('h', "help", conf_handle_help, NULL, CONF_NO_VALUE, NULL, \
	          "Show this help and exit")

#ifdef CONFIG_SECTION
/** -S, --set <sec.attr=value>: set one attribute by name. */
#define CONF_SET_OPTION \
	CONF_CALL('S', "set", conf_handle_set, NULL, CONF_REQUIRED_VALUE, \
	          "SEC.ATTR=VALUE", "Set a configuration attribute")
/** -C, --config <file>: load a configuration file. */
#define CONF_FILE_OPTION \
	CONF_CALL('C', "config", conf_handle_load, NULL, CONF_REQUIRED_VALUE, \
	          "FILE", "Load a configuration file")
/** --dumpconfig: write the configuration to stdout and exit(0). */
#define CONF_DUMP_OPTION \
	CONF_CALL(0, "dumpconfig", conf_handle_dump, NULL, CONF_NO_VALUE, \
	          NULL, "Dump the configuration and exit")
#else
/* No names to address, no file syntax to parse: the options are gone, and
 * with them their strings. An inert slot keeps the array shape. */
#define CONF_SET_OPTION		{ .cls = CONF_CL_HELP }
#define CONF_FILE_OPTION	{ .cls = CONF_CL_HELP }
#define CONF_DUMP_OPTION	{ .cls = CONF_CL_HELP }
#endif

/*** Context ***/

/**
 * Create a parsing context. Everything it allocates - the generated tables,
 * parsed strings, error messages - comes from @mm, hpc's allocator interface
 * (<mem/alloc.h>). Pass NULL for mm_libc().
 *
 * Any mm will do, including an arena or a pool: the context keeps its own list
 * of what it allocated, so conf_free() releases it whether or not the mm's
 * free() does anything.
 */
struct conf_ctx *conf_alloc(struct mm *mm);

/**
 * Release everything the context allocated. Strings that CONF_STRING
 * attributes were given came from here, so a program that outlives its context
 * must copy them first.
 */
void conf_free(struct conf_ctx *ctx);

/**
 * Register a section. Returns CONF_OK, or CONF_ERROR for a malformed
 * declaration (a duplicate short letter or long name, a CONF_USER code inside
 * the reserved range). Must precede the first conf_getopt()/conf_longopts().
 */
int conf_add(struct conf_ctx *ctx, const struct conf_section *sec);

/** Name used in usage and diagnostics. conf_getopt() takes it from argv[0]. */
void conf_progname(struct conf_ctx *ctx, const char *name);

/** The last error, valid until the next failing call. NULL if there was none. */
const char *conf_strerror(struct conf_ctx *ctx);

/** Set the error message; for use from a conf_handler. Always returns CONF_ERROR. */
int conf_error(struct conf_ctx *ctx, const char *fmt, ...);

/*** getopt_long ***/

/** The generated `struct option` array, zero-terminated. */
const struct option *conf_longopts(struct conf_ctx *ctx);

/** The generated short-option string, opening with ':'. */
const char *conf_shortopts(struct conf_ctx *ctx);

/**
 * Apply one option getopt_long() returned. @value is optarg (NULL where the
 * option took none). Returns CONF_OK, CONF_UNKNOWN for a code that is not
 * ours - a CONF_USER attribute or an option the caller added itself - or
 * CONF_ERROR.
 */
int conf_dispatch(struct conf_ctx *ctx, int code, const char *value);

/**
 * Run the whole getopt_long() loop. Returns the index of the first non-option
 * argument (optind), or CONF_ERROR after printing the reason to stderr.
 */
int conf_getopt(struct conf_ctx *ctx, int argc, char **argv);

/**
 * Verify that every CONF_REQUIRED attribute was given. conf_getopt() calls
 * this; a caller driving its own loop should.
 */
int conf_check(struct conf_ctx *ctx);

/** Whether the attribute reached by @name was given on the command line. */
int conf_seen(struct conf_ctx *ctx, const char *name);

/*** Changing the configuration of a running program ***/

/**
 * Declare that startup is over. From here on, every attribute that was not
 * declared CONF_RUNTIME is refused with an error naming it, whether the change
 * arrives through conf_set(), -S or a re-read configuration file.
 *
 * The distinction is not politeness. A limit that only bounds work - a timeout,
 * a ceiling a parser checks - can be read again on the next packet and takes
 * effect there. One that sized something at startup - a reservation, a table
 * whose width is a power of two - cannot be changed without building that thing
 * again, and a program that silently accepted the new number would report a
 * configuration it is not running. Declaring CONF_RUNTIME is a statement that
 * re-reading the value is enough.
 *
 * Pass 0 to go back to startup rules, which is what a program does around a
 * deliberate rebuild of whatever the boot-only values sized.
 */
void conf_running(struct conf_ctx *ctx, int running);

/**
 * How many times a value has changed, from zero. Whoever holds a copy of the
 * configuration - a worker with its own instance, a shared segment, a cached
 * derived value - compares this against what it last acted on, and re-reads
 * when the two differ. Cheap enough to check on a timer tick.
 */
unsigned int conf_generation(struct conf_ctx *ctx);

/*** Help ***/

/**
 * Write the usage text to @f: one group per section, with headlines and
 * descriptions. Without CONFIG_SECTION there is nothing to describe with, and
 * this prints the bare list of option names.
 */
void conf_help(struct conf_ctx *ctx, FILE *f);

int conf_handle_help(struct conf_ctx *ctx, const struct conf_attr *attr,
                     const char *value, void *data);

/*** Sections ***/

#ifdef CONFIG_SECTION

/**
 * Set the attribute named "<section>.<attribute>" - or just "<attribute>", if
 * that is unambiguous across the registered sections - to @value. Returns
 * CONF_OK, CONF_UNKNOWN if no such attribute, or CONF_ERROR if @value does not
 * parse.
 */
int conf_set(struct conf_ctx *ctx, const char *name, const char *value);

/** Same, given "<section>.<attribute>=<value>" in one string. This is -S. */
int conf_set_assign(struct conf_ctx *ctx, const char *assignment);

/**
 * Read a configuration file:
 *
 *   # a comment
 *   [net]                 # everything below is net.*
 *   listen 0.0.0.0        # or:  listen = 0.0.0.0
 *   backlog 128
 *   log.level "debug"     # a dotted name works anywhere
 *
 * Values run to the end of the line, with surrounding space removed; a
 * double-quoted value keeps its spaces and honours \" \\ \n \t \r \0.
 * Returns CONF_OK or CONF_ERROR.
 */
int conf_load(struct conf_ctx *ctx, const char *path);

/** Write every attribute as "<section>.<attribute> <value>", in that syntax. */
void conf_dump(struct conf_ctx *ctx, FILE *f);

int conf_handle_set(struct conf_ctx *ctx, const struct conf_attr *attr,
                    const char *value, void *data);
int conf_handle_load(struct conf_ctx *ctx, const struct conf_attr *attr,
                     const char *value, void *data);
int conf_handle_dump(struct conf_ctx *ctx, const struct conf_attr *attr,
                     const char *value, void *data);

#else /* !CONFIG_SECTION */

static inline int
conf_set(struct conf_ctx *ctx, const char *name, const char *value)
{
	(void)ctx; (void)name; (void)value;
	return CONF_UNKNOWN;
}

static inline int
conf_set_assign(struct conf_ctx *ctx, const char *assignment)
{
	(void)ctx; (void)assignment;
	return CONF_UNKNOWN;
}

static inline int
conf_load(struct conf_ctx *ctx, const char *path)
{
	(void)ctx; (void)path;
	return CONF_UNKNOWN;
}

static inline void
conf_dump(struct conf_ctx *ctx, FILE *f)
{
	(void)ctx; (void)f;
}

#endif /* CONFIG_SECTION */

/*** Value parsers ***/

/*
 * Each returns NULL on success, or a static message describing what was wrong
 * with the string. The integer parsers accept 0x/0 radix prefixes and a
 * trailing K, M, G or T (powers of 1024).
 */
const char *conf_parse_int(const char *str, int *ptr);
const char *conf_parse_uint(const char *str, unsigned int *ptr);
const char *conf_parse_u64(const char *str, uint64_t *ptr);
const char *conf_parse_double(const char *str, double *ptr);
const char *conf_parse_bool(const char *str, int *ptr);
const char *conf_parse_lookup(const char *str, const char * const *tab, int *ptr);

__END_DECLS

#endif
