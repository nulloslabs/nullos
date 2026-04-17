#include <freestanding/stddef.h>
#include <freestanding/stdint.h>
#include <main/string.h>

void *memcpy(void *dest, const void *src, size_t n) {
    if (!dest || !src) return dest;
    if (n == 0) return dest;

    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dest;
}

void *memmove(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;

    if (d < s) {
        // Copy forward
        for (size_t i = 0; i < n; i++) {
            d[i] = s[i];
        }
    } else {
        // Copy backward to handle overlap
        for (size_t i = n; i > 0; i--) {
            d[i - 1] = s[i - 1];
        }
    }
    return dest;
}

void *memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t *)s;
    for (size_t i = 0; i < n; i++) {
        p[i] = (uint8_t)c;
    }
    return s;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *p1 = (const uint8_t *)s1;
    const uint8_t *p2 = (const uint8_t *)s2;
    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i]) return p1[i] - p2[i];
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

size_t strnlen(const char *s, size_t maxlen) {
    size_t len;
    
    for (len = 0; len < maxlen; len++) {
        if (s[len] == '\0') {
            break;
        }
    }
    
    return len;
}

char *strcpy(char *dest, const char *src) {
    size_t i = 0;
    while ((dest[i] = src[i]) != '\0') {
        i++;
    }
    return dest;
}

char *strncpy(char *dest, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    for ( ; i < n; i++) {
        dest[i] = '\0';
    }
    return dest;
}

int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s1[i] != s2[i] || s1[i] == '\0') {
            return (unsigned char)s1[i] - (unsigned char)s2[i];
        }
    }
    return 0;
}

char* strcat(char* dest, const char* src) {
    char* d = dest;
    // Find the end of dest
    while (*d) d++;
    // Copy src starting there
    while ((*d++ = *src++));
    return dest;
}

char* strncat(char* dest, const char* src, size_t n) {
    char* d = dest;

    // find end of dest
    while (*d != '\0') {
        d++;
    }

    // append src
    size_t i = 0;
    while (i < n && src[i] != '\0') {
        *d = src[i];
        d++;
        i++;
    }

    // null-terminate
    *d = '\0';

    return dest;
}

char* strstr(const char* haystack, const char* needle) {
    if (!*needle) return (char*)haystack;

    for (; *haystack; haystack++) {
        if (*haystack == *needle) {
            const char *h = haystack;
            const char *n = needle;
            while (*h && *n && *h == *n) {
                h++;
                n++;
            }
            if (!*n) return (char*)haystack;
        }
    }
    return NULL;
}

char* strnstr(const char* haystack, const char* needle, size_t n) {
    size_t needle_len = strlen(needle);
    if (needle_len == 0) return (char*)haystack;

    while (n >= needle_len && *haystack) {
        if (memcmp(haystack, needle, needle_len) == 0) {
            return (char*)haystack;
        }
        haystack++;
        n--;
    }
    return NULL;
}

char* strtok(char* str, const char* delim) {
    static char* last_token = NULL;
    char* token_start;

    if (str != NULL) {
        last_token = str;
    }

    if (last_token == NULL || *last_token == '\0') {
        return NULL;
    }

    while (*last_token != '\0' && strchr(delim, *last_token) != NULL) {
        last_token++;
    }

    if (*last_token == '\0') {
        return NULL;
    }

    token_start = last_token;

    while (*last_token != '\0' && strchr(delim, *last_token) == NULL) {
        last_token++;
    }

    if (*last_token != '\0') {
        *last_token = '\0';
        last_token++;
    }

    return token_start;
}

char* strchr(const char* s, int c) {
    while (*s != (char)c) {
        if (!*s++) {
            return NULL;
        }
    }
    return (char*)s;
}

char* strrchr(const char* s, int c) {
    char* last = NULL;
    do {
        if (*s == (char)c) {
            last = (char*)s;
        }
    } while (*s++);
    return last;
}