#pragma once

#define assert(expr) \
    do { \
        if (!(expr)) __assert_fail(__func__, #expr); \
    } while(0)

__attribute__((noreturn)) void __assert_fail(const char *func, const char *expr);
