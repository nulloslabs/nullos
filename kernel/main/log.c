#include <freestanding/stdint.h>
#include <freestanding/stdarg.h>
#include <main/log.h>
#include <io/serial.h>
#include <io/terminal.h>

static inline const char* get_basename(const char* path) {
    const char* base = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/') {
            base = p + 1;
        }
    }
    return base;
}

void logat(const char* file, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    
    printf("%s: ", get_basename(file));
    vprintf(fmt, args);
    printf("\n");
    
    va_end(args);
}

void serial_logat(const char* file, uint16_t port, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    
    serial_printf(port, "%s: ", get_basename(file));
    serial_vprintf(port, fmt, args);
    serial_printf(port, "\n");
    
    va_end(args);
}
