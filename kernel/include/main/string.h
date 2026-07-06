#pragma once

#include <freestanding/stddef.h>

void *memcpy(void *restrict dest, const void *restrict src, size_t n);
void *memmove(void *dest, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
int memcmp(const void *s1, const void *s2, size_t n);
void *memchr(const void *s, int c, size_t n);
void *memrchr(const void *s, int c, size_t n);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t maxlen);
char *strcpy(char *restrict dest, const char *restrict src);
char *strncpy(char *restrict dest, const char *restrict src, size_t n);
size_t strlcpy(char *restrict dst, const char *restrict src, size_t size);
char *strcat(char *restrict dest, const char *restrict src);
char *strncat(char *restrict dest, const char *restrict src, size_t n);
size_t strlcat(char *restrict dst, const char *restrict src, size_t size);
int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, size_t n);
char *strstr(const char *haystack, const char *needle);
char *strnstr(const char *haystack, const char *needle, size_t n);
char *strtok(char *restrict str, const char *restrict delim);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strdup(const char *s);
char *strndup(const char *s, size_t n);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char *strpbrk(const char *s, const char *accept);
char *strerror(int errnum);
