#pragma once

#include <freestanding/stddef.h>

char *index(const char *s, int c);
char *rindex(const char *s, int c);
int bcmp(const void *s1, const void *s2, size_t n);
void bcopy(const void *src, void *dest, size_t n);
void bzero(void *s, size_t n);
int ffs(int i);
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);
const char *strcasestr(const char *hay, const char *needle);
const char *strncasestr(const char *hay, const char *needle, size_t n);