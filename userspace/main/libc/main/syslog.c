#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

static const char *log_ident;
static int log_option;
static int log_facility = LOG_USER;
static int log_mask = 0xff;

void openlog(const char *ident, int option, int facility) {
    log_ident = ident;
    log_option = option;
    if (facility) log_facility = facility;
}

void closelog(void) {
    log_ident = NULL;
    log_option = 0;
    log_facility = LOG_USER;
}

int setlogmask(int mask) {
    int old = log_mask;
    if (mask) log_mask = mask;
    return old;
}

void vsyslog(int priority, const char *format, va_list ap) {
    char msg[1024];
    char line[1200];
    int pri = LOG_PRI(priority);

    if (!(log_mask & LOG_MASK(pri))) return;
    if (!(priority & ~LOG_PRIMASK)) priority |= log_facility;

    vsnprintf(msg, sizeof(msg), format, ap);
    if (log_ident) {
        if (log_option & LOG_PID)
            snprintf(line, sizeof(line), "<%d>%s[%d]: %s\n", priority, log_ident, getpid(), msg);
        else
            snprintf(line, sizeof(line), "<%d>%s: %s\n", priority, log_ident, msg);
    } else { snprintf(line, sizeof(line), "<%d>%s\n", priority, msg); }
    write(2, line, strlen(line));
}

void syslog(int priority, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    vsyslog(priority, format, ap);
    va_end(ap);
}