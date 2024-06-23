/*
 * Lock-free and asynch-safe logging capability
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
 * Logging code use stack, asynch-safe calls and atomic write() operation.
 *
 * POSIX.1 says that write(2)s of less than PIPE_BUF bytes must be atomic.
 * The precise semantics depend on whether the file descriptor is nonblocking.
 *
 * https://tools.ietf.org/html/rfc5424
 * Asynch-safe syslog()
 *
 */

#ifndef __LOCKFREE_LOGGING_ATOMIC_H__
#define __LOCKFREE_LOGGING_ATOMIC_H__

#include <hpc/compiler.h>
#include <stdarg.h>
#include <stdbool.h>
#include <errno.h>
#include <time.h>

__BEGIN_DECLS

/* POSIX.1 requires PIPE_BUF to be at least 512 bytes. */
#ifndef PIPE_BUF
#define PIPE_BUF 512
#endif

#ifndef LOG_ERROR
#define LOG_ERROR 3
#endif
#ifndef LOG_WARN
#define LOG_WARN 4
#endif
#ifndef LOG_INFO
#define LOG_INFO 6
#endif
#ifndef LOG_DEBUG
#define LOG_DEBUG 7
#endif
#ifndef LOG_DEBUG1
#define LOG_DEBUG1 8
#endif
#ifndef LOG_DEBUG2
#define LOG_DEBUG2 9
#endif
#ifndef LOG_DEBUG3
#define LOG_DEBUG3 10
#endif
#ifndef LOG_DEBUG4
#define LOG_DEBUG4 11
#endif

#ifndef LOG_TRACE1
#define LOG_TRACE1 12
#endif
#ifndef LOG_TRACE2
#define LOG_TRACE2 13
#endif
#ifndef LOG_TRACE3
#define LOG_TRACE3 14
#endif
#ifndef LOG_TRACE4
#define LOG_TRACE4 15
#endif

#ifndef LOG_MODULE
#define LOG_MODULE ""
#endif

struct log_ctx {
	const char *mod, *fn, *file;
	unsigned int secs, usec, line, type;
	void *user;
};

#define __log_init_ctx(level) \
({ \
	struct log_ctx ctx = (struct log_ctx) { \
		.mod  = LOG_MODULE, .fn = __PRETTY_FUNCTION__, \
		.file = __FILE__, .secs = 0, .usec = 0, \
		.line = __LINE__, .type = level \
	}; ctx; \
})

#ifndef CONFIG_LOG_SILENT

#define __log_atomic_printf(type, require, fmt, ...) \
({ \
  if (!log_silent && log_verbose >= require) { \
    struct log_ctx log_ctx = __log_init_ctx(type); \
    __log_asynch_safe_atomic_printf(&log_ctx, fmt, ## __VA_ARGS__); \
  } \
})

#define __log_atomic_write_b16(type, require, prefix, indent, buf, size) \
({ \
  if (!log_silent && log_verbose >= require) { \
    struct log_ctx log_ctx = __log_init_ctx(type); \
    __log_asynch_safe_atomic_write_b16(&log_ctx, prefix, indent, buf, size); \
  } \
})

#else

#define __log_atomic_printf(type, require, fmt, ...)
#define __log_atomic_write_b16(type, require, prefix, indent, buf, size)

#endif

void
__log_asynch_safe_atomic_write_b16(struct log_ctx *ctx, const char *prefix,
                                   int indent, const u8 *b, unsigned int size);
void
__log_asynch_safe_atomic_printf(struct log_ctx *, const char *fmt, ...)
                                __attribute__ ((__format__ (__printf__, 2, 3)));

__END_DECLS

#endif
