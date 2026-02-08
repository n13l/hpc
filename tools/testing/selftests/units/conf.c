/*
 * Unit tests for <hpc/conf.h> - configuration sections and attributes, and the
 * getopt_long() tables generated from them.
 *
 * The unit builds and runs both ways. What CONFIG_SECTION gates is guarded
 * here with #ifdef rather than split into a second unit, because the point
 * being asserted is that the two builds agree: the same argv parses to the
 * same values either way, and only the naming, the descriptions and the
 * options that need them (-S, -C, --dumpconfig) are gone.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <hpc/compiler.h>
#include <hpc/conf.h>

static char *listen_addr;
static int backlog;
static int verbose;
static int daemonize;
static uint64_t bufsize;
static double ratio;
static int level;
static int mode;
static int once;
static int timeout;

static const char *const levels[] = { "error", "warn", "info", "debug", NULL };

DEFINE_CONF_SECTION(net_conf, "net", "Networking",
	CONF_STRING('l', "listen", listen_addr, 0, "ADDR", "Address to bind to"),
	CONF_INT(0, "backlog", backlog, 0, "N", "Listen backlog"),
	CONF_U64('b', "bufsize", bufsize, 0, "BYTES", "Socket buffer size"),
	CONF_DOUBLE(0, "ratio", ratio, 0, "F", "Load factor")
);

DEFINE_CONF_SECTION(log_conf, "log", "Logging",
	CONF_INC('v', "verbose", verbose, 0, NULL, "Increase verbosity"),
	CONF_LOOKUP(0, "level", level, levels, 0, "LEVEL", "One of the levels"),
	CONF_BOOL('d', "daemon", daemonize, 0, NULL, "Run in the background"),
	CONF_SWITCH(0, "fast", mode, 2, 0, NULL, "Fast mode"),
	CONF_INT(0, "single", once, CONF_SINGLE, "N", "At most once"),
	CONF_INT(0, "timeout", timeout, CONF_RUNTIME, "MS", "Changeable while running")
);

DEFINE_CONF_SECTION(gen_conf, "general", "General",
	CONF_USER('V', "version", 0, CONF_NO_VALUE, NULL, "Print the version"),
	CONF_SET_OPTION,
	CONF_FILE_OPTION,
	CONF_DUMP_OPTION,
	CONF_HELP_OPTION
);

static struct conf_ctx *
setup(void)
{
	struct conf_ctx *ctx = conf_alloc(NULL);

	assert_non_null(ctx);
	assert_int_equal(conf_add(ctx, &net_conf), CONF_OK);
	assert_int_equal(conf_add(ctx, &log_conf), CONF_OK);
	assert_int_equal(conf_add(ctx, &gen_conf), CONF_OK);

	listen_addr = NULL; backlog = 128; verbose = 0; daemonize = 0;
	bufsize = 0; ratio = 0; level = 0; mode = 0; once = 0;
	timeout = 500;
	return ctx;
}

/* conf_getopt() over a NULL-terminated argv, argc counted for the caller. */
static int
run(struct conf_ctx *ctx, char **argv)
{
	int argc = 0;

	while (argv[argc])
		argc++;
	return conf_getopt(ctx, argc, argv);
}

/* ---- values, in every spelling getopt_long offers ----------------------- */

static void
test_parse_values(void **state)
{
	(void)state;
	struct conf_ctx *ctx = setup();
	char *argv[] = { "t", "--listen", "10.0.0.1", "-b", "4K", "-vvv",
	                 "--backlog=512", "--level", "debug", "-d",
	                 "--ratio", "0.5", "cmd", "arg", NULL };

	assert_int_equal(run(ctx, argv), 12);	/* index of "cmd" */
	assert_string_equal(listen_addr, "10.0.0.1");
	assert_int_equal(backlog, 512);
	assert_int_equal((int)bufsize, 4096);	/* the K suffix is 1024 */
	assert_int_equal(verbose, 3);		/* -vvv counts */
	assert_int_equal(level, 3);		/* "debug" is index 3 */
	assert_int_equal(daemonize, 1);
	assert_true(ratio == 0.5);
	conf_free(ctx);
}

/* A parsed string outlives argv: it was copied into the context. */
static void
test_string_is_owned(void **state)
{
	(void)state;
	struct conf_ctx *ctx = setup();
	char arg[] = "10.0.0.1";
	char *argv[] = { "t", "--listen", arg, NULL };

	assert_true(run(ctx, argv) > 0);
	memset(arg, 'x', sizeof(arg) - 1);
	assert_string_equal(listen_addr, "10.0.0.1");
	conf_free(ctx);
}

/* ---- booleans and their generated --no- twin ---------------------------- */

static void
test_bool_forms(void **state)
{
	(void)state;
	struct conf_ctx *ctx;
	char *bare[] = { "t", "--daemon", NULL };
	char *value[] = { "t", "--daemon=no", NULL };
	char *negated[] = { "t", "--daemon=yes", "--no-daemon", NULL };
	char *shortopt[] = { "t", "-d", NULL };

	ctx = setup();
	assert_true(run(ctx, bare) > 0);
	assert_int_equal(daemonize, 1);
	conf_free(ctx);

	ctx = setup();
	assert_true(run(ctx, value) > 0);
	assert_int_equal(daemonize, 0);
	conf_free(ctx);

	ctx = setup();
	assert_true(run(ctx, negated) > 0);
	assert_int_equal(daemonize, 0);
	conf_free(ctx);

	ctx = setup();
	assert_true(run(ctx, shortopt) > 0);
	assert_int_equal(daemonize, 1);
	conf_free(ctx);
}

static void
test_switch(void **state)
{
	(void)state;
	struct conf_ctx *ctx = setup();
	char *argv[] = { "t", "--fast", NULL };

	assert_true(run(ctx, argv) > 0);
	assert_int_equal(mode, 2);
	conf_free(ctx);
}

/* ---- what the parser refuses -------------------------------------------- */

static void
test_errors(void **state)
{
	(void)state;
	struct conf_ctx *ctx;
	char *bad_value[] = { "t", "--backlog", "hello", NULL };
	char *unknown[] = { "t", "--nonesuch", NULL };
	char *no_value[] = { "t", "--listen", NULL };
	char *twice[] = { "t", "--single", "1", "--single", "2", NULL };

	ctx = setup();
	assert_int_equal(run(ctx, bad_value), CONF_ERROR);
	assert_non_null(strstr(conf_strerror(ctx), "--backlog"));
	conf_free(ctx);

	ctx = setup();
	assert_int_equal(run(ctx, unknown), CONF_ERROR);
	assert_non_null(strstr(conf_strerror(ctx), "--nonesuch"));
	conf_free(ctx);

	/* Named as it was typed, not as the short letter it shares a code
	 * with. */
	ctx = setup();
	assert_int_equal(run(ctx, no_value), CONF_ERROR);
	assert_non_null(strstr(conf_strerror(ctx), "--listen"));
	conf_free(ctx);

	ctx = setup();
	assert_int_equal(run(ctx, twice), CONF_ERROR);
	conf_free(ctx);
}

/* A declaration that cannot produce a working table is rejected at
 * registration, so the tables themselves can never half-build. */
static int decl_a, decl_b;

/* A section is declared at file scope: its attribute array is a compound
 * literal, which is only a constant initializer outside a function. */
DEFINE_CONF_SECTION(dup_letter, "x", "X",
	CONF_INT('n', "one", decl_a, 0, "N", "one"),
	CONF_INT('n', "two", decl_b, 0, "N", "two")
);
DEFINE_CONF_SECTION(dup_name, "y", "Y",
	CONF_INT(0, "same", decl_a, 0, "N", "one"),
	CONF_INT(0, "same", decl_b, 0, "N", "two")
);
DEFINE_CONF_SECTION(neg_clash, "z", "Z",
	CONF_BOOL(0, "flag", decl_a, 0, NULL, "flag"),
	CONF_INT(0, "no-flag", decl_b, 0, "N", "collides with --no-flag")
);
DEFINE_CONF_SECTION(reserved, "w", "W",
	CONF_USER(0, "user", CONF_CODE_BASE + 1, CONF_NO_VALUE, NULL, "u")
);

static void
test_bad_declarations(void **state)
{
	(void)state;
	struct conf_ctx *ctx = conf_alloc(NULL);

	assert_int_equal(conf_add(ctx, &dup_letter), CONF_ERROR);
	assert_int_equal(conf_add(ctx, &dup_name), CONF_ERROR);
	assert_int_equal(conf_add(ctx, &neg_clash), CONF_ERROR);
	assert_int_equal(conf_add(ctx, &reserved), CONF_ERROR);

	/* A rejected section leaves nothing of itself behind. */
	assert_int_equal(conf_add(ctx, &net_conf), CONF_OK);
	conf_free(ctx);
}

/* ---- the caller keeps its own loop -------------------------------------- */

static void
test_caller_driven_loop(void **state)
{
	(void)state;
	struct conf_ctx *ctx = setup();
	char *argv[] = { "t", "-V", "--listen", "9.9.9.9", NULL };
	int c, seen_version = 0;

	optind = 0;
	opterr = 0;
	while ((c = getopt_long(4, argv, conf_shortopts(ctx),
	                        conf_longopts(ctx), NULL)) != -1) {
		if (conf_dispatch(ctx, c, optarg) != CONF_UNKNOWN)
			continue;
		/* CONF_USER with a short letter comes back as that letter,
		 * exactly as a hand-written table would return it. */
		assert_int_equal(c, 'V');
		seen_version = 1;
	}

	assert_int_equal(seen_version, 1);
	assert_string_equal(listen_addr, "9.9.9.9");
	conf_free(ctx);
}

/* The generated table is the one you would have written by hand. */
static void
test_generated_tables(void **state)
{
	(void)state;
	struct conf_ctx *ctx = setup();
	const struct option *lo = conf_longopts(ctx);
	const char *so = conf_shortopts(ctx);
	int found_listen = 0, found_no_daemon = 0, i;

	assert_non_null(lo);
	assert_int_equal(so[0], ':');	/* so a missing argument is its own case */
	assert_non_null(strchr(so, 'l'));
	assert_non_null(strstr(so, "l:"));	/* --listen takes a value */
	assert_non_null(strchr(so, 'v'));

	for (i = 0; lo[i].name; i++) {
		if (!strcmp(lo[i].name, "listen")) {
			found_listen = 1;
			assert_int_equal(lo[i].has_arg, required_argument);
			assert_int_equal(lo[i].val, 'l');
			assert_null(lo[i].flag);
		}
		if (!strcmp(lo[i].name, "no-daemon")) {
			found_no_daemon = 1;
			assert_int_equal(lo[i].has_arg, no_argument);
		}
	}

	assert_int_equal(found_listen, 1);
	assert_int_equal(found_no_daemon, 1);
	assert_null(lo[i].name);	/* zero-terminated */
	conf_free(ctx);
}

/* ---- the value parsers, on their own ------------------------------------ */

static void
test_parsers(void **state)
{
	(void)state;
	int i = 0;
	unsigned int u = 0;
	uint64_t q = 0;
	double d = 0;

	assert_null(conf_parse_int("-17", &i));
	assert_int_equal(i, -17);
	assert_null(conf_parse_int("0x10", &i));
	assert_int_equal(i, 16);
	assert_null(conf_parse_int("2K", &i));
	assert_int_equal(i, 2048);
	assert_non_null(conf_parse_int("", &i));
	assert_non_null(conf_parse_int("12x", &i));
	assert_non_null(conf_parse_int("99999999999999", &i));

	assert_null(conf_parse_uint("7M", &u));
	assert_int_equal(u, 7u * 1024 * 1024);
	assert_non_null(conf_parse_uint("-1", &u));

	assert_null(conf_parse_u64("4G", &q));
	assert_true(q == 4ull * 1024 * 1024 * 1024);

	assert_null(conf_parse_double("2.5", &d));
	assert_true(d == 2.5);
	assert_non_null(conf_parse_double("nope", &d));

	assert_null(conf_parse_bool("YES", &i));
	assert_int_equal(i, 1);
	assert_null(conf_parse_bool("off", &i));
	assert_int_equal(i, 0);
	assert_non_null(conf_parse_bool("maybe", &i));

	assert_null(conf_parse_lookup("Info", levels, &i));
	assert_int_equal(i, 2);
	assert_non_null(conf_parse_lookup("trace", levels, &i));
}

/* ---- changing the configuration of a running program -------------------- */

/* Every value that moves is counted, so a holder of a copy has one thing to
 * compare; a failed change is not a change. */
static void
test_generation(void **state)
{
	(void)state;
	struct conf_ctx *ctx = setup();
	char *argv[] = { "t", "--backlog", "1", "-vv", NULL };
	unsigned int gen;

	assert_int_equal(conf_generation(ctx), 0);
	assert_true(run(ctx, argv) > 0);

	/* --backlog and -v twice */
	gen = conf_generation(ctx);
	assert_int_equal(gen, 3);

	/* A change that failed is not a change. */
	char *bad[] = { "t", "--backlog", "hello", NULL };
	assert_int_equal(run(ctx, bad), CONF_ERROR);
	assert_int_equal(conf_generation(ctx), gen);
	conf_free(ctx);
}

/* ---- sections: the half CONFIG_SECTION decides ------------------------- */

#ifdef CONFIG_SECTION

static void
test_set_by_name(void **state)
{
	(void)state;
	struct conf_ctx *ctx = setup();

	assert_int_equal(conf_set(ctx, "net.listen", "1.2.3.4"), CONF_OK);
	assert_string_equal(listen_addr, "1.2.3.4");
	assert_int_equal(conf_set(ctx, "net.backlog", "2K"), CONF_OK);
	assert_int_equal(backlog, 2048);

	/* A bare name works where only one section has it. */
	assert_int_equal(conf_set(ctx, "level", "warn"), CONF_OK);
	assert_int_equal(level, 1);

	assert_int_equal(conf_set(ctx, "net.nope", "1"), CONF_UNKNOWN);
	assert_int_equal(conf_set(ctx, "nope", "1"), CONF_UNKNOWN);
	assert_int_equal(conf_set(ctx, "net.backlog", "hello"), CONF_ERROR);

	/* A value the parser would have to invent is refused, not parsed. */
	assert_int_equal(conf_set(ctx, "net.backlog", NULL), CONF_ERROR);
	assert_int_equal(backlog, 2048);
	conf_free(ctx);
}

static void
test_set_option(void **state)
{
	(void)state;
	struct conf_ctx *ctx = setup();
	char *argv[] = { "t", "-Snet.listen=1.2.3.4", "-S", "log.level=warn",
	                 NULL };

	assert_true(run(ctx, argv) > 0);
	assert_string_equal(listen_addr, "1.2.3.4");
	assert_int_equal(level, 1);
	conf_free(ctx);
}

/* Write @body to a scratch file, naming it into @path. */
static void
write_cf(char *path, size_t size, const char *name, const char *body)
{
	FILE *f;

	snprintf(path, size, "/tmp/hpc-conf-test-%d-%s", (int)getpid(), name);
	f = fopen(path, "w");
	assert_non_null(f);
	fputs(body, f);
	fclose(f);
}

/* After conf_running(), only what was declared CONF_RUNTIME may move. */
static void
test_running_gate(void **state)
{
	(void)state;
	struct conf_ctx *ctx = setup();

	conf_running(ctx, 1);

	assert_int_equal(conf_set(ctx, "log.timeout", "900"), CONF_OK);
	assert_int_equal(timeout, 900);

	assert_int_equal(conf_set(ctx, "net.backlog", "512"), CONF_ERROR);
	assert_non_null(strstr(conf_strerror(ctx), "--backlog"));
	assert_non_null(strstr(conf_strerror(ctx), "running"));
	assert_int_equal(backlog, 128);		/* untouched */

	/* and back, for a program that is rebuilding what the boot-only
	 * values sized */
	conf_running(ctx, 0);
	assert_int_equal(conf_set(ctx, "net.backlog", "512"), CONF_OK);
	assert_int_equal(backlog, 512);
	conf_free(ctx);
}

/* The gate is in the one place every route to a value goes through, so a
 * config file re-read is refused the same way -S is. */
static void
test_running_gate_applies_to_files(void **state)
{
	(void)state;
	struct conf_ctx *ctx = setup();
	char path[256];

	write_cf(path, sizeof(path), "running",
	         "[log]\ntimeout 750\n[net]\nbacklog 999\n");

	conf_running(ctx, 1);
	assert_int_equal(conf_load(ctx, path), CONF_ERROR);
	assert_int_equal(timeout, 750);		/* the runtime one took */
	assert_int_equal(backlog, 128);		/* the boot-only one did not */
	unlink(path);
	conf_free(ctx);
}

static void
test_config_file(void **state)
{
	(void)state;
	struct conf_ctx *ctx = setup();
	char path[256];

	write_cf(path, sizeof(path), "good",
		"# a comment\n"
		"[net]\n"
		"listen = 172.16.0.1   # trailing comment\n"
		"backlog 1K\n"
		"\n"
		"[log]\n"
		"level \"info\"\n"
		"daemon yes\n"
		"net.ratio 2.5\n");	/* a dotted key leaves the section */

	assert_int_equal(conf_load(ctx, path), CONF_OK);
	assert_string_equal(listen_addr, "172.16.0.1");
	assert_int_equal(backlog, 1024);
	assert_int_equal(level, 2);
	assert_int_equal(daemonize, 1);
	assert_true(ratio == 2.5);
	unlink(path);
	conf_free(ctx);
}

static void
test_config_file_errors(void **state)
{
	(void)state;
	struct conf_ctx *ctx = setup();
	char bad_value[256], unknown[256], unterminated[256];

	write_cf(bad_value, sizeof(bad_value), "bad", "[net]\nbacklog nope\n");
	write_cf(unknown, sizeof(unknown), "unknown", "[net]\nnosuchthing 1\n");
	write_cf(unterminated, sizeof(unterminated), "unterm",
	         "[net\nbacklog 1\n");

	assert_int_equal(conf_load(ctx, bad_value), CONF_ERROR);
	/* the message says where, not only what */
	assert_non_null(strstr(conf_strerror(ctx), ":2:"));

	assert_int_equal(conf_load(ctx, unknown), CONF_ERROR);
	assert_non_null(strstr(conf_strerror(ctx), "nosuchthing"));

	assert_int_equal(conf_load(ctx, unterminated), CONF_ERROR);

	assert_int_equal(conf_load(ctx, "/nonexistent/hpc-conf-test"),
	                 CONF_ERROR);

	unlink(bad_value);
	unlink(unknown);
	unlink(unterminated);
	conf_free(ctx);
}

/* What conf_dump() writes, conf_load() reads back to the same values. */
static void
test_dump_round_trip(void **state)
{
	(void)state;
	struct conf_ctx *ctx = setup();
	char path[256];
	FILE *f;

	assert_int_equal(conf_set(ctx, "net.listen", "quoted \"value\""),
	                 CONF_OK);
	assert_int_equal(conf_set(ctx, "net.backlog", "77"), CONF_OK);
	assert_int_equal(conf_set(ctx, "log.level", "debug"), CONF_OK);
	assert_int_equal(conf_set(ctx, "log.daemon", "yes"), CONF_OK);

	snprintf(path, sizeof(path), "/tmp/hpc-conf-test-%d-dump",
	         (int)getpid());
	f = fopen(path, "w");
	assert_non_null(f);
	conf_dump(ctx, f);
	fclose(f);

	listen_addr = NULL; backlog = 0; level = 0; daemonize = 0;

	assert_int_equal(conf_load(ctx, path), CONF_OK);
	assert_string_equal(listen_addr, "quoted \"value\"");
	assert_int_equal(backlog, 77);
	assert_int_equal(level, 3);
	assert_int_equal(daemonize, 1);

	unlink(path);
	conf_free(ctx);
}

/* --help groups by section and carries the descriptions. */
static void
test_help_has_sections(void **state)
{
	(void)state;
	struct conf_ctx *ctx = setup();
	char buf[4096];
	FILE *f = fmemopen(buf, sizeof(buf), "w");

	assert_non_null(f);
	conf_progname(ctx, "t");
	conf_help(ctx, f);
	fclose(f);

	assert_non_null(strstr(buf, "usage: t [options]"));
	assert_non_null(strstr(buf, "Networking"));
	assert_non_null(strstr(buf, "Address to bind to"));
	assert_non_null(strstr(buf, "-l, --listen=ADDR"));
	conf_free(ctx);
}

#else /* !CONFIG_SECTION */

/* The section entry points are stubs, and say so rather than pretending. */
static void
test_section_api_is_stubbed(void **state)
{
	(void)state;
	struct conf_ctx *ctx = setup();

	assert_int_equal(conf_set(ctx, "net.listen", "1.2.3.4"), CONF_UNKNOWN);
	assert_null(listen_addr);
	assert_int_equal(conf_load(ctx, "/dev/null"), CONF_UNKNOWN);
	conf_dump(ctx, stderr);		/* a no-op, not a crash */
	conf_free(ctx);
}

/* -S, -C and --dumpconfig are not merely inert - they are not options. */
static void
test_section_options_are_gone(void **state)
{
	(void)state;
	struct conf_ctx *ctx;
	char *set[] = { "t", "-Snet.listen=1.2.3.4", NULL };
	char *dump[] = { "t", "--dumpconfig", NULL };

	ctx = setup();
	assert_int_equal(run(ctx, set), CONF_ERROR);
	conf_free(ctx);

	ctx = setup();
	assert_int_equal(run(ctx, dump), CONF_ERROR);
	conf_free(ctx);
}

/* --help still lists the options; there is nothing left to describe them. */
static void
test_help_is_bare(void **state)
{
	(void)state;
	struct conf_ctx *ctx = setup();
	char buf[4096];
	FILE *f = fmemopen(buf, sizeof(buf), "w");

	assert_non_null(f);
	conf_progname(ctx, "t");
	conf_help(ctx, f);
	fclose(f);

	assert_non_null(strstr(buf, "usage: t [options]"));
	assert_non_null(strstr(buf, "--listen"));
	assert_null(strstr(buf, "Address to bind to"));
	assert_null(strstr(buf, "Networking"));
	conf_free(ctx);
}

#endif /* CONFIG_SECTION */

int
main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_parse_values),
		cmocka_unit_test(test_string_is_owned),
		cmocka_unit_test(test_bool_forms),
		cmocka_unit_test(test_switch),
		cmocka_unit_test(test_errors),
		cmocka_unit_test(test_bad_declarations),
		cmocka_unit_test(test_caller_driven_loop),
		cmocka_unit_test(test_generated_tables),
		cmocka_unit_test(test_parsers),
		cmocka_unit_test(test_generation),
#ifdef CONFIG_SECTION
		cmocka_unit_test(test_running_gate),
		cmocka_unit_test(test_running_gate_applies_to_files),
		cmocka_unit_test(test_set_by_name),
		cmocka_unit_test(test_set_option),
		cmocka_unit_test(test_config_file),
		cmocka_unit_test(test_config_file_errors),
		cmocka_unit_test(test_dump_round_trip),
		cmocka_unit_test(test_help_has_sections),
#else
		cmocka_unit_test(test_section_api_is_stubbed),
		cmocka_unit_test(test_section_options_are_gone),
		cmocka_unit_test(test_help_is_bare),
#endif
	};
	return cmocka_run_group_tests_name("conf", tests, NULL, NULL);
}
