/* syslog.h -- EmbLink override header (newlib ships none).
 *
 * Exists because git-compat-util.h does `#include <syslog.h>` UNCONDITIONALLY
 * -- there is no NO_SYSLOG knob -- so every one of git's ~500 translation units
 * needs the header to parse, while only git-daemon (which EmbLink doesn't
 * build) ever CALLS these.
 *
 * Deliberately declarations only, no definitions anywhere in our libc: a
 * program that actually calls syslog() gets an honest LINK error naming the
 * missing capability, not a silent no-op that eats its log lines. EmbLink has
 * no syslog daemon; if logging is ever wanted, it should be a real decision
 * (a kernel log object? a file?), not a stub that pretends. Same rule as
 * getentropy: never fake the capability under the real name. */
#ifndef _EMBK_SYSLOG_H
#define _EMBK_SYSLOG_H

/* Priorities (RFC 5424 numbering -- these values are load-bearing: callers
 * pack them with facility codes). */
#define LOG_EMERG   0
#define LOG_ALERT   1
#define LOG_CRIT    2
#define LOG_ERR     3
#define LOG_WARNING 4
#define LOG_NOTICE  5
#define LOG_INFO    6
#define LOG_DEBUG   7

/* Facilities (<<3 per convention). Only the ones a port plausibly names. */
#define LOG_KERN   (0 << 3)
#define LOG_USER   (1 << 3)
#define LOG_MAIL   (2 << 3)
#define LOG_DAEMON (3 << 3)
#define LOG_AUTH   (4 << 3)
#define LOG_LOCAL0 (16 << 3)
#define LOG_LOCAL1 (17 << 3)
#define LOG_LOCAL2 (18 << 3)
#define LOG_LOCAL3 (19 << 3)
#define LOG_LOCAL4 (20 << 3)
#define LOG_LOCAL5 (21 << 3)
#define LOG_LOCAL6 (22 << 3)
#define LOG_LOCAL7 (23 << 3)

/* openlog() options. Accepted in the API, meaningless without a logger. */
#define LOG_PID    0x01
#define LOG_CONS   0x02
#define LOG_NDELAY 0x08
#define LOG_NOWAIT 0x10
#define LOG_PERROR 0x20

#ifdef __cplusplus
extern "C" {
#endif

void openlog(const char *ident, int option, int facility);
void syslog(int priority, const char *format, ...);
void closelog(void);

#ifdef __cplusplus
}
#endif

#endif /* _EMBK_SYSLOG_H */
