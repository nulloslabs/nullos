#pragma once

#include <stddef.h>
#include <stdarg.h>

#define EOF (-1)

typedef struct {
    int fd;
} FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

// NOTE: putc() and fputc() are the same thing.

int fputc(int c, FILE *stream);
#define putc(c, s) fputc(c, s)
int putchar(int c);

int fputs(const char *s, FILE *stream);
int puts(const char *s);

int vfprintf(FILE *stream, const char *fmt, va_list args);
int fprintf(FILE *stream, const char *fmt, ...);
int vprintf(const char *fmt, va_list args);
int printf(const char *fmt, ...);

void perror(const char *s);