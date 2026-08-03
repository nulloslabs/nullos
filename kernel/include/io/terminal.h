#pragma once

#include <stdint.h>
#include <stdarg.h>
#include <limine.h>
#include <io/fb.h>

typedef enum {
    STATE_NORMAL,
    STATE_EXPECT_BRACKET,
    STATE_READ_PARAMS
} parser_state_t;

extern uint32_t *back_buffer;
extern uint64_t back_buffer_width;
extern uint64_t back_buffer_height;
extern uint64_t back_buffer_pitch;
extern bool     back_buffer_initialized;
extern bool     back_buffer_available;
extern bool     back_buffer_dirty;

extern uint64_t cursor_x;
extern uint64_t cursor_y;
extern uint32_t fg_color; 
extern uint32_t bg_color;
extern uint32_t default_color;
extern uint64_t line_start_y;

void sync_terminal(void);
void show_cursor(bool visible);
void scroll(void);
void clrscr(void);
int putchar(int c);
int puts(const char *s);
int vprintf(const char *fmt, va_list args);
int printf(const char *fmt, ...);
