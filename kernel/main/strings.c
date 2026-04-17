#include <freestanding/stddef.h>
#include <freestanding/ctype.h>
#include <main/strings.h>
#include <main/string.h>

int strcasecmp(const char *a, const char *b) {
    while (*a && *b) {
        if (toupper(*a) != toupper(*b)) return toupper(*a) - toupper(*b);
        a++; b++;
    }
    return toupper(*a) - toupper(*b);
}

int strncasecmp(const char *a, const char *b, size_t n) {
    while (n--) {
        if (toupper(*a) != toupper(*b)) return toupper(*a) - toupper(*b);
        if (*a == '\0') return 0;
        a++; b++;
    }
    return 0;
}

const char *strcasestr(const char *hay, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0) return hay;
    while (*hay) {
        if (strncasecmp(hay, needle, nlen) == 0) return hay;
        hay++;
    }
    return NULL;
}

const char *strncasestr(const char *hay, const char *needle, size_t n) {
    size_t nlen = strlen(needle);
    if (nlen == 0) return hay;
    while (*hay && n >= nlen) {
        if (strncasecmp(hay, needle, nlen) == 0) return hay;
        hay++; n--;
    }
    return NULL;
}