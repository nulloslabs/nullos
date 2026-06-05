#include <ctype.h>
#include <fnmatch.h>
#include <string.h>

static int chr_eq(int a, int b, int flags) {
    if (flags & FNM_CASEFOLD) return tolower(a) == tolower(b);
    return a == b;
}

static int rangematch(const char **pat, int c, int flags) {
    const char *p = *pat;
    int negate = 0, ok = 0;

    if (*p == '!' || *p == '^') {
        negate = 1;
        p++;
    }

    if (*p == ']') {
        if (chr_eq(*p, c, flags)) ok = 1;
        p++;
    }

    while (*p && *p != ']') {
        int start = *p++;
        if (start == '\\' && !(flags & FNM_NOESCAPE) && *p) start = *p++;
        if (*p == '-' && p[1] && p[1] != ']') {
            int end;
            p++;
            end = *p++;
            if (end == '\\' && !(flags & FNM_NOESCAPE) && *p) end = *p++;
            if (flags & FNM_CASEFOLD) {
                start = tolower(start);
                end = tolower(end);
                c = tolower(c);
            }
            if (start <= c && c <= end) ok = 1;
        } else if (chr_eq(start, c, flags)) {
            ok = 1;
        }
    }

    if (*p == ']') p++;
    *pat = p;
    return negate ? !ok : ok;
}

static int match(const char *pat, const char *str, const char *start, int flags) {
    for (;;) {
        char pc = *pat++;
        char sc = *str;

        switch (pc) {
            case '\0':
                return sc == '\0' || ((flags & FNM_LEADING_DIR) && sc == '/');
            case '?':
                if (sc == '\0') return 0;
                if ((flags & FNM_PATHNAME) && sc == '/') return 0;
                if ((flags & FNM_PERIOD) && sc == '.' &&
                    (str == start || ((flags & FNM_PATHNAME) && str[-1] == '/'))) return 0;
                str++;
                break;
            case '*':
                while (*pat == '*') pat++;
                if ((flags & FNM_PERIOD) && sc == '.' &&
                    (str == start || ((flags & FNM_PATHNAME) && str[-1] == '/'))) return 0;
                if (*pat == '\0') {
                    if (!(flags & FNM_PATHNAME)) return 1;
                    return (flags & FNM_LEADING_DIR) || !*str || !strchr(str, '/');
                }
                for (; *str; str++) {
                    if ((flags & FNM_PATHNAME) && *str == '/') break;
                    if (match(pat, str, start, flags)) return 1;
                }
                return match(pat, str, start, flags);
            case '[':
                if (sc == '\0') return 0;
                if ((flags & FNM_PATHNAME) && sc == '/') return 0;
                if ((flags & FNM_PERIOD) && sc == '.' &&
                    (str == start || ((flags & FNM_PATHNAME) && str[-1] == '/'))) return 0;
                if (!rangematch(&pat, sc, flags)) return 0;
                str++;
                break;
            case '\\':
                if (!(flags & FNM_NOESCAPE) && *pat) pc = *pat++;
                if (!chr_eq(pc, sc, flags)) return 0;
                str++;
                break;
            default:
                if (!chr_eq(pc, sc, flags)) return 0;
                str++;
                break;
        }
    }
}

int fnmatch(const char *pattern, const char *string, int flags) {
    return match(pattern, string, string, flags) ? 0 : FNM_NOMATCH;
}
