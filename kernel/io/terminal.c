#include <freestanding/stdint.h>
#include <freestanding/stdbool.h>
#include <freestanding/stdarg.h>
#include <main/string.h>
#include <main/limine_req.h>
#include <main/spinlock.h>
#include <main/halt.h>
#include <io/framebuffer.h>
#include <io/fonts.h>
#include <io/serial.h>
#include <io/terminal.h>

// Back buffer for fast scrolling (double-buffering)
// Statically allocated to work before MM is initialized
#define BACKBUFFER_SIZE (1920 * 1080 * 4)  // Max 1920x1080 @ 32bpp
static uint8_t back_buffer_static[BACKBUFFER_SIZE] __attribute__((aligned(4096)));
static uint32_t *back_buffer = NULL;
static uint64_t back_buffer_width = 0;
static uint64_t back_buffer_height = 0;
static uint64_t back_buffer_pitch = 0;
static bool back_buffer_initialized = false;
static bool back_buffer_available = false;  // Set when MM is ready

uint64_t cursor_x = 0;
uint64_t cursor_y = 0;
uint32_t fg_color = 0xAAAAAA;
uint32_t bg_color = 0x000000;
uint32_t default_color = 0xAAAAAA;
uint64_t line_start_y = 0; // Track where current input line started

static parser_state_t state = STATE_NORMAL;
static char ansi_buffer[16];
static int ansi_idx = 0;
static bool is_bold = false;
static bool cursor_visible = false;
static bool cursor_enabled = true;
static spinlock_t term_lock = SPINLOCK_INIT;

static void flush_backbuffer(struct limine_framebuffer *fb) {
    if (!back_buffer_initialized || !back_buffer || !back_buffer_available) return;
    if (!fb || !fb->address) return;

    uint64_t width = fb->width;
    uint64_t height = fb->height;

    // Validate framebuffer dimensions
    if (width == 0 || height == 0 || width > 8192 || height > 8192) return;

    uint8_t *fb_addr = (uint8_t *)fb->address;
    uint8_t r_size = fb->red_mask_size;
    uint8_t r_shift = fb->red_mask_shift;
    uint8_t g_size = fb->green_mask_size;
    uint8_t g_shift = fb->green_mask_shift;
    uint8_t b_size = fb->blue_mask_size;
    uint8_t b_shift = fb->blue_mask_shift;
    uint8_t bpp = fb->bpp;

    for (uint64_t y = 0; y < height; y++) {
        uint64_t fb_row_offset = y * fb->pitch;
        uint64_t bb_row_offset = y * width;

        for (uint64_t x = 0; x < width; x++) {
            uint32_t color = back_buffer[bb_row_offset + x];
            uint8_t r = (color >> 16) & 0xFF;
            uint8_t g = (color >> 8) & 0xFF;
            uint8_t b = color & 0xFF;

            uint32_t pixel = 0;
            pixel |= (uint32_t)((r * ((1 << r_size) - 1)) / 255) << r_shift;
            pixel |= (uint32_t)((g * ((1 << g_size) - 1)) / 255) << g_shift;
            pixel |= (uint32_t)((b * ((1 << b_size) - 1)) / 255) << b_shift;

            uint64_t fb_offset = fb_row_offset + x * ((bpp + 7) / 8);
            switch (bpp) {
                case 15:
                case 16: *(uint16_t *)(fb_addr + fb_offset) = (uint16_t)pixel; break;
                case 24:
                    fb_addr[fb_offset + 0] = (uint8_t)(pixel & 0xFF);
                    fb_addr[fb_offset + 1] = (uint8_t)((pixel >> 8) & 0xFF);
                    fb_addr[fb_offset + 2] = (uint8_t)((pixel >> 16) & 0xFF);
                    break;
                case 32: *(uint32_t *)(fb_addr + fb_offset) = pixel; break;
                default: halt(); break;
            }
        }
    }
}

static void flush_region_backbuffer(struct limine_framebuffer *fb, uint64_t x, uint64_t y, uint64_t w, uint64_t h) {
    if (!back_buffer_initialized || !back_buffer || !back_buffer_available) return;
    if (!fb || !fb->address) return;
    if (x >= fb->width || y >= fb->height) return;
    if (w == 0 || h == 0 || w > 8192 || h > 8192) return;

    // Clamp region to framebuffer bounds
    if (x + w > fb->width) w = fb->width - x;
    if (y + h > fb->height) h = fb->height - y;

    uint8_t *fb_addr = (uint8_t *)fb->address;
    uint8_t r_size = fb->red_mask_size;
    uint8_t r_shift = fb->red_mask_shift;
    uint8_t g_size = fb->green_mask_size;
    uint8_t g_shift = fb->green_mask_shift;
    uint8_t b_size = fb->blue_mask_size;
    uint8_t b_shift = fb->blue_mask_shift;
    uint8_t bpp = fb->bpp;

    for (uint64_t row = 0; row < h; row++) {
        uint64_t fb_row_offset = (y + row) * fb->pitch;
        uint64_t bb_row_offset = (y + row) * back_buffer_width;

        for (uint64_t col = 0; col < w; col++) {
            uint32_t color = back_buffer[bb_row_offset + (x + col)];
            uint8_t r = (color >> 16) & 0xFF;
            uint8_t g = (color >> 8) & 0xFF;
            uint8_t b = color & 0xFF;

            uint32_t pixel = 0;
            pixel |= (uint32_t)((r * ((1 << r_size) - 1)) / 255) << r_shift;
            pixel |= (uint32_t)((g * ((1 << g_size) - 1)) / 255) << g_shift;
            pixel |= (uint32_t)((b * ((1 << b_size) - 1)) / 255) << b_shift;

            uint64_t fb_offset = fb_row_offset + (x + col) * ((bpp + 7) / 8);
            switch (bpp) {
                case 15:
                case 16: *(uint16_t *)(fb_addr + fb_offset) = (uint16_t)pixel; break;
                case 24:
                    fb_addr[fb_offset + 0] = (uint8_t)(pixel & 0xFF);
                    fb_addr[fb_offset + 1] = (uint8_t)((pixel >> 8) & 0xFF);
                    fb_addr[fb_offset + 2] = (uint8_t)((pixel >> 16) & 0xFF);
                    break;
                case 32: *(uint32_t *)(fb_addr + fb_offset) = pixel; break;
                default: halt(); break;
            }
        }
    }
}

static void fill_rect_backbuffer(uint64_t x, uint64_t y, uint64_t w, uint64_t h, uint32_t color) {
    if (!back_buffer_initialized || !back_buffer || !back_buffer_available) return;
    if (w == 0 || h == 0 || w > 8192 || h > 8192) return;

    // Clamp to backbuffer bounds
    if (x >= back_buffer_width || y >= back_buffer_height) return;
    if (x + w > back_buffer_width) w = back_buffer_width - x;
    if (y + h > back_buffer_height) h = back_buffer_height - y;

    for (uint64_t row = 0; row < h; row++) {
        uint64_t offset = (y + row) * back_buffer_width + x;
        if (offset >= (back_buffer_pitch * back_buffer_height / 4)) break;

        uint32_t *ptr = back_buffer + offset;
        for (uint64_t col = 0; col < w; col++) {
            ptr[col] = color;
        }
    }
}

static void scroll_backbuffer(uint64_t line_height, uint32_t bg_color) {
    if (!back_buffer_initialized || !back_buffer || !back_buffer_available) return;
    if (line_height == 0 || line_height >= back_buffer_height) return;

    uint64_t bytes_per_line = line_height * back_buffer_pitch;
    uint64_t total_size = back_buffer_height * back_buffer_pitch;

    // Fast memmove in RAM (much faster than VRAM)
    memmove(back_buffer,
            (uint8_t *)back_buffer + bytes_per_line,
            total_size - bytes_per_line);

    // Clear bottom line
    uint64_t start_y = back_buffer_height - line_height;
    uint64_t pixels_per_line = back_buffer_width * line_height;
    uint64_t start_offset = start_y * back_buffer_width;

    if (start_offset < (back_buffer_pitch * back_buffer_height / 4)) {
        for (uint64_t i = 0; i < pixels_per_line; i++) {
            back_buffer[start_offset + i] = bg_color;
        }
    }
}

static void put_pixel_bb(uint32_t x, uint32_t y, uint32_t color) {
    if (!back_buffer_initialized || !back_buffer || !back_buffer_available) return;
    if (x >= back_buffer_width || y >= back_buffer_height) return;
    back_buffer[y * back_buffer_width + x] = color;
}

static void putc_bb(char c, int x, int y, uint32_t fg, uint32_t bg) {
    if (!current_font_w || !current_font_h) return;
    if (!back_buffer_initialized || !back_buffer || !back_buffer_available) return;

    unsigned char *glyph = &current_font[(unsigned char)c * current_font_h];

    for (int row = 0; row < current_font_h; row++) {
        unsigned char row_data = glyph[row];
        for (int col = 0; col < current_font_w; col++) {
            if (row_data & (0x80 >> col)) {
                put_pixel_bb(x + col, y + row, fg);
            } else {
                put_pixel_bb(x + col, y + row, bg);
            }
        }
    }
}

static inline uint32_t rgb_to_hex(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static uint32_t ansi_to_hex(int code, bool bold) {
    static const uint32_t vga_colors[] = {
        0x000000, 0xAA0000, 0x00AA00, 0xAA5500,
        0x0000AA, 0xAA00AA, 0x00AAAA, 0xAAAAAA,
        0x555555, 0xFF5555, 0x55FF55, 0xFFFF55,
        0x5555FF, 0xFF55FF, 0x55FFFF, 0xFFFFFF
    };

    if ((code >= 30 && code <= 37) || (code >= 40 && code <= 47)) {
        int index = (code >= 40) ? (code - 40) : (code - 30);
        return vga_colors[bold ? (index + 8) : index];
    }

    if ((code >= 90 && code <= 97) || (code >= 100 && code <= 107)) {
        int index = (code >= 100) ? (code - 100) : (code - 90);
        return vga_colors[index + 8];
    }

    if (code >= 0 && code <= 255) {
        if (code < 16) return vga_colors[code];
        if (code >= 16 && code <= 231) {
            int r = (code - 16) / 36, g = ((code - 16) % 36) / 6, b = (code - 16) % 6;
            return rgb_to_hex(r ? r * 40 + 55 : 0, g ? g * 40 + 55 : 0, b ? b * 40 + 55 : 0);
        }
        if (code >= 232 && code <= 255) {
            uint8_t gray = (code - 232) * 10 + 8;
            return rgb_to_hex(gray, gray, gray);
        }
    }
    return default_color;
}

static bool is_visible_control(unsigned char c) {
    if (c == 0x7F) return true;
    if (c >= 0x20) return false;

    switch (c) {
        case '\033':
        case '\r':
        case '\n':
        case '\b':
        case '\t':
            return false;
        default:
            return true;
    }
}

static char visible_control_char(unsigned char c) {
    return (c == 0x7F) ? '?' : (char)(c + '@');
}

static void int_to_str(uint64_t value, char *buf, size_t buf_size, int base, bool uppercase) {
    char temp[64];
    int i = 0;

    // Ensure base is valid (default to 10 if invalid)
    if (base <= 0 || base > 36) base = 10;

    if (value == 0) {
        if (buf_size > 1) { buf[0] = '0'; buf[1] = '\0'; }
        return;
    }

    // Determine the letter offset: 'A' (65) for uppercase, 'a' (97) for lowercase
    char hex_offset = uppercase ? 'A' : 'a';

    while (value > 0 && i < 63) {
        uint64_t rem = value % base;
        // If rem is 10, (10 - 10 + 'A') = 'A'. Perfect.
        temp[i++] = (rem < 10) ? (rem + '0') : (rem - 10 + hex_offset);
        value /= base;
    }

    int j = 0;
    while (i > 0 && j < (int)buf_size - 1) {
        buf[j++] = temp[--i];
    }
    buf[j] = '\0';
}

void show_cursor(bool visible) {
    if (!current_font_w || !current_font_h) return;
    if (cursor_visible == visible) return;
    if (!fb_req.response || fb_req.response->framebuffer_count < 1) return;
    struct limine_framebuffer *fb = fb_req.response->framebuffers[0];

    if (!back_buffer_initialized) {
        back_buffer_width = fb->width;
        back_buffer_height = fb->height;
        back_buffer_pitch = fb->width * sizeof(uint32_t);

        uint64_t required_size = back_buffer_pitch * fb->height;
        if (required_size <= BACKBUFFER_SIZE) {
            back_buffer = (uint32_t *)back_buffer_static;
            back_buffer_initialized = true;
            back_buffer_available = true;
        } else {
            back_buffer = NULL;
            back_buffer_initialized = true;
            back_buffer_available = false;
        }
    }

    if (visible) {
        char current_char = ' ';
        unsigned char *glyph = &current_font[(unsigned char)current_char * current_font_h];

        for (int row_idx = 0; row_idx < current_font_h; row_idx++) {
            unsigned char row_data = glyph[row_idx];
            
            for (int col_idx = 0; col_idx < current_font_w; col_idx++) {
                uint32_t color = (row_data & (0x80 >> col_idx)) ? bg_color : fg_color;
                put_pixel_fb(cursor_x + col_idx, cursor_y + row_idx, color);
            }
        }
    } else {
        if (back_buffer_available) flush_region_backbuffer(fb, cursor_x, cursor_y, current_font_w, current_font_h);
        else putc_fb(' ', cursor_x, cursor_y, fg_color, bg_color);
    }

    cursor_visible = visible;
}

void scroll(struct limine_framebuffer *fb) {
    uint64_t line_height = current_font_h;

    // Initialize back buffer if not already done (inline)
    if (!back_buffer_initialized) {
        back_buffer_width = fb->width;
        back_buffer_height = fb->height;
        back_buffer_pitch = fb->width * sizeof(uint32_t);

        uint64_t required_size = back_buffer_pitch * fb->height;
        if (required_size <= BACKBUFFER_SIZE) {
            back_buffer = (uint32_t *)back_buffer_static;
            back_buffer_initialized = true;
            back_buffer_available = true;
        } else {
            back_buffer = NULL;
            back_buffer_initialized = true;
            back_buffer_available = false;
        }
    }

    // Scroll - use back buffer if available, otherwise direct FB
    if (back_buffer_available) {
        scroll_backbuffer(line_height, bg_color);
        flush_backbuffer(fb);
    } else {
        uint8_t *fb_addr = (uint8_t *)fb->address;
        uint64_t bytes_per_line = line_height * fb->pitch;
        uint64_t total_fb_size = fb->height * fb->pitch;

        // memmove is format-independent as it just moves raw bytes
        memmove(fb_addr, fb_addr + bytes_per_line, total_fb_size - bytes_per_line);

        // Clear bottom line using the new bit-depth independent function
        for (uint64_t y = fb->height - line_height; y < fb->height; y++) for (uint64_t x = 0; x < fb->width; x++) put_pixel_fb(x, y, bg_color);
    }

    cursor_y = fb->height - line_height;
}

void clrscr(void) {
    uint64_t rflags;
    spin_lock_irqsave(&term_lock, &rflags);
    if (!fb_req.response || fb_req.response->framebuffer_count < 1) {
        spin_unlock_irqrestore(&term_lock, rflags);
        return;
    }
    struct limine_framebuffer *fb = fb_req.response->framebuffers[0];

    // Validate framebuffer
    if (!fb->address || fb->width == 0 || fb->height == 0 || fb->width > 8192 || fb->height > 8192) {
        spin_unlock_irqrestore(&term_lock, rflags);
        return;
    }

    // Initialize back buffer if not already done (inline)
    if (!back_buffer_initialized) {
        back_buffer_width = fb->width;
        back_buffer_height = fb->height;
        back_buffer_pitch = fb->width * sizeof(uint32_t);

        uint64_t required_size = back_buffer_pitch * fb->height;
        if (required_size <= BACKBUFFER_SIZE) {
            back_buffer = (uint32_t *)back_buffer_static;
            back_buffer_initialized = true;
            back_buffer_available = true;
        } else {
            back_buffer = NULL;
            back_buffer_initialized = true;
            back_buffer_available = false;
        }
    }

    // Hide cursor before clearing
    if (cursor_visible) show_cursor(false);

    // Clear screen - use back buffer if available, otherwise direct FB
    if (back_buffer_available && back_buffer_initialized && back_buffer) {
        // Clear the back buffer with background color
        uint64_t total_pixels = back_buffer_width * back_buffer_height;
        for (uint64_t i = 0; i < total_pixels; i++) {
            back_buffer[i] = bg_color;
        }
        flush_backbuffer(fb);
    } else {
        // Direct framebuffer clear
        for (uint64_t y = 0; y < fb->height; y++) {
            for (uint64_t x = 0; x < fb->width; x++) {
                put_pixel_fb(x, y, bg_color);
            }
        }
    }

    // Reset cursor position and terminal state
    cursor_x = 0;
    cursor_y = 0;
    line_start_y = 0;
    state = STATE_NORMAL;
    is_bold = false;

    // Show cursor at new position if enabled
    if (cursor_enabled) show_cursor(true);
    spin_unlock_irqrestore(&term_lock, rflags);
}

static void putc_unlocked(char c) {
    if (state == STATE_NORMAL && is_visible_control((unsigned char)c)) {
        putc_unlocked('^');
        putc_unlocked(visible_control_char((unsigned char)c));
        return;
    }

    serial_putc(COM1, c);

    if (!current_font_w || !current_font_h) return;
    if (!fb_req.response || fb_req.response->framebuffer_count < 1) return;

    struct limine_framebuffer *fb = fb_req.response->framebuffers[0];

    // Initialize back buffer if not already done (inline)
    if (!back_buffer_initialized) {
        back_buffer_width = fb->width;
        back_buffer_height = fb->height;
        back_buffer_pitch = fb->width * sizeof(uint32_t);

        uint64_t required_size = back_buffer_pitch * fb->height;
        if (required_size <= BACKBUFFER_SIZE) {
            back_buffer = (uint32_t *)back_buffer_static;
            back_buffer_initialized = true;
            back_buffer_available = true;
        } else {
            back_buffer = NULL;
            back_buffer_initialized = true;
            back_buffer_available = false;
        }
    }

    show_cursor(false);

    if (state == STATE_NORMAL) {
        switch (c) {
            case '\033': state = STATE_EXPECT_BRACKET; break;
            case '\r':   cursor_x = 0; break;
            case '\n':   cursor_x = 0; cursor_y += current_font_h; line_start_y = cursor_y; break;
            case '\t': {
                uint64_t col = cursor_x / current_font_w;
                cursor_x = ((col + 8) & ~7ULL) * current_font_w;
                if (cursor_x >= fb->width) { cursor_x = 0; cursor_y += current_font_h; }
                break;
            }
            case '\b':
                if (cursor_x >= current_font_w) cursor_x -= current_font_w;
                else if (cursor_y > line_start_y) { cursor_y -= current_font_h; cursor_x = fb->width - current_font_w; }
                else break;

                // Clear character - use back buffer if available, otherwise direct FB
                if (back_buffer_available) {
                    fill_rect_backbuffer(cursor_x, cursor_y, current_font_w, current_font_h, bg_color);
                    flush_region_backbuffer(fb, cursor_x, cursor_y, current_font_w, current_font_h);
                } else {
                    for (uint64_t y = 0; y < current_font_h; y++) {
                        for (uint64_t x = 0; x < current_font_w; x++) {
                            put_pixel_fb(cursor_x + x, cursor_y + y, bg_color);
                        }
                    }
                }
                break;
            default:
                // Draw character - use back buffer if available, otherwise direct FB
                if (back_buffer_available) {
                    putc_bb(c, cursor_x, cursor_y, fg_color, bg_color);
                    flush_region_backbuffer(fb, cursor_x, cursor_y, current_font_w, current_font_h);
                } else {
                    putc_fb(c, cursor_x, cursor_y, fg_color, bg_color);
                }

                cursor_x += current_font_w;
                if (cursor_x >= fb->width) { cursor_x = 0; cursor_y += current_font_h; }
                break;
        }
        while (cursor_y + current_font_h > fb->height) scroll(fb);
    } else if (state == STATE_EXPECT_BRACKET) {
        if (c == '[') { ansi_idx = 0; state = STATE_READ_PARAMS; }
        else state = STATE_NORMAL;
    } else if (state == STATE_READ_PARAMS) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
            ansi_buffer[ansi_idx] = '\0';
            if (c == 'm') {
                char *p = ansi_buffer;
                while (*p) {
                    int val = 0;
                    while (*p >= '0' && *p <= '9') val = (val * 10) + (*p++ - '0');

                    if (val == 0) { fg_color = default_color; bg_color = 0; is_bold = false; }
                    else if (val == 1) is_bold = true;
                    else if ((val >= 30 && val <= 37) || (val >= 90 && val <= 97)) fg_color = ansi_to_hex(val, is_bold);
                    else if ((val >= 40 && val <= 47) || (val >= 100 && val <= 107)) bg_color = ansi_to_hex(val, false);
                    else if (val == 38 || val == 48) {
                        bool fg = (val == 38);
                        if (*p == ';') p++;
                        int mode = 0;
                        while (*p >= '0' && *p <= '9') mode = (mode * 10) + (*p++ - '0');

                        if (mode == 5) { // 256 Colors
                            if (*p == ';') p++;
                            int color = 0;
                            while (*p >= '0' && *p <= '9') color = (color * 10) + (*p++ - '0');
                            if (fg) fg_color = ansi_to_hex(color, false); else bg_color = ansi_to_hex(color, false);
                        } else if (mode == 2) { // 24-bit RGB
                            int r = 0, g = 0, b = 0;
                            if (*p == ';') p++; 
                            while (*p >= '0' && *p <= '9') r = (r * 10) + (*p++ - '0');
                            
                            if (*p == ';') p++; 
                            while (*p >= '0' && *p <= '9') g = (g * 10) + (*p++ - '0');
                            
                            if (*p == ';') p++; 
                            while (*p >= '0' && *p <= '9') b = (b * 10) + (*p++ - '0');
                            
                            if (fg) fg_color = rgb_to_hex(r, g, b); else bg_color = rgb_to_hex(r, g, b);
                        }
                    }
                    if (*p == ';') p++; else break;
                }
            } else if (c == 'J') {
                if (ansi_buffer[0] == '2') {
                    // Clear entire screen - use back buffer if available, otherwise direct FB
                    if (back_buffer_available) {
                        fill_rect_backbuffer(0, 0, fb->width, fb->height, bg_color);
                        flush_backbuffer(fb);
                    } else {
                        for (uint64_t y = 0; y < fb->height; y++) {
                            for (uint64_t x = 0; x < fb->width; x++) {
                                put_pixel_fb(x, y, bg_color);
                            }
                        }
                    }
                    cursor_x = 0;
                    cursor_y = 0;
                    line_start_y = 0;
                    is_bold = false;
                }
            } else if (c == 'H') {
                cursor_x = 0;
                cursor_y = 0;
            } else if (c == 's') {
                line_start_y = cursor_y;
            } else if (c == 'h' || c == 'l') {
                if (strcmp(ansi_buffer, "?25") == 0) {
                    cursor_enabled = (c == 'h');
                }
            }
            state = STATE_NORMAL;
        } else if (c == '?') {
            if (ansi_idx < 15) ansi_buffer[ansi_idx++] = c;
        } else if (ansi_idx < 15) ansi_buffer[ansi_idx++] = c;
    }
    if (cursor_enabled) show_cursor(true);
}

void putc(char c) {
    uint64_t rflags;
    spin_lock_irqsave(&term_lock, &rflags);
    putc_unlocked(c);
    spin_unlock_irqrestore(&term_lock, rflags);
}

void puts(const char *s) {
    uint64_t rflags;
    spin_lock_irqsave(&term_lock, &rflags);
    while (*s) { putc_unlocked(*s); s++; }
    spin_unlock_irqrestore(&term_lock, rflags);
}

/* We don't have %f implemented into the kernel vprintf(), because:
   1. We don't use it
   2. We use vprintf() before SSE is enabled */
void vprintf(const char *fmt, va_list args) {
    uint64_t rflags;
    spin_lock_irqsave(&term_lock, &rflags);
    for (const char *p = fmt; *p != '\0'; p++) {
        if (*p != '%') {
            putc_unlocked(*p);
            continue;
        }
        p++;
        if (*p == '\0') { putc_unlocked('%'); break; }
        bool left_align = false;
        int width = 0;
        char pad_char = ' ';
        bool is_long = false;

        // Left-align flag
        if (*p == '-') {
            left_align = true;
            p++;
        }
        // Zero-pad flag (ignored if left-aligning)
        if (*p == '0') {
            if (!left_align) pad_char = '0';
            p++;
        }
        // Width
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            p++;
        }
        // Long modifier
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
                if (!left_align)
                    while (width > len) { putc_unlocked(pad_char); width--; }
    
                while (*s) putc_unlocked(*s++);
    
                if (left_align)
                    while (width > len) { putc_unlocked(' '); width--; }
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
                int base = (*p=='x'||*p=='X') ? 16 : (*p=='o'||*p=='O') ? 8 : 10;
                char buf[64];
                int_to_str(val, buf, 64, base, (*p == 'X'));
                int len = 0;
                while (buf[len]) len++;
                if (is_neg) len++; // account for '-'
                if (!left_align)
                    while (width > len) { putc_unlocked(pad_char); width--; }
                if (is_neg) putc_unlocked('-');
                char *ptr = buf;
                while (*ptr) putc_unlocked(*ptr++);
                if (left_align)
                    while (width > len) { putc_unlocked(' '); width--; }
                break;
            }
            case 'p': {
                uint64_t x = va_arg(args, uint64_t);
                char buf[64];
                int_to_str(x, buf, 64, 16, false);
                putc_unlocked('0'); putc_unlocked('x');
                int len = 0;
                while (buf[len]) len++;
                for (int i = 0; i < (16 - len); i++) putc_unlocked('0');
                char *ptr = buf;
                while (*ptr) putc_unlocked(*ptr++);
                break;
            }
            case 'c':
                putc_unlocked((char)va_arg(args, int));
                break;
            case '%':
                putc_unlocked('%');
                break;
            default:
                putc_unlocked('%');
                putc_unlocked(*p);
                break;
        }
    }
    spin_unlock_irqrestore(&term_lock, rflags);
}

void printf(const char *fmt, ...) {
    // No spinlocks here since vprintf already has spinlocks
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}
