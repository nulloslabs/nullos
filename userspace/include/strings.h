#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

char *index(const char *s, int c);
char *rindex(const char *s, int c);
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);
const char *strcasestr(const char *hay, const char *needle);
const char *strncasestr(const char *hay, const char *needle, size_t n);

#ifdef __cplusplus
}
#endif
