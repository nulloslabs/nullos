#pragma once

#include <freestanding/stdint.h>
#include <freestanding/stdbool.h>
#include <limine/limine.h>
#include <io/framebuffer.h>

extern uint64_t cursor_x;
extern uint64_t cursor_y;
extern uint32_t fg_color; 
extern uint32_t bg_color;
extern uint32_t default_color;
extern uint64_t line_start_y;

typedef enum {
    STATE_NORMAL,
    STATE_EXPECT_BRACKET,
    STATE_READ_PARAMS
} parser_state_t;

void reset_term_line_start(void);
void show_cursor(bool visible);
void scroll(struct limine_framebuffer *fb);
void clrscr(void);
void putc(char c);
void puts(const char *s);
void printf(const char *fmt, ...);