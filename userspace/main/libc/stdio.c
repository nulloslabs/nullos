#include <stdio.h>
#include <unistd.h>
#include <stdarg.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>

static FILE _stdin  = { .fd = 0 };
static FILE _stdout = { .fd = 1 };
static FILE _stderr = { .fd = 2 };

FILE *stdin  = &_stdin;
FILE *stdout = &_stdout;
FILE *stderr = &_stderr;

int fputc(int c, FILE *stream) {
    if (!stream) return EOF;
    unsigned char ch = (unsigned char)c;
    if (write(stream->fd, &ch, 1) != 1) return EOF;
    return ch;
}

int putchar(int c) {
    return fputc(c, stdout);
}

int fputs(const char *s, FILE *stream) {
    if (!s || !stream) return EOF;
    size_t len = strlen(s);
    if (write(stream->fd, s, (int)len) != (int)len) return EOF;
    return 1;
}

int puts(const char *s) {
    if (fputs(s, stdout) == EOF) return EOF;
    return fputc('\n', stdout);
}

static void int_to_str(unsigned long long value, char *buf, int base, bool uppercase) {
    char temp[64];
    int i = 0;

    if (value == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }

    char hex_offset = uppercase ? 'A' : 'a';
    while (value > 0) {
        int rem = (int)(value % base);
        temp[i++] = (rem < 10) ? (rem + '0') : (rem - 10 + hex_offset);
        value /= base;
    }

    int j = 0;
    while (i > 0) buf[j++] = temp[--i];
    buf[j] = '\0';
}

typedef struct {
    FILE *stream;
    char buf[256];
    int buf_len;
    int written;
} vfprintf_state_t;

static void flush_buf(vfprintf_state_t *state) {
    if (state->buf_len > 0) {
        write(state->stream->fd, state->buf, state->buf_len);
        state->buf_len = 0;
    }
}

static void put_buf(vfprintf_state_t *state, char c) {
    state->buf[state->buf_len++] = c;
    if (state->buf_len >= 256) flush_buf(state);
    state->written++;
}

int vfprintf(FILE *stream, const char *fmt, va_list args) {
    vfprintf_state_t state;
    state.stream = stream;
    state.buf_len = 0;
    state.written = 0;

    for (const char *p = fmt; *p != '\0'; p++) {
        if (*p != '%') {
            put_buf(&state, *p);
            continue;
        }
        p++;
        if (*p == '\0') { put_buf(&state, '%'); break; }

        bool left_align = false;
        int width = 0;
        char pad_char = ' ';
        bool is_long = false;

        if (*p == '-') { left_align = true; p++; }
        if (*p == '0') { if (!left_align) pad_char = '0'; p++; }
        while (*p >= '0' && *p <= '9') { width = width * 10 + (*p - '0'); p++; }
        if (*p == 'l') { is_long = true; p++; if (*p == 'l') p++; }

        switch (*p) {
            case 's': {
                char *s = va_arg(args, char *);
                if (!s) s = "(null)";
                int len = (int)strlen(s);
                if (!left_align) while (width > len) { put_buf(&state, pad_char); width--; }
                while (*s) { put_buf(&state, *s++); }
                if (left_align) while (width > len) { put_buf(&state, ' '); width--; }
                break;
            }
            case 'd': case 'D':
            case 'u': case 'U':
            case 'o': case 'O':
            case 'x': case 'X': {
                uint64_t val;
                bool forced_long = (*p == 'D' || *p == 'U' || *p == 'O');
                if (is_long || forced_long) val = va_arg(args, uint64_t);
                else if (*p == 'd') val = (uint64_t)(int64_t)va_arg(args, int);
                else val = (uint64_t)va_arg(args, unsigned int);

                bool is_neg = (*p == 'd' || *p == 'D') && (int64_t)val < 0;
                if (is_neg) val = (uint64_t)(-(int64_t)val);

                int base = (*p == 'x' || *p == 'X') ? 16 : (*p == 'o' || *p == 'O') ? 8 : 10;
                char num_buf[64];
                int_to_str(val, num_buf, base, (*p == 'X'));
                int len = (int)strlen(num_buf);
                if (is_neg) len++;

                if (!left_align) while (width > len) { put_buf(&state, pad_char); width--; }
                if (is_neg) { put_buf(&state, '-'); }
                for (char *ptr = num_buf; *ptr; ptr++) { put_buf(&state, *ptr); }
                if (left_align) while (width > len) { put_buf(&state, ' '); width--; }
                break;
            }
            case 'p': {
                uint64_t x = va_arg(args, uint64_t);
                char num_buf[64];
                int_to_str(x, num_buf, 16, false);
                put_buf(&state, '0'); put_buf(&state, 'x');
                int len = (int)strlen(num_buf);
                for (int i = 0; i < 16 - len; i++) { put_buf(&state, '0'); }
                for (char *ptr = num_buf; *ptr; ptr++) { put_buf(&state, *ptr); }
                break;
            }
            case 'c':
                put_buf(&state, (char)va_arg(args, int));
                break;
            case '%':
                put_buf(&state, '%');
                break;
            default:
                put_buf(&state, '%'); put_buf(&state, *p);
                break;
        }
    }

    flush_buf(&state);
    return state.written;
}

int fprintf(FILE *stream, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int ret = vfprintf(stream, fmt, args);
    va_end(args);
    return ret;
}

int vprintf(const char *fmt, va_list args) {
    return vfprintf(stdout, fmt, args);
}

int printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int ret = vprintf(fmt, args);
    va_end(args);
    return ret;
}

void perror(const char *s) {
    printf("%s: %s\n", s, strerror(errno));
}