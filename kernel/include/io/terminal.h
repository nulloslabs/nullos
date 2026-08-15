#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <limine.h>
#include <io/fb.h>

#define FONT_PENDING_BUFFER_SIZE 4096
#define TERMINAL_MAX_COLUMNS 8192
#define TAB_STOP_WORD_BITS 64
#define TAB_STOP_WORDS (TERMINAL_MAX_COLUMNS / TAB_STOP_WORD_BITS)

typedef enum {
    STATE_NORMAL,
    STATE_EXPECT_BRACKET,
    STATE_READ_PARAMS
} parser_state_t;

typedef struct {
    unsigned char character;
    uint32_t foreground;
    uint32_t background;
} terminal_cell_t;

typedef struct {
    parser_state_t state;
    char ansi_buffer[32];
    int ansi_idx;
    bool is_bold;
    bool is_reverse;
    uint32_t reverse_bg;
    int last_printable_char;
    bool acs_active;
    bool expect_charset_designator;
    bool cursor_visible;
    bool cursor_enabled;
    uint64_t tab_stops[TAB_STOP_WORDS];
    bool tab_stops_initialized;
    uint32_t cursor_saved_pixels[32 * 32];
    uint64_t cursor_saved_x;
    uint64_t cursor_saved_y;
    uint64_t cursor_saved_w;
    uint64_t cursor_saved_h;
    bool region_set;
    uint64_t region_top;
    uint64_t region_bottom;
    uint64_t saved_cursor_x;
    uint64_t saved_cursor_y;
    uint32_t saved_fg;
    uint32_t saved_bg;
    bool saved_bold;
    bool saved_reverse;
    bool alt_active;
    uint64_t alt_saved_cursor_x;
    uint64_t alt_saved_cursor_y;
    uint32_t alt_saved_fg;
    uint32_t alt_saved_bg;
    bool alt_saved_bold;
    bool alt_saved_reverse;
    uint32_t *back_buffer;
    uint32_t *alt_back_buffer;
    terminal_cell_t *cell_buffer;
    terminal_cell_t *alt_cell_buffer;
    uint64_t cursor_x;
    uint64_t cursor_y;
    uint32_t fg_color;
    uint32_t bg_color;
    uint32_t default_color;
    uint64_t line_start_y;
    bool initialized;
} terminal_vt_t;

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
uint64_t write_terminal(const char *buf, uint64_t count, bool onlcr);
uint64_t write_terminal_tty(int tty_idx, const char *buf, uint64_t count, bool onlcr);
void switch_terminal_tty(int tty_idx);
int vprintf(const char *fmt, va_list args);
int printf(const char *fmt, ...);
void init_terminal_backbuffer(void);
