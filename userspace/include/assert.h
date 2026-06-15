#pragma once

#include <stdio.h>
#include <stdlib.h>

extern const char *__progname;

#ifdef NDEBUG
    #define assert(expr) ((void)(expr))
#else
    #define assert(expr) \
        do { \
            if (!(expr)) { \
                fprintf(stderr, "%s: %s:%d: %s: Assertion '%s' failed.\n", __progname, __FILE__, __LINE__, __func__, #expr); \
                abort(); \
            } \
        } while (0)
#endif
