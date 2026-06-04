#pragma once

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LOG_EMERG   0
#define LOG_ALERT   1
#define LOG_CRIT    2
#define LOG_ERR     3
#define LOG_WARNING 4
#define LOG_NOTICE  5
#define LOG_INFO    6
#define LOG_DEBUG   7

#define LOG_PRIMASK 0x07
#define LOG_PRI(p) ((p) & LOG_PRIMASK)
#define LOG_MAKEPRI(fac, pri) ((fac) | (pri))

#define LOG_KERN     0x000
#define LOG_USER     0x008
#define LOG_MAIL     0x010
#define LOG_DAEMON   0x018
#define LOG_AUTH     0x020
#define LOG_SYSLOG   0x028
#define LOG_LPR      0x030
#define LOG_NEWS     0x038
#define LOG_UUCP     0x040
#define LOG_CRON     0x048
#define LOG_AUTHPRIV 0x050
#define LOG_LOCAL0   0x080
#define LOG_LOCAL1   0x088
#define LOG_LOCAL2   0x090
#define LOG_LOCAL3   0x098
#define LOG_LOCAL4   0x0a0
#define LOG_LOCAL5   0x0a8
#define LOG_LOCAL6   0x0b0
#define LOG_LOCAL7   0x0b8

#define LOG_PID     0x01
#define LOG_CONS    0x02
#define LOG_NDELAY  0x08
#define LOG_ODELAY  0x04
#define LOG_NOWAIT  0x10

#define LOG_MASK(pri) (1 << (pri))
#define LOG_UPTO(pri) ((1 << ((pri) + 1)) - 1)

void openlog(const char *ident, int option, int facility);
void syslog(int priority, const char *format, ...);
void vsyslog(int priority, const char *format, va_list ap);
void closelog(void);
int setlogmask(int mask);

#ifdef __cplusplus
}
#endif
