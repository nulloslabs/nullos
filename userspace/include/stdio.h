#pragma once

#include <stddef.h>
#include <stdarg.h>

#define EOF (-1)

typedef struct {
    int fd;
    char buf[1024];
    int buf_len;
    int mode;
} FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

int fflush(FILE *stream);

int fputc(int c, FILE *stream);
#define putc(c, s) fputc(c, s) // NOTE: putc() and fputc() are the same thing, just put macro instead of wasting space
int putchar(int c);

int fputs(const char *s, FILE *stream);
int puts(const char *s);

int vfprintf(FILE *stream, const char *fmt, va_list args);
int fprintf(FILE *stream, const char *fmt, ...);
int vprintf(const char *fmt, va_list args);
int printf(const char *fmt, ...);

void perror(const char *s);
