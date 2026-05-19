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
int putc(int c, FILE *stream);
int putchar(int c);

int fputs(const char *s, FILE *stream);
int puts(const char *s);

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
int fseek(FILE *stream, long offset, int whence);
long ftell(FILE *stream);
int fclose(FILE *stream);

int vfprintf(FILE *stream, const char *fmt, va_list args);
int fprintf(FILE *stream, const char *fmt, ...);
int vprintf(const char *fmt, va_list args);
int printf(const char *fmt, ...);

int vsnprintf(char *str, size_t size, const char *fmt, va_list args);
int snprintf(char *str, size_t size, const char *fmt, ...);
int vsprintf(char *str, const char *fmt, va_list args);
int sprintf(char *str, const char *fmt, ...);

void perror(const char *s);
