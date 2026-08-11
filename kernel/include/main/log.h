#pragma once

#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>

#define LOG_TEXT_SIZE (128U * 1024U)
#define LOG_RECORD_COUNT 2048U
#define LOG_MESSAGE_SIZE 2048U
#define LOG_DEFAULT_LEVEL 6

#define SYSLOG_ACTION_CLOSE 0
#define SYSLOG_ACTION_OPEN 1
#define SYSLOG_ACTION_READ 2
#define SYSLOG_ACTION_READ_ALL 3
#define SYSLOG_ACTION_READ_CLEAR 4
#define SYSLOG_ACTION_CLEAR 5
#define SYSLOG_ACTION_CONSOLE_OFF 6
#define SYSLOG_ACTION_CONSOLE_ON 7
#define SYSLOG_ACTION_CONSOLE_LEVEL 8
#define SYSLOG_ACTION_SIZE_UNREAD 9
#define SYSLOG_ACTION_SIZE_BUFFER 10

typedef struct {
    uint64_t sequence;
    size_t offset;
    size_t length;
} log_record_t;

typedef struct {
    char *buf;
    size_t capacity;
    size_t stored;
    size_t total;
} format_output_t;

size_t read_log(char *buf, size_t size);
size_t read_clear_log(char *buf, size_t size);
size_t read_stream_log(char *buf, size_t size);
void clear_log(void);
size_t get_log_size(void);
size_t get_log_capacity(void);
void control_log_console(int action, int level);
int vlog(const char *fmt, va_list args);
int log(const char *fmt, ...);
