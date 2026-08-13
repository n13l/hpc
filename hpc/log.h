
#ifndef __LOCKFREE_LOGGING_CAPABILITY_H__
#define __LOCKFREE_LOGGING_CAPABILITY_H__

#include <hpc/compiler.h>
#include <stdlib.h>
#include <stdio.h>
#include <hpc/log/atomic.h>

__BEGIN_DECLS

enum log_opt_e {
	LOG_OPT_SILENT    = 1,
	LOG_OPT_VERBOSITY = 2,
};

enum log_cap {
	LOG_CAP_LEVEL     = 1, 
	LOG_CAP_TIME      = 2, 
	LOG_CAP_TIMESTAMP = 4,
	LOG_CAP_PID       = 8,
	LOG_CAP_TID       = 16,
	LOG_CAP_USER      = 32,
	LOG_CAP_NAME      = 64,
	LOG_CAP_MODULE    = 128,
	LOG_CAP_FN        = 256,
};

enum log_msg {
	LOG_MSG_ERROR     = 1,
	LOG_MSG_WARN      = 2,
	LOG_MSG_INFO      = 3,
	LOG_MSG_DEBUG1    = 4,
	LOG_MSG_DEBUG2    = 5,
	LOG_MSG_DEBUG3    = 6,
	LOG_MSG_DEBUG4    = 7,
	LOG_MSG_TRACE1    = 4,
	LOG_MSG_TRACE2    = 5,
	LOG_MSG_TRACE3    = 6,
	LOG_MSG_TRACE4    = 7,
};


#define info(fmt, ...) \
  __log_atomic_printf(LOG_INFO, 0, fmt, ## __VA_ARGS__)


#define error(fmt, ...) \
  __log_atomic_printf(LOG_ERROR, 0, fmt, ## __VA_ARGS__)


#define warning(fmt, ...) \
  __log_atomic_printf(LOG_WARN, 0, fmt, ## __VA_ARGS__)


#define die(fmt, ...) \
({ \
  __log_atomic_printf(LOG_ERROR, 0, fmt, ## __VA_ARGS__); exit(1);\
})



#define debug1(fmt, ...) \
  __log_atomic_printf(LOG_DEBUG1, 1, fmt, ## __VA_ARGS__)


#define debug2(fmt, ...) \
  __log_atomic_printf(LOG_DEBUG2, 2, fmt, ## __VA_ARGS__)


#define debug3(fmt, ...) \
  __log_atomic_printf(LOG_DEBUG3, 3, fmt, ## __VA_ARGS__)


#define debug4(fmt, ...) \
  __log_atomic_printf(LOG_DEBUG4, 4, fmt, ## __VA_ARGS__)


#define debug1_buf(prefix, indent, buf, size) \
  __log_atomic_write_b16(LOG_DEBUG1, 1, prefix, indent, buf, size)


#define debug2_buf(prefix, indent, buf, size) \
  __log_atomic_write_b16(LOG_DEBUG2, 2, prefix, indent, buf, size)


#define debug3_buf(prefix, indent, buf, size) \
  __log_atomic_write_b16(LOG_DEBUG3, 3, prefix, indent, buf, size)


#define debug4_buf(prefix, indent, buf, size) \
  __log_atomic_write_b16(LOG_DEBUG4, 4, prefix, indent, buf, size)

#ifdef CONFIG_TRACE



#define trace1(fmt, ...) \
  __log_atomic_printf(LOG_TRACE1, 1, fmt, ## __VA_ARGS__)


#define trace2(fmt, ...) \
  __log_atomic_printf(LOG_TRACE2, 2, fmt, ## __VA_ARGS__)


#define trace3(fmt, ...) \
  __log_atomic_printf(LOG_TRACE3, 3, fmt, ## __VA_ARGS__)


#define trace4(fmt, ...) \
  __log_atomic_printf(LOG_TRACE4, 4, fmt, ## __VA_ARGS__)


#define trace1_buf(prefix, indent, buf, size) \
  __log_atomic_write_b16(LOG_TRACE1, 1, prefix, indent, buf, size)


#define trace2_buf(prefix, indent, buf, size) \
  __log_atomic_write_b16(LOG_TRACE2, 2, prefix, indent, buf, size)


#define trace3_buf(prefix, indent, buf, size) \
  __log_atomic_write_b16(LOG_TRACE3, 3, prefix, indent, buf, size)


#define trace4_buf(prefix, indent, buf, size) \
  __log_atomic_write_b16(LOG_TRACE4, 4, prefix, indent, buf, size)

#else

#define trace1(fmt, ...) do {} while (0)
#define trace2(fmt, ...) do {} while (0)
#define trace3(fmt, ...) do {} while (0)
#define trace4(fmt, ...) do {} while (0)

#define trace1_buf(fmt, ...) do {} while (0)
#define trace2_buf(fmt, ...) do {} while (0)
#define trace3_buf(fmt, ...) do {} while (0)
#define trace4_buf(fmt, ...) do {} while (0)

#endif


#define debug_attr1(name, ptr, bytes) \
	do { \
		debug1("  %s(%d):", name, bytes); \
		debug1_buf("", 4, ptr, bytes); \
	} while (0)

typedef void (*custom_log_fn)(char *msg, unsigned int len);


#ifdef CONFIG_LOGGING

void
log_name(const char *name);

void
log_open(const char *file);

void
log_close(void);

void
log_setcaps(int caps);

void
log_set_handler(custom_log_fn fn);

int
log_getcaps(void);

#else

#define log_name(name)        ((void)(name))
#define log_open(file)        ((void)(file))
#define log_close(...)        ((void)0)
#define log_setcaps(caps)     ((void)(caps))
#define log_set_handler(fn)   ((void)(fn))
#define log_getcaps(...)      (0)

#endif

#ifdef CONFIG_LOGGING
extern int log_verbose;
extern int log_silent;
extern int log_append;
#else
static int log_verbose _unused;
static int log_silent _unused;
static int log_append _unused;
#endif

__END_DECLS

#endif
