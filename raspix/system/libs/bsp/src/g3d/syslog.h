/* Minimal <syslog.h> shim for the EwokOS userland port of the mesa compiler.
 *
 * EwokOS has no syslog(3). util/log.c pulls in <syslog.h> under DETECT_OS_POSIX
 * and wires an optional "syslog" logger (selected only via the MESA_LOG=syslog
 * env var, never by default). Providing the standard constants plus no-op inline
 * entry points lets log.c compile and link unchanged; the syslog sink simply
 * discards messages, while mesa's default stderr/file logger is unaffected.
 *
 * Lives in the platform bsp g3d dir (exported to the build as V3D_PORT_DIR),
 * which the v3dcompiler Makefile puts first on the include path, so this shadows
 * any (absent) system <syslog.h>. Part of the platform build config.
 */
#ifndef EWOKOS_PORT_SYSLOG_H
#define EWOKOS_PORT_SYSLOG_H

#include <stdarg.h>

/* Priorities (BSD/syslog.h ordering). */
#define LOG_EMERG   0
#define LOG_ALERT   1
#define LOG_CRIT    2
#define LOG_ERR     3
#define LOG_WARNING 4
#define LOG_NOTICE  5
#define LOG_INFO    6
#define LOG_DEBUG   7

/* openlog() option flags. */
#define LOG_PID     0x01
#define LOG_CONS    0x02
#define LOG_ODELAY  0x04
#define LOG_NDELAY  0x08
#define LOG_NOWAIT  0x10
#define LOG_PERROR  0x20

/* Facilities (only LOG_USER is referenced by mesa's log.c). */
#define LOG_KERN     (0 << 3)
#define LOG_USER     (1 << 3)
#define LOG_MAIL     (2 << 3)
#define LOG_DAEMON   (3 << 3)
#define LOG_LOCAL0   (16 << 3)

static inline void
openlog(const char *ident, int option, int facility)
{
   (void)ident; (void)option; (void)facility;
}

static inline void
closelog(void)
{
}

static inline void
syslog(int priority, const char *format, ...)
{
   (void)priority; (void)format;
}

static inline void
vsyslog(int priority, const char *format, va_list ap)
{
   (void)priority; (void)format; (void)ap;
}

static inline int
setlogmask(int mask)
{
   (void)mask;
   return 0xff;
}

#endif /* EWOKOS_PORT_SYSLOG_H */
