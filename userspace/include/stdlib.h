#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Defines
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

// Memory management
void *malloc(size_t size);
void *realloc(void *ptr, size_t size);
void *calloc(size_t nmemb, size_t size);
void free(void *ptr);

// Exit family
__attribute__((noreturn)) void _Exit(int status);
__attribute__((noreturn)) void exit(int status);

// Strto* family
long strtol(const char *s, char **endptr, int base);
unsigned long strtoul(const char *s, char **endptr, int base);
long long strtoll(const char *s, char **endptr, int base);
unsigned long long strtoull(const char *s, char **endptr, int base);
double strtod(const char *s, char **endptr);
float strtof(const char *s, char **endptr);
long double strtold(const char *s, char **endptr);

// Ato* family
int atoi(const char *s);
long atol(const char *s);
long long atoll(const char *s);
double atof(const char *s);

#ifdef __cplusplus
}
#endif

