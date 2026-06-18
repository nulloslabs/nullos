#pragma once

#define BUFSIZ 8192
#define EOF (-1)

typedef struct {
    int fd;
    char buf[1024];
    int buf_len;
    int mode;
    int eof;
    int error;
} FILE;
