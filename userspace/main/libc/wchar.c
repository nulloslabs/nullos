#include <stddef.h>
#include <limits.h>
#include <wchar.h>
#include <errno.h>

long long wcstoll(const wchar_t *nptr, wchar_t **endptr, int base) {
    const wchar_t *s = nptr;
    unsigned long long acc, cutoff;
    wchar_t c;
    int neg = 0, any = 0, cutlim;

    do {
        c = *s++;
    } while (c == L' ' || c == L'\f' || c == L'\n' || c == L'\r' || c == L'\t' || c == L'\v');

    if (c == L'-') {
        neg = 1;
        c = *s++;
    } else if (c == L'+') {
        c = *s++;
    }

    if ((base == 0 || base == 16) &&
        c == L'0' && (*s == L'x' || *s == L'X')) {
        if ((s[1] >= L'0' && s[1] <= L'9') ||
            (s[1] >= L'A' && s[1] <= L'F') ||
            (s[1] >= L'a' && s[1] <= L'f')) {
            c = s[1];
            s += 2;
            base = 16;
        }
    }

    if (base == 0)
        base = (c == L'0') ? 8 : 10;

    cutoff = neg ? -(unsigned long long)LLONG_MIN : LLONG_MAX;
    cutlim = cutoff % (unsigned long long)base;
    cutoff /= (unsigned long long)base;

    for (acc = 0;; c = *s++) {
        if (c >= L'0' && c <= L'9')
            c -= L'0';
        else if (c >= L'A' && c <= L'Z')
            c -= L'A' - 10;
        else if (c >= L'a' && c <= L'z')
            c -= L'a' - 10;
        else
            break;
        if (c >= base)
            break;
        if (any < 0 || acc > cutoff || (acc == cutoff && c > cutlim)) {
            any = -1;
        } else {
            any = 1;
            acc = acc * base + c;
        }
    }

    if (any < 0) {
        acc = neg ? (unsigned long long)LLONG_MIN : (unsigned long long)LLONG_MAX;
        errno = ERANGE;
    } else if (neg) {
        acc = -acc;
    }

    if (endptr != NULL)
        *endptr = any ? (wchar_t *)(s - 1) : (wchar_t *)nptr;

    return (long long)acc;
}

unsigned long long wcstoull(const wchar_t *nptr, wchar_t **endptr, int base) {
    const wchar_t *s = nptr;
    unsigned long long acc, cutoff;
    wchar_t c;
    int neg = 0, any = 0, cutlim;

    do {
        c = *s++;
    } while (c == L' ' || c == L'\f' || c == L'\n' || c == L'\r' || c == L'\t' || c == L'\v');

    if (c == L'-') {
        neg = 1;
        c = *s++;
    } else if (c == L'+') {
        c = *s++;
    }

    if ((base == 0 || base == 16) &&
        c == L'0' && (*s == L'x' || *s == L'X')) {
        if ((s[1] >= L'0' && s[1] <= L'9') ||
            (s[1] >= L'A' && s[1] <= L'F') ||
            (s[1] >= L'a' && s[1] <= L'f')) {
            c = s[1];
            s += 2;
            base = 16;
        }
    }

    if (base == 0)
        base = (c == L'0') ? 8 : 10;

    cutoff = ULLONG_MAX / (unsigned long long)base;
    cutlim = ULLONG_MAX % (unsigned long long)base;

    for (acc = 0;; c = *s++) {
        if (c >= L'0' && c <= L'9')
            c -= L'0';
        else if (c >= L'A' && c <= L'Z')
            c -= L'A' - 10;
        else if (c >= L'a' && c <= L'z')
            c -= L'a' - 10;
        else
            break;
        if (c >= base)
            break;
        if (any < 0 || acc > cutoff || (acc == cutoff && c > cutlim)) {
            any = -1;
        } else {
            any = 1;
            acc = acc * base + c;
        }
    }

    if (any < 0) {
        acc = ULLONG_MAX;
        errno = ERANGE;
    } else if (neg) {
        acc = -acc;
    }

    if (endptr != NULL)
        *endptr = any ? (wchar_t *)(s - 1) : (wchar_t *)nptr;

    return acc;
}
