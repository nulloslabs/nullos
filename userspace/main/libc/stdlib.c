#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>

// Variables, structs etc. for functions
typedef struct block {
    size_t size;
    int free;
    struct block *next;
} block_t;

#define ALIGN 16
#define HDR sizeof(block_t)

static block_t *heap_head = NULL;

void *malloc(size_t size) {
    if (!size) return NULL;
    size = (size + ALIGN - 1) & ~(ALIGN - 1);

    block_t *b = heap_head;
    while (b) {
        if (b->free && b->size >= size) {
            b->free = 0;
            return (void *)(b + 1);
        }
        b = b->next;
    }

    b = sbrk(0);
    if (sbrk(HDR + size) == (void *)-1) return NULL;
    b->size = size;
    b->free = 0;
    b->next = NULL;

    if (!heap_head) {
        heap_head = b;
    } else {
        block_t *cur = heap_head;
        while (cur->next) cur = cur->next;
        cur->next = b;
    }

    return (void *)(b + 1);
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (!size) { free(ptr); return NULL; }

    block_t *b = (block_t *)ptr - 1;
    if (b->size >= size) return ptr;

    void *new = malloc(size);
    if (!new) return NULL;
    memcpy(new, ptr, b->size);
    free(ptr);
    return new;
}

void *calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *ptr = malloc(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

void free(void *ptr) {
    if (!ptr) return;
    block_t *b = (block_t *)ptr - 1;
    b->free = 1;
}

__attribute__((noreturn)) void _Exit(int status) {
    // _Exit() is just a _exit() wrapper (wtf POSIX)
    _exit(status);
    __builtin_unreachable();
}

__attribute__((noreturn)) void exit(int status) {
    fflush(NULL); // Flush file descriptors
    _Exit(status);
    __builtin_unreachable();
}

long strtol(const char *s, char **endptr, int base) {
    while (*s == ' ' || *s == '\t') s++;
    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;

    if (base == 0) {
        if (*s == '0' && (*(s+1) == 'x' || *(s+1) == 'X')) { base = 16; s += 2; }
        else if (*s == '0') { base = 8; s++; }
        else base = 10;
    } else if (base == 16 && *s == '0' && (*(s+1) == 'x' || *(s+1) == 'X')) {
        s += 2;
    }

    long result = 0;
    const char *start = s;
    while (*s) {
        int digit;
        if (*s >= '0' && *s <= '9') digit = *s - '0';
        else if (*s >= 'a' && *s <= 'z') digit = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') digit = *s - 'A' + 10;
        else break;
        if (digit >= base) break;
        if (sign == 1 && result > (LONG_MAX - digit) / base) { errno = ERANGE; if (endptr) *endptr = (char *)s; return LONG_MAX; }
        if (sign == -1 && result > (-(LONG_MIN + digit)) / base) { errno = ERANGE; if (endptr) *endptr = (char *)s; return LONG_MIN; }
        result = result * base + digit;
        s++;
    }

    if (endptr) *endptr = (char *)(s == start ? start : s);
    return sign * result;
}

unsigned long strtoul(const char *s, char **endptr, int base) {
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '+') s++;

    if (base == 0) {
        if (*s == '0' && (*(s+1) == 'x' || *(s+1) == 'X')) { base = 16; s += 2; }
        else if (*s == '0') { base = 8; s++; }
        else base = 10;
    } else if (base == 16 && *s == '0' && (*(s+1) == 'x' || *(s+1) == 'X')) {
        s += 2;
    }

    unsigned long result = 0;
    const char *start = s;
    while (*s) {
        int digit;
        if (*s >= '0' && *s <= '9') digit = *s - '0';
        else if (*s >= 'a' && *s <= 'z') digit = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') digit = *s - 'A' + 10;
        else break;
        if (digit >= base) break;
        if (result > (ULONG_MAX - digit) / base) { errno = ERANGE; if (endptr) *endptr = (char *)s; return ULONG_MAX; }
        result = result * base + digit;
        s++;
    }

    if (endptr) *endptr = (char *)(s == start ? start : s);
    return result;
}

long long strtoll(const char *s, char **endptr, int base) {
    while (*s == ' ' || *s == '\t') s++;
    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;

    if (base == 0) {
        if (*s == '0' && (*(s+1) == 'x' || *(s+1) == 'X')) { base = 16; s += 2; }
        else if (*s == '0') { base = 8; s++; }
        else base = 10;
    } else if (base == 16 && *s == '0' && (*(s+1) == 'x' || *(s+1) == 'X')) {
        s += 2;
    }

    long long result = 0;
    const char *start = s;
    while (*s) {
        int digit;
        if (*s >= '0' && *s <= '9') digit = *s - '0';
        else if (*s >= 'a' && *s <= 'z') digit = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') digit = *s - 'A' + 10;
        else break;
        if (digit >= base) break;
        if (sign == 1 && result > (LLONG_MAX - digit) / base) { errno = ERANGE; if (endptr) *endptr = (char *)s; return LLONG_MAX; }
        if (sign == -1 && result > (-(LLONG_MIN + digit)) / base) { errno = ERANGE; if (endptr) *endptr = (char *)s; return LLONG_MIN; }
        result = result * base + digit;
        s++;
    }

    if (endptr) *endptr = (char *)(s == start ? start : s);
    return sign * result;
}

unsigned long long strtoull(const char *s, char **endptr, int base) {
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '+') s++;

    if (base == 0) {
        if (*s == '0' && (*(s+1) == 'x' || *(s+1) == 'X')) { base = 16; s += 2; }
        else if (*s == '0') { base = 8; s++; }
        else base = 10;
    } else if (base == 16 && *s == '0' && (*(s+1) == 'x' || *(s+1) == 'X')) {
        s += 2;
    }

    unsigned long long result = 0;
    const char *start = s;
    while (*s) {
        int digit;
        if (*s >= '0' && *s <= '9') digit = *s - '0';
        else if (*s >= 'a' && *s <= 'z') digit = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') digit = *s - 'A' + 10;
        else break;
        if (digit >= base) break;
        if (result > (ULLONG_MAX - digit) / base) { errno = ERANGE; if (endptr) *endptr = (char *)s; return ULLONG_MAX; }
        result = result * base + digit;
        s++;
    }

    if (endptr) *endptr = (char *)(s == start ? start : s);
    return result;
}

double strtod(const char *s, char **endptr) {
    while (*s == ' ' || *s == '\t') s++;
    double sign = 1.0;
    if (*s == '-') { sign = -1.0; s++; }
    else if (*s == '+') s++;

    double result = 0.0;
    const char *start = s;

    while (*s >= '0' && *s <= '9')
        result = result * 10.0 + (*s++ - '0');

    if (*s == '.') {
        s++;
        double frac = 0.1;
        while (*s >= '0' && *s <= '9') {
            result += (*s++ - '0') * frac;
            frac *= 0.1;
        }
    }

    if (*s == 'e' || *s == 'E') {
        s++;
        int esign = 1;
        if (*s == '-') { esign = -1; s++; }
        else if (*s == '+') s++;
        int exp = 0;
        while (*s >= '0' && *s <= '9')
            exp = exp * 10 + (*s++ - '0');
        double scale = 1.0;
        for (int i = 0; i < exp; i++) scale *= 10.0;
        if (esign > 0) result *= scale;
        else result /= scale;
    }

    if (endptr) *endptr = (char *)(s == start ? start : s);
    return sign * result;
}

float strtof(const char *s, char **endptr) {
    return (float)strtod(s, endptr);
}

long double strtold(const char *s, char **endptr) {
    return (long double)strtod(s, endptr);
}

int atoi(const char *s) {
    int result = 0, sign = 1;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { sign = -1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') result = result * 10 + (*s++ - '0');
    return sign * result;
}

long atol(const char *s) {
    long result = 0; int sign = 1;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { sign = -1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') result = result * 10 + (*s++ - '0');
    return sign * result;
}

long long atoll(const char *s) {
    long long result = 0; int sign = 1;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { sign = -1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') result = result * 10 + (*s++ - '0');
    return sign * result;
}

double atof(const char *s) {
    double result = 0.0, sign = 1.0, frac = 0.1;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { sign = -1.0; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') result = result * 10.0 + (*s++ - '0');
    if (*s == '.') { s++; while (*s >= '0' && *s <= '9') { result += (*s++ - '0') * frac; frac *= 0.1; } }
    return sign * result;
}
