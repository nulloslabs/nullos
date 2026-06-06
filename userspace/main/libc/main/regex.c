#include <ctype.h>
#include <errno.h>
#include <regex.h>
#include <stdlib.h>
#include <string.h>

static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static int chr_eq(int a, int b, int flags) {
    if (flags & REG_ICASE) return tolower(a) == tolower(b);
    return a == b;
}

static const char *atom_end(const char *re) {
    if (*re == '[') {
        re++;
        if (*re == '^' || *re == ']') re++;
        while (*re && *re != ']') {
            if (*re == '\\' && re[1]) re++;
            re++;
        }
        return *re == ']' ? re + 1 : re;
    }
    if (*re == '\\' && re[1]) return re + 2;
    return *re ? re + 1 : re;
}

static int class_match(const char *start, const char *end, int c, int flags) {
    int negate = 0, ok = 0;
    const char *p = start + 1;

    if (*p == '^') {
        negate = 1;
        p++;
    }
    if (*p == ']') {
        if (chr_eq(']', c, flags)) ok = 1;
        p++;
    }
    while (p < end - 1) {
        int a = *p++;
        if (a == '\\' && p < end - 1) a = *p++;
        if (*p == '-' && p + 1 < end - 1) {
            int b;
            p++;
            b = *p++;
            if (b == '\\' && p < end - 1) b = *p++;
            if (flags & REG_ICASE) {
                a = tolower(a);
                b = tolower(b);
                c = tolower(c);
            }
            if (a <= c && c <= b) ok = 1;
        } else if (chr_eq(a, c, flags)) {
            ok = 1;
        }
    }
    return negate ? !ok : ok;
}

static int atom_match(const char *start, const char *end, int c, int flags) {
    if (c == '\0') return 0;
    if (*start == '.') {
        if ((flags & REG_NEWLINE) && c == '\n') return 0;
        return 1;
    }
    if (*start == '[') return class_match(start, end, c, flags);
    if (*start == '\\' && start + 1 < end) return chr_eq(start[1], c, flags);
    return chr_eq(*start, c, flags);
}

static int match_here(const char *re, const char *text, int flags, const char **end_out);

static int match_repeat(const char *atom, const char *after_atom,
                        const char *after_op, const char *text,
                        int min, int flags, const char **end_out) {
    const char *t = text;
    int count = 0;
    while (atom_match(atom, after_atom, *t, flags)) {
        t++;
        count++;
    }
    while (count >= min) {
        if (match_here(after_op, t, flags, end_out)) return 1;
        t--;
        count--;
    }
    return 0;
}

static int match_here(const char *re, const char *text, int flags, const char **end_out) {
    const char *end;

    if (*re == '\0') {
        if (end_out) *end_out = text;
        return 1;
    }
    if (*re == '$' && re[1] == '\0') {
        if (*text == '\0') {
            if (end_out) *end_out = text;
            return 1;
        }
        return 0;
    }

    end = atom_end(re);
    if (*end == '*') return match_repeat(re, end, end + 1, text, 0, flags, end_out);
    if (*end == '+') return match_repeat(re, end, end + 1, text, 1, flags, end_out);
    if (*end == '?') {
        if (atom_match(re, end, *text, flags) && match_here(end + 1, text + 1, flags, end_out))
            return 1;
        return match_here(end + 1, text, flags, end_out);
    }
    if (atom_match(re, end, *text, flags)) return match_here(end, text + 1, flags, end_out);
    return 0;
}

static int validate(const char *re, size_t *subs) {
    int brackets = 0, parens = 0;
    *subs = 0;
    for (const char *p = re; *p; p++) {
        if (*p == '\\') {
            if (!p[1]) return REG_EESCAPE;
            p++;
        } else if (*p == '[') {
            brackets = 1;
            p++;
            if (*p == '^' || *p == ']') p++;
            while (*p && *p != ']') {
                if (*p == '\\' && p[1]) p++;
                p++;
            }
            if (!*p) return REG_EBRACK;
            brackets = 0;
        } else if (*p == '(') {
            parens++;
            (*subs)++;
        } else if (*p == ')') {
            if (parens == 0) return REG_EPAREN;
            parens--;
        } else if ((*p == '*' || *p == '+' || *p == '?') &&
                   (p == re || p[-1] == '^' || p[-1] == '(')) {
            return REG_BADRPT;
        }
    }
    if (brackets) return REG_EBRACK;
    if (parens) return REG_EPAREN;
    return 0;
}

int regcomp(regex_t *preg, const char *regex, int cflags) {
    int err;
    if (!preg || !regex) return REG_BADPAT;
    err = validate(regex, &preg->re_nsub);
    if (err) return err;
    preg->__pattern = xstrdup(regex);
    if (!preg->__pattern) return REG_ESPACE;
    preg->__cflags = cflags;
    return 0;
}

int regexec(const regex_t *preg, const char *string, size_t nmatch, regmatch_t pmatch[], int eflags) {
    const char *re;
    (void)eflags;

    if (!preg || !preg->__pattern || !string) return REG_NOMATCH;
    re = preg->__pattern;

    if (*re == '^') {
        const char *end;
        if (match_here(re + 1, string, preg->__cflags, &end)) {
            if (!(preg->__cflags & REG_NOSUB) && nmatch && pmatch) {
                pmatch[0].rm_so = 0;
                pmatch[0].rm_eo = (regoff_t)(end - string);
            }
            return 0;
        }
        return REG_NOMATCH;
    }

    for (const char *s = string;; s++) {
        const char *end;
        if (match_here(re, s, preg->__cflags, &end)) {
            if (!(preg->__cflags & REG_NOSUB) && nmatch && pmatch) {
                pmatch[0].rm_so = (regoff_t)(s - string);
                pmatch[0].rm_eo = (regoff_t)(end - string);
            }
            return 0;
        }
        if (*s == '\0') break;
    }
    return REG_NOMATCH;
}

size_t regerror(int errcode, const regex_t *preg, char *errbuf, size_t errbuf_size) {
    const char *msg;
    (void)preg;
    switch (errcode) {
        case 0: msg = "Success"; break;
        case REG_NOMATCH: msg = "No match"; break;
        case REG_BADPAT: msg = "Invalid pattern"; break;
        case REG_EESCAPE: msg = "Trailing backslash"; break;
        case REG_EBRACK: msg = "Unmatched bracket"; break;
        case REG_EPAREN: msg = "Unmatched parenthesis"; break;
        case REG_BADRPT: msg = "Invalid repetition"; break;
        case REG_ESPACE: msg = "Out of memory"; break;
        default: msg = "Regex error"; break;
    }
    size_t len = strlen(msg) + 1;
    if (errbuf && errbuf_size) {
        size_t n = len < errbuf_size ? len : errbuf_size;
        memcpy(errbuf, msg, n);
        errbuf[n - 1] = '\0';
    }
    return len;
}

void regfree(regex_t *preg) {
    if (!preg) return;
    free(preg->__pattern);
    preg->__pattern = NULL;
    preg->re_nsub = 0;
    preg->__cflags = 0;
}
