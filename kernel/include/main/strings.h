#pragma once

#include <freestanding/stddef.h>

int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);
const char *strcasestr(const char *hay, const char *needle);
const char *strncasestr(const char *hay, const char *needle, size_t n);
