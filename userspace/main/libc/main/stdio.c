#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/syscall.h>
#include <stdlib.h>
#include <fcntl.h>

// Variables, structs etc. for functions
static FILE _stdin  = { .fd = 0, .buf_len = 0, .mode = 0 };
static FILE _stdout = { .fd = 1, .buf_len = 0, .mode = 1 };
static FILE _stderr = { .fd = 2, .buf_len = 0, .mode = 0 };

FILE *stdin  = &_stdin;
FILE *stdout = &_stdout;
FILE *stderr = &_stderr;

// Helpers for functions
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
            stream->error = 1;
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
        if (write(stream->fd, &ch, 1) != 1) {
            stream->error = 1;
            return EOF;
        }
        return ch;
    }

    stream->buf[stream->buf_len++] = (char)ch;

    if (stream->buf_len >= 1024 || ch == '\n') {
        if (fflush(stream) == EOF) return EOF;
    }

    return ch;
}

int fgetc(FILE *stream) {
    if (!stream) return EOF;

    unsigned char ch;
    ssize_t nread = read(stream->fd, &ch, 1);
    if (nread == 1) return ch;
    if (nread == 0) stream->eof = 1;
    else stream->error = 1;
    return EOF;
}

int putc(int c, FILE *stream) {
    return fputc(c, stream);
}

int putchar(int c) {
    return fputc(c, stdout);
}

int fputs(const char *s, FILE *stream) {
    if (!s || !stream) return EOF;
    while (*s) { if (fputc(*s, stream) == EOF) return EOF; s++; }
    return 1;
}

int puts(const char *s) {
    if (fputs(s, stdout) == EOF) return EOF;
    return fputc('\n', stdout);
}

FILE *fopen(const char *pathname, const char *mode) {
    if (!pathname || !mode) {
        errno = EINVAL;
        return NULL;
    }
    int flags = 0;
    if (mode[0] == 'r') {
        flags = O_RDONLY;
        if (mode[1] == '+' || (mode[1] == 'b' && mode[2] == '+')) flags = O_RDWR;
    } else if (mode[0] == 'w') {
        flags = O_WRONLY | O_CREAT | O_TRUNC;
        if (mode[1] == '+' || (mode[1] == 'b' && mode[2] == '+')) flags = O_RDWR | O_CREAT | O_TRUNC;
    } else if (mode[0] == 'a') {
        flags = O_WRONLY | O_CREAT | O_APPEND;
        if (mode[1] == '+' || (mode[1] == 'b' && mode[2] == '+')) flags = O_RDWR | O_CREAT | O_APPEND;
    } else {
        errno = EINVAL;
        return NULL;
    }

    int fd = open(pathname, flags, 0666);
    if (fd < 0) return NULL;

    FILE *f = malloc(sizeof(FILE));
    if (!f) {
        close(fd);
        return NULL;
    }
    f->fd = fd;
    f->buf_len = 0;
    f->mode = ((flags & O_ACCMODE) == O_RDONLY) ? 0 : 1;
    f->eof = 0;
    f->error = 0;
    return f;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (size == 0 || nmemb == 0) return 0;

    const char *p = (const char *)ptr;
    size_t total = size * nmemb;
    for (size_t i = 0; i < total; i++) {
        if (fputc(p[i], stream) == EOF) return i / size;
    }
    return nmemb;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (size == 0 || nmemb == 0) return 0;

    char *p = (char *)ptr;
    size_t total = size * nmemb;
    for (size_t i = 0; i < total; i++) {
        int c = fgetc(stream);
        if (c == EOF) return i / size;
        p[i] = (char)c;
    }
    return nmemb;
}

int fseek(FILE *stream, long offset, int whence) {
    if (!stream) return -1;
    if (fflush(stream) == EOF) return -1;

    int ret = (int)syscall(SYS_lseek, stream->fd, offset, whence);
    if (ret == -1) {
        stream->error = 1;
        return -1;
    }

    stream->eof = 0;
    return ret;
}

long ftell(FILE *stream) {
    if (!stream) return EOF;
    long ret = (long)syscall(SYS_lseek, stream->fd, 0, 1);
    if (ret == -1) stream->error = 1;
    return ret;
}

int fclose(FILE *stream) {
    if (!stream) return EOF;
    fflush(stream);
    int ret = (int)syscall(SYS_close, stream->fd);
    if (stream != stdin && stream != stdout && stream != stderr) free(stream);
    return ret;
}

void clearerr(FILE *stream) {
    if (!stream) return;
    stream->eof = 0;
    stream->error = 0;
}

int feof(FILE *stream) {
    if (!stream) return 0;
    return stream->eof;
}

int ferror(FILE *stream) {
    if (!stream) return 0;
    return stream->error;
}

int vfprintf(FILE *stream, const char *fmt, va_list args) {
    if (!stream || !fmt) return 0;
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
            case 'i':
            case 'd':
            case 'u':
            case 'x': case 'X': {
                uint64_t val;
                if (is_long) {
                    val = va_arg(args, uint64_t);
                } else {
                    if (*p == 'd' || *p == 'i') val = (uint64_t)va_arg(args, int);
                    else           val = (uint64_t)va_arg(args, unsigned int);
                }
                bool is_neg = (*p == 'd' || *p == 'D' || *p == 'i') && (int64_t)val < 0;
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
            case 'f': {
                double fval = va_arg(args, double);
                bool is_neg = false;
                if (fval < 0) {
                    is_neg = true;
                    fval = -fval;
                }
                uint64_t int_part = (uint64_t)fval;
                double frac_part = fval - (double)int_part;
                
                char int_buf[64];
                int_to_str(int_part, int_buf, 10, false);
                
                char frac_buf[16];
                frac_buf[0] = '.';
                for (int i = 1; i <= 6; i++) {
                    frac_part *= 10.0;
                    int digit = (int)frac_part;
                    frac_buf[i] = '0' + digit;
                    frac_part -= digit;
                }
                frac_buf[7] = '\0';
                
                int len = 0;
                while (int_buf[len]) len++;
                len += 7; // .xxxxxx
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
                char *ptr = int_buf;
                while (*ptr) { 
                    if (fputc(*ptr++, stream) == EOF) return total_written;
                    total_written++; 
                }
                ptr = frac_buf;
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

int vsnprintf(char *str, size_t size, const char *fmt, va_list args) {
    if (!fmt) return 0;
    int total_written = 0;

    #define PUTVSNC(c) do { \
        if (size > 0 && (size_t)total_written < size - 1 && str) { \
            str[total_written] = (char)(c); \
        } \
        total_written++; \
    } while (0)

    for (const char *p = fmt; *p != '\0'; p++) {
        if (*p != '%') {
            PUTVSNC(*p);
            continue;
        }
        p++;
        if (*p == '\0') { 
            PUTVSNC('%');
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
                        PUTVSNC(pad_char);
                        width--; 
                    }
                }
                while (*s) { 
                    PUTVSNC(*s++);
                }
                if (left_align) {
                    while (width > len) { 
                        PUTVSNC(' ');
                        width--; 
                    }
                }
                break;
            }
            case 'o':
            case 'i':
            case 'd':
            case 'u':
            case 'x': case 'X': {
                uint64_t val;
                if (is_long) {
                    val = va_arg(args, uint64_t);
                } else {
                    if (*p == 'd' || *p == 'i') val = (uint64_t)va_arg(args, int);
                    else           val = (uint64_t)va_arg(args, unsigned int);
                }
                bool is_neg = (*p == 'd' || *p == 'D' || *p == 'i') && (int64_t)val < 0;
                if (is_neg) val = -(int64_t)val;
                int base = (*p == 'x' || *p == 'X') ? 16 : (*p == 'o' || *p == 'O') ? 8 : 10;
                char buf[64];
                int_to_str(val, buf, base, (*p == 'X'));
                int len = 0;
                while (buf[len]) len++;
                if (is_neg) len++;

                if (!left_align) {
                    while (width > len) { 
                        PUTVSNC(pad_char);
                        width--; 
                    }
                }
                if (is_neg) { 
                    PUTVSNC('-');
                }
                char *ptr = buf;
                while (*ptr) { 
                    PUTVSNC(*ptr++);
                }
                if (left_align) {
                    while (width > len) { 
                        PUTVSNC(' ');
                        width--; 
                    }
                }
                break;
            }
            case 'f': {
                double fval = va_arg(args, double);
                bool is_neg = false;
                if (fval < 0) {
                    is_neg = true;
                    fval = -fval;
                }
                uint64_t int_part = (uint64_t)fval;
                double frac_part = fval - (double)int_part;
                
                char int_buf[64];
                int_to_str(int_part, int_buf, 10, false);
                
                char frac_buf[16];
                frac_buf[0] = '.';
                for (int i = 1; i <= 6; i++) {
                    frac_part *= 10.0;
                    int digit = (int)frac_part;
                    frac_buf[i] = '0' + digit;
                    frac_part -= digit;
                }
                frac_buf[7] = '\0';
                
                int len = 0;
                while (int_buf[len]) len++;
                len += 7; // .xxxxxx
                if (is_neg) len++;

                if (!left_align) {
                    while (width > len) { 
                        PUTVSNC(pad_char);
                        width--; 
                    }
                }
                if (is_neg) { 
                    PUTVSNC('-');
                }
                char *ptr = int_buf;
                while (*ptr) { 
                    PUTVSNC(*ptr++);
                }
                ptr = frac_buf;
                while (*ptr) { 
                    PUTVSNC(*ptr++);
                }
                if (left_align) {
                    while (width > len) { 
                        PUTVSNC(' ');
                        width--; 
                    }
                }
                break;
            }
            case 'p': {
                uint64_t x = va_arg(args, uint64_t);
                char buf[64];
                int_to_str(x, buf, 16, false);
                PUTVSNC('0');
                PUTVSNC('x');
                
                int len = 0;
                while (buf[len]) len++;
                for (int i = 0; i < (16 - len); i++) { 
                    PUTVSNC('0');
                }
                char *ptr = buf;
                while (*ptr) { 
                    PUTVSNC(*ptr++);
                }
                break;
            }
            case 'c':
                PUTVSNC((char)va_arg(args, int));
                break;
            case '%':
                PUTVSNC('%');
                break;
            default:
                PUTVSNC('%');
                PUTVSNC(*p);
                break;
        }
    }

    if (size > 0 && str) {
        size_t null_idx = ((size_t)total_written < size) ? (size_t)total_written : (size - 1);
        str[null_idx] = '\0';
    }

    #undef PUTVSNC
    return total_written; 
}

int snprintf(char *str, size_t size, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int ret = vsnprintf(str, size, fmt, args);
    va_end(args);
    return ret;
}

int vsprintf(char *str, const char *fmt, va_list args) {
    return vsnprintf(str, (size_t)-1, fmt, args);
}

int sprintf(char *str, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int ret = vsprintf(str, fmt, args);
    va_end(args);
    return ret;
}

ssize_t getline(char **lineptr, size_t *n, FILE *stream) {
    if (!lineptr || !n || !stream) {
        errno = EINVAL;
        return -1;
    }
    if (*lineptr == NULL) {
        *n = 128;
        *lineptr = malloc(*n);
        if (!*lineptr) return -1;
    }
    
    size_t pos = 0;
    int c;
    while ((c = fgetc(stream)) != EOF) {
        if (pos + 1 >= *n) {
            size_t new_n = *n * 2;
            char *new_ptr = realloc(*lineptr, new_n);
            if (!new_ptr) {
                errno = ENOMEM;
                return -1;
            }
            *lineptr = new_ptr;
            *n = new_n;
        }
        (*lineptr)[pos++] = (char)c;
        if (c == '\n') break;
    }
    
    if (pos == 0 && c == EOF) {
        return -1;
    }
    
    (*lineptr)[pos] = '\0';
    return pos;
}

void perror(const char *s) {
    if (s && *s) fprintf(stderr, "%s: %s\n", s, strerror(errno));
    else fprintf(stderr, "%s\n", strerror(errno));
}
