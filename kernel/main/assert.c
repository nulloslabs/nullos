#include <main/assert.h>
#include <main/panic.h>

__attribute__((noreturn)) void __assert_fail(const char *func, const char *expr) {
    dopanic(func, "assertion '%s' failed", expr);
    __builtin_unreachable();
}
