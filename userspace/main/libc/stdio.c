#include <stdio.h>
#include <unistd.h>
#include <stdarg.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>

static FILE _stdin  = { .fd = 0, .buf_len = 0, .mode = 0 };
static FILE _stdout = { .fd = 1, .buf_len = 0, .mode = 1 };
static FILE _stderr = { .fd = 2, .buf_len = 0, .mode = 0 };

FILE *stdin  = &_stdin;
FILE *stdout = &_stdout;
FILE *stderr = &_stderr;

int fflush(FILE *stream) {
    if (!stream) {
        int ret = 0;
        if (fflush(stdout) == EOF) ret = EOF;
        if (fflush(stderr) == EOF) ret = EOF;
        return ret;
    }

    if (stream->buf_len > 0) {
        ssize_t written = write(stream->fd, stream->buf, stream->buf_len);
        if (written != stream->buf_len) {
            return EOF;
        }
        stream->buf_len = 0;
    }
    return 0;
}

int fputc(int c, FILE *stream) {
    if (!stream) return EOF;

    unsigned char ch = (unsigned char)c;

    if (stream->mode == 0) {
        if (write(stream->fd, &ch, 1) != 1) return EOF;
        return ch;
    }

    stream->buf[stream->buf_len++] = (char)ch;

    if (stream->buf_len >= 1024 || ch == '\n') {
        if (fflush(stream) == EOF) return EOF;
    }

    return ch;
}

int putchar(int c) {
    return fputc(c, stdout);
}

int fputs(const char *s, FILE *stream) {
    if (!s || !stream) return EOF;
    while (*s) {
        if (fputc(*s++, stream) == EOF) return EOF;
    }
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

int vfprintf(FILE *stream, const char *fmt, va_list args) {
    if (!stream) return 0;
    int total_written = 0;

    for (const char *p = fmt; *p != '\0'; p++) {
        if (*p != '%') {
            if (fputc(*p, stream) == EOF) return total_written;
            total_written++;
            continue;
        }
        p++;
        if (*p == '\0') { 
            if (fputc('%', stream) == EOF) return total_written;
            total_written++; 
            break; 
        }

        bool left_align = false;
        int width = 0;
        char pad_char = ' ';
        bool is_long = false;

        if (*p == '-') {
            left_align = true;
            p++;
        }
        if (*p == '0') {
            if (!left_align) pad_char = '0';
            p++;
        }
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            p++;
        }
        if (*p == 'l') {
            is_long = true;
            p++;
            if (*p == 'l') p++;
        }

        switch (*p) {
            case 's': {
                char *s = va_arg(args, char *);
                if (!s) s = "(null)";
                int len = strlen(s);
                if (!left_align) {
                    while (width > len) { 
                        if (fputc(pad_char, stream) == EOF) return total_written;
                        width--; 
                        total_written++; 
                    }
                }
                while (*s) { 
                    if (fputc(*s++, stream) == EOF) return total_written;
                    total_written++; 
                }
                if (left_align) {
                    while (width > len) { 
                        if (fputc(' ', stream) == EOF) return total_written;
                        width--; 
                        total_written++; 
                    }
                }
                break;
            }
            case 'o':
            case 'd':
            case 'u':
            case 'x': case 'X': {
                uint64_t val;
                if (is_long) {
                    val = va_arg(args, uint64_t);
                } else {
                    if (*p == 'd') val = (uint64_t)va_arg(args, int);
                    else           val = (uint64_t)va_arg(args, unsigned int);
                }
                bool is_neg = (*p == 'd' || *p == 'D') && (int64_t)val < 0;
                if (is_neg) val = -(int64_t)val;
                int base = (*p == 'x' || *p == 'X') ? 16 : (*p == 'o' || *p == 'O') ? 8 : 10;
                char buf[64];
                int_to_str(val, buf, base, (*p == 'X'));
                int len = 0;
                while (buf[len]) len++;
                if (is_neg) len++;

                if (!left_align) {
                    while (width > len) { 
                        if (fputc(pad_char, stream) == EOF) return total_written;
                        width--; 
                        total_written++; 
                    }
                }
                if (is_neg) { 
                    if (fputc('-', stream) == EOF) return total_written;
                    total_written++; 
                }
                char *ptr = buf;
                while (*ptr) { 
                    if (fputc(*ptr++, stream) == EOF) return total_written;
                    total_written++; 
                }
                if (left_align) {
                    while (width > len) { 
                        if (fputc(' ', stream) == EOF) return total_written;
                        width--; 
                        total_written++; 
                    }
                }
                break;
            }
            case 'p': {
                uint64_t x = va_arg(args, uint64_t);
                char buf[64];
                int_to_str(x, buf, 16, false);
                if (fputc('0', stream) == EOF) return total_written;
                if (fputc('x', stream) == EOF) return total_written;
                total_written += 2;
                int len = 0;
                while (buf[len]) len++;
                for (int i = 0; i < (16 - len); i++) { 
                    if (fputc('0', stream) == EOF) return total_written;
                    total_written++; 
                }
                char *ptr = buf;
                while (*ptr) { 
                    if (fputc(*ptr++, stream) == EOF) return total_written;
                    total_written++; 
                }
                break;
            }
            case 'c':
                if (fputc((char)va_arg(args, int), stream) == EOF) return total_written;
                total_written++;
                break;
            case '%':
                if (fputc('%', stream) == EOF) return total_written;
                total_written++;
                break;
            default:
                if (fputc('%', stream) == EOF) return total_written;
                if (fputc(*p, stream) == EOF) return total_written;
                total_written += 2;
                break;
        }
    }
    return total_written;
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
    fprintf(stderr, "%s: %s\n", s, strerror(errno));
}
