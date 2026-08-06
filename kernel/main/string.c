#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <main/string.h>
#include <mm/mm.h>

void *memcpy(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;

    // Align destination to 8 bytes for word-sized copies
    while (n > 0 && ((uintptr_t)d & 7)) {
        *d++ = *s++;
        n--;
    }

    // Copy 64 bits at a time
    uint64_t *d64 = (uint64_t *)d;
    const uint64_t *s64 = (const uint64_t *)s;
    while (n >= 8) {
        *d64++ = *s64++;
        n -= 8;
    }

    // Handle remaining bytes
    d = (uint8_t *)d64;
    s = (const uint8_t *)s64;
    while (n--) {
        *d++ = *s++;
    }
    return dest;
}

void *memmove(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;

    if (d < s) {
        // Copy forward using 64-bit words when possible
        while (n > 0 && ((uintptr_t)d & 7)) {
            *d++ = *s++;
            n--;
        }
        uint64_t *d64 = (uint64_t *)d;
        const uint64_t *s64 = (const uint64_t *)s;
        while (n >= 8) {
            *d64++ = *s64++;
            n -= 8;
        }
        d = (uint8_t *)d64;
        s = (const uint8_t *)s64;
        while (n--) {
            *d++ = *s++;
        }
    } else {
        // Copy backward to handle overlap
        d += n;
        s += n;
        while (n > 0 && ((uintptr_t)d & 7)) {
            *--d = *--s;
            n--;
        }
        uint64_t *d64 = (uint64_t *)d;
        const uint64_t *s64 = (const uint64_t *)s;
        while (n >= 8) {
            d64--;
            s64--;
            *d64 = *s64;
            n -= 8;
        }
        d = (uint8_t *)d64;
        s = (const uint8_t *)s64;
        while (n--) {
            *--d = *--s;
        }
    }
    return dest;
}

void *memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t *)s;

    // Align to 8 bytes
    while (n > 0 && ((uintptr_t)p & 7)) {
        *p++ = (uint8_t)c;
        n--;
    }

    // Fill 64 bits at a time
    uint64_t word = (uint8_t)c;
    word |= word << 8;
    word |= word << 16;
    word |= word << 32;
    uint64_t *p64 = (uint64_t *)p;
    while (n >= 8) {
        *p64++ = word;
        n -= 8;
    }

    // Handle remaining bytes
    p = (uint8_t *)p64;
    while (n--) {
        *p++ = (uint8_t)c;
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

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    while (n--) {
        if (*p == (unsigned char)c) return (void *)p;
        p++;
    }
    return NULL;
}

void *memrchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s + n;
    while (n--) {
        if (*--p == (unsigned char)c) return (void *)p;
    }
    return NULL;
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

char *strcpy(char *restrict dest, const char *restrict src) {
    size_t i = 0;
    while ((dest[i] = src[i]) != '\0') {
        i++;
    }
    return dest;
}

char *strncpy(char *restrict dest, const char *restrict src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    for (; i < n; i++) {
        dest[i] = '\0';
    }
    return dest;
}

size_t strlcpy(char *restrict dst, const char *restrict src, size_t size) {
    const char *s = src;
    size_t n = size;

    if (n != 0) {
        while (--n != 0) {
            if ((*dst++ = *s++) == '\0') break;
        }
    }

    if (n == 0) {
        if (size != 0) *dst = '\0';
        while (*s) s++;
    }

    return (s - src - 1);
}

char* strcat(char *restrict dest, const char *restrict src) {
    char* d = dest;
    while (*d) d++;
    while ((*d++ = *src++));
    return dest;
}

char* strncat(char *restrict dest, const char *restrict src, size_t n) {
    char* d = dest;

    while (*d != '\0') d++;

    size_t i = 0;
    while (i < n && src[i] != '\0') {
        *d = src[i];
        d++;
        i++;
    }

    *d = '\0';

    return dest;
}

size_t strlcat(char *restrict dst, const char *restrict src, size_t size) {
    char *d = dst;
    const char *s = src;
    size_t n = size;
    size_t dlen;

    while (n-- != 0 && *d != '\0') d++;
    dlen = d - dst;
    n = size - dlen;

    if (n == 0) return (dlen + strlen(s));
    while (*s != '\0') {
        if (n != 1) {
            *d = *s;
            d++;
            n--;
        }
        s++;
    }

    *d = '\0';

    return (dlen + (s - src));
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

char* strtok(char *restrict str, const char *restrict delim) {
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

char *strdup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char *new_s = malloc(len);
    if (new_s) {
        memcpy(new_s, s, len);
    }
    return new_s;
}

char *strndup(const char *s, size_t n) {
    if (!s) return NULL;
    size_t len = strnlen(s, n);
    char *new_s = malloc(len + 1);
    if (new_s) {
        memcpy(new_s, s, len);
        new_s[len] = '\0';
    }
    return new_s;
}

size_t strspn(const char *s, const char *accept) {
    const char *p, *a;
    for (p = s; *p; p++) {
        for (a = accept; *a; a++)
            if (*p == *a)
                break;
        if (!*a)
            return p - s;
    }
    return p - s;
}

size_t strcspn(const char *s, const char *reject) {
    const char *p, *r;
    for (p = s; *p; p++)
        for (r = reject; *r; r++)
            if (*p == *r)
                return p - s;
    return p - s;
}

char *strpbrk(const char *s, const char *accept) {
    s += strcspn(s, accept);
    return *s ? (char *)s : NULL;
}
