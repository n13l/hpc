
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <ctype.h>

#include <hpc/compiler.h>
#include <hpc/log.h>
#include <hpc/list.h>
#include <mem/alloc.h>
#include <mem/stack.h>
#include <sys/time.h>

#include <arch/os/linux/io/str.h>

#include <unistd.h>
#include <sys/syscall.h>

#ifdef CONFIG_OS_LINUX_IO

#include <arch/os/linux/io/io.h>

#define log_sys_open(path, flags, mode)	_sys_open((path), (flags), (mode))
#define log_sys_write(fd, buf, len)	_sys_write((fd), (buf), (len))
#define log_sys_getpid()		((unsigned int)_sys_getpid())
#define log_sys_exit(status)		_sys_exit(status)
#define log_sys_err(ret)		((int)-(ret))
#define log_sys_stdout			1
#define log_sys_stderr			2

static inline unsigned int compat_gettid(void)
{
	return (unsigned int)_syscall0(SYS_gettid);
}

static inline void log_sys_now(struct timeval *tv)
{
	_syscall2(SYS_gettimeofday, tv, 0);
}

static inline void log_sys_die(const char *file, int err)
{
	char num[24];

	nolibc_say(log_sys_stderr, "file: ", file, ": ",
	           nolibc_utoa((unsigned long)err, num, sizeof(num)),
	           (const char *)0);
	log_sys_exit(1);
}

#else

#define log_sys_open(path, flags, mode)	open((path), (flags), (mode))
#define log_sys_write(fd, buf, len)	write((fd), (buf), (len))
#define log_sys_getpid()		((unsigned int)getpid())
#define log_sys_exit(status)		exit(status)
#define log_sys_err(ret)		errno
#define log_sys_stdout			fileno(stdout)
#define log_sys_stderr			fileno(stderr)

#ifdef SYS_gettid
static inline unsigned int compat_gettid(void)
{ 
	return (unsigned int) syscall(SYS_gettid);
}
#elif __APPLE__
#if TARGET_OS_IPHONE && TARGET_IPHONE_SIMULATOR
#elif TARGET_OS_IPHONE
#else
#define TARGET_OS_OSX 1
static inline unsigned int compat_gettid(void)
{
	return (unsigned int)pthread_self();
}
#endif
#else
#error "SYS_gettid unavailable on this system"
#endif

static inline void log_sys_now(struct timeval *tv)
{
	gettimeofday(tv, NULL);
}

static inline void log_sys_die(const char *file, int err)
{
	printf("file: %s:%d:%s\n", file, err, strerror(err));
	log_sys_exit(EXIT_FAILURE);
}

#endif


#ifndef PIPE_BUF
#define PIPE_BUF 512
#endif

#ifndef LOG_USER
#define LOG_USER        (1<<3)
#endif
#ifndef LOG_DAEMON
#define LOG_DAEMON      (3<<3)
#endif
#ifndef LOG_AUTH
#define LOG_AUTH        (4<<3)
#endif
#ifndef LOG_AUTHPRIV
#define LOG_AUTHPRIV    (10<<3)
#endif

static const char *type_names[] = {
	[LOG_ERROR]  = "error",
	[LOG_INFO]   = "info",
	[LOG_WARN]   = "warn",
	[LOG_DEBUG]  = "debug",
	[LOG_DEBUG1] = "debug1",
	[LOG_DEBUG2] = "debug2",
	[LOG_DEBUG3] = "debug3",
	[LOG_DEBUG4] = "debug4",
	[LOG_TRACE1] = "trace1",
	[LOG_TRACE2] = "trace2",
	[LOG_TRACE3] = "trace3",
	[LOG_TRACE4] = "trace4",
};

enum log_out {
	LOG_TYPE_SYSLOG   = 1,
	LOG_TYPE_STDOUT   = 2,
	LOG_TYPE_STDERR   = 3,
	LOG_TYPE_FILE     = 4,
};


static int log_caps = 0;
static int log_type = 0;

#ifndef CONFIG_VERBOSE
#define CONFIG_VERBOSE 0
#endif

void *log_userdata = NULL;
int log_verbose = CONFIG_VERBOSE;
int log_silent = 0;
int log_append = 0;
char progname[256] = {0};
int log_fd = -1;
static struct timeval start = {0};
static int started = 1;

custom_log_fn log_msg_handler = NULL;

void
log_open(const char *file)
{
	log_type = 0;
	if (!xstrcmp(file, "stdout"))
		log_type = LOG_TYPE_STDOUT;
	else if (!xstrcmp(file, "stderr"))
		log_type = LOG_TYPE_STDERR;

	switch (log_type) {
	case LOG_TYPE_STDOUT:
		log_fd = log_sys_stdout;
		break;
	case LOG_TYPE_STDERR:
		log_fd = log_sys_stderr;
		break;
	default: {
		long fd;

		if (log_append)
			fd = log_sys_open(file, O_APPEND | O_RDWR, 0644);
		else
			fd = log_sys_open(file, O_CREAT | O_RDWR | O_TRUNC,
			                  0644);

		if (fd >= 0) {
			log_fd = (int)fd;
			break;
		}
		log_fd = -1;
		log_sys_die(file, log_sys_err(fd));
	}
	}
}

void
log_set_handler(custom_log_fn fn)
{
	log_msg_handler = fn;
}

void
log_name(const char *name)
{
	snprintf(progname, sizeof(progname), "%s", name);
}

void
log_close(void)
{
}

void
log_setcaps(int caps)
{
	log_caps = caps;
}

int
log_getcaps(void)
{
	return log_caps;
}


static inline void
do_log_cap_timestamp(struct log_ctx *c)
{
	if (!(log_caps & LOG_CAP_TIMESTAMP))
		return;
	struct timeval now;
	log_sys_now(&now);
	if (started) {
		log_sys_now(&start);
		now = start;
		started = 0;
	}

	c->secs = now.tv_sec - start.tv_sec;
	c->usec = now.tv_usec;
}

static int __attribute__((format(printf, 4, 5)))
hdr_addf(char *msg, int sz, int cap, const char *fmt, ...)
{
	int rem = cap - sz, n;
	va_list ap;

	if (rem <= 0)
		return cap;
	va_start(ap, fmt);
	n = vsnprintf(msg + sz, (size_t)rem, fmt, ap);
	va_end(ap);
	if (n < 0)
		return sz;
	sz += n;
	return sz > cap ? cap : sz;
}

static int
do_log_hdr_parse(struct log_ctx *c, char *p, int cap)
{
	int sz = 0;
	do_log_cap_timestamp(c);
	if (log_caps & LOG_CAP_LEVEL)
		sz = hdr_addf(p, sz, cap, "%6s: ", type_names[c->type]);
	if (log_caps & LOG_CAP_TIMESTAMP)
		sz = hdr_addf(p, sz, cap, "%08u.%06u ", c->secs, c->usec);
	if (log_caps & LOG_CAP_PID)
		sz = hdr_addf(p, sz, cap, "%u ", log_sys_getpid());
	if (log_caps & LOG_CAP_TID)
		sz = hdr_addf(p, sz, cap, "%u ", compat_gettid());
	if (log_caps & LOG_CAP_NAME)
		sz = hdr_addf(p, sz, cap, "[%s] ", progname);
	if (log_caps & LOG_CAP_MODULE)
		sz = hdr_addf(p, sz, cap, "%s ", c->mod);
	if (log_caps & LOG_CAP_FN)
		sz = hdr_addf(p, sz, cap, "%s:%s:%d ", c->fn, c->file, c->line);
	return sz;
}


static void
log_flush(char *msg, int sz)
{
	if (sz < 0)
		sz = 0;
	else if (sz > PIPE_BUF - 2)
		sz = PIPE_BUF - 2;

	msg[sz++] = '\n';
	msg[sz] = '\0';

	if (log_msg_handler)
		log_msg_handler(msg, sz);
	else if (log_fd != -1) {
		if (log_sys_write(log_fd, msg, sz) < 0)
			; /* best-effort async-safe write; nothing to do */
	}
}

void
__log_asynch_safe_atomic_vprintf(struct log_ctx *c, const char *fmt, va_list a)
{
	char msg[PIPE_BUF];
	int sz = do_log_hdr_parse(c, msg, PIPE_BUF - 4);
	int rem = PIPE_BUF - sz - 2;
	va_list args;
	int n;

	va_copy(args, a);
	n = vsnprintf(msg + sz, (size_t)rem, fmt, args);
	va_end(args);
	if (n > 0)
		sz += (n < rem) ? n : rem - 1;

	log_flush(msg, sz);
}

void
__log_asynch_safe_atomic_printf(struct log_ctx *ctx, const char *fmt, ...)
{
	if (log_silent)
		return;

	va_list args;
	va_start(args, fmt);
	__log_asynch_safe_atomic_vprintf(ctx, fmt, args);
	va_end(args);
}

#define DUMP_WIDTH_LESS_INDENT(i) (16 -((i - (i > 6 ? 6:i) + 3) / 4))

static void
b16_format_row(char *buf, size_t bufsz, const char *indent,
               const u8 *s, int off, int width, int len)
{
	char tmp[20];

	xstrlcpy(buf, bufsz, indent);
	snprintf(tmp, sizeof(tmp), "%04x - ", off);
	xstrlcat(buf, bufsz, tmp);

	for (int j = 0; j < width; j++) {
		if (off + j >= len) {
			xstrlcat(buf, bufsz, "   ");
		} else {
			unsigned char ch = s[off + j];
			snprintf(tmp, sizeof(tmp), "%02x%c", ch,
			         j == 7 ? '-' : ' ');
			xstrlcat(buf, bufsz, tmp);
		}
	}

	xstrlcat(buf, bufsz, "  ");
	for (int j = 0; j < width; j++) {
		unsigned char ch;
		if (off + j >= len)
			break;
		ch = s[off + j];
		snprintf(tmp, sizeof(tmp), "%c",
		         (ch >= toascii(' ') && ch <= toascii('~')) ? ch : '.');
		xstrlcat(buf, bufsz, tmp);
	}
}

static void
b16_emit(struct log_ctx *c, const char *prefix, const char *buf)
{
	char msg[PIPE_BUF];
	int sz = do_log_hdr_parse(c, msg, PIPE_BUF - 4);
	int room = PIPE_BUF - 2 - sz;
	unsigned prefix_len, buf_len;
	char *p = msg + sz;

	if (room < 0)
		room = 0;

	prefix_len = prefix ? xstrlen(prefix) : 0;
	if (prefix_len > (unsigned)room)
		prefix_len = (unsigned)room;
	memcpy(p, prefix, prefix_len);
	p += prefix_len;
	sz += (int)prefix_len;
	room -= (int)prefix_len;

	buf_len = xstrlen(buf);
	if (buf_len > (unsigned)room)
		buf_len = (unsigned)room;
	memcpy(p, buf, buf_len);
	sz += (int)buf_len;

	log_flush(msg, sz);
}

static int
b16_indent(struct log_ctx *c, const char *prefix, int ind, const u8 *s, int len)
{
	char buf[289], str[129];
	int i, rows, dump_width;

	if (ind < 0)
		ind = 0;
	if (ind > 128)
		ind = 128;
	memset(str, ' ', ind);
	str[ind] = '\0';

	dump_width = DUMP_WIDTH_LESS_INDENT(ind);
	rows = (len + dump_width - 1) / dump_width;

	for (i = 0; i < rows; i++) {
		int off = i * dump_width;
		b16_format_row(buf, sizeof(buf), str, s, off, dump_width, len);
		b16_emit(c, prefix, buf);
	}
	return 0;
}

void
__log_asynch_safe_atomic_write_b16(struct log_ctx *ctx, const char *prefix,
                                   int indent, const u8 *buf, unsigned int size)
{
	if (log_silent)
		return;

	b16_indent(ctx, prefix, indent, buf, size);
}
