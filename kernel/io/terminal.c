#include <freestanding/stdint.h>
#include <freestanding/stdarg.h>
#include <freestanding/stdio.h>
#include <main/string.h>
#include <main/limine_req.h>
#include <main/spinlocks.h>
#include <main/halt.h>
#include <io/framebuffer.h>
#include <io/fonts.h>
#include <io/serial.h>
#include <io/terminal.h>

// Kept as static
#define BACKBUFFER_SIZE (1920 * 1080 * 4)
#define FONT_PENDING_BUFFER_SIZE 4096

static uint8_t back_buffer_static[BACKBUFFER_SIZE] __attribute__((aligned(4096)));
static uint32_t *back_buffer = NULL;
static uint64_t back_buffer_width = 0;
static uint64_t back_buffer_height = 0;
static uint64_t back_buffer_pitch = 0;
static bool back_buffer_initialized = false;
static bool back_buffer_available = false;
static bool back_buffer_dirty = false;

static parser_state_t state = STATE_NORMAL;
static char ansi_buffer[32];
static int ansi_idx = 0;
static bool is_bold = false;
static bool is_reverse = false;       // SGR 7 (reverse video) — vi status line
static bool cursor_visible = false;
static bool cursor_enabled = true;
static spinlock_t term_lock = SPINLOCK_INIT;

// Saved backbuffer pixels under the cursor, so we can restore them when the
// cursor moves.  The cursor is rendered INTO the backbuffer — that way it
// survives flush_backbuffer / scroll_region_both without flickering.
static uint32_t cursor_saved_pixels[32 * 16]; // max glyph: 32w × 16h
static uint64_t cursor_saved_x = 0;
static uint64_t cursor_saved_y = 0;
static uint64_t cursor_saved_w = 0;
static uint64_t cursor_saved_h = 0;

static bool   region_set = false;
static uint64_t region_top = 0;       // first row (pixels)
static uint64_t region_bottom = 0;    // one-past last row (pixels)

static uint64_t saved_cursor_x = 0;
static uint64_t saved_cursor_y = 0;
static uint32_t saved_fg = 0;
static uint32_t saved_bg = 0;
static bool     saved_bold = false;
static bool     saved_reverse = false;

static bool alt_active = false;
static uint64_t alt_saved_cursor_x = 0;
static uint64_t alt_saved_cursor_y = 0;
static uint32_t alt_saved_fg = 0;
static uint32_t alt_saved_bg = 0;
static bool     alt_saved_bold = false;
static bool     alt_saved_reverse = false;

static char font_pending_buffer[FONT_PENDING_BUFFER_SIZE];
static size_t font_pending_len = 0;
static bool font_pending_overflowed = false;
static bool font_pending_replaying = false;

uint64_t cursor_x = 0;
uint64_t cursor_y = 0;
uint32_t fg_color = 0x00AAAAAA;
uint32_t bg_color = 0x00000000;
uint32_t default_color = 0x00AAAAAA;
uint64_t line_start_y = 0; // Track where current input line started

static void backbuffer_reload_from_fb(struct limine_framebuffer *fb) {
    if (!fb || !fb->address) return;
    if (fb->width != back_buffer_width || fb->height != back_buffer_height) return;

    uint8_t *fb_addr = (uint8_t *)fb->address;
    uint8_t r_size = fb->red_mask_size;
    uint8_t r_shift = fb->red_mask_shift;
    uint8_t g_size = fb->green_mask_size;
    uint8_t g_shift = fb->green_mask_shift;
    uint8_t b_size = fb->blue_mask_size;
    uint8_t b_shift = fb->blue_mask_shift;
    uint8_t bpp = fb->bpp;

    for (uint64_t y = 0; y < back_buffer_height; y++) {
        uint64_t fb_row_offset = y * fb->pitch;
        uint64_t bb_row_offset = y * back_buffer_width;
        for (uint64_t x = 0; x < back_buffer_width; x++) {
            uint64_t fb_offset = fb_row_offset + x * ((bpp + 7) / 8);
            uint32_t pixel = 0;
            switch (bpp) {
                case 15: case 16: pixel = *(uint16_t *)(fb_addr + fb_offset); break;
                case 24:
                    pixel  = (uint32_t)fb_addr[fb_offset + 0];
                    pixel |= (uint32_t)fb_addr[fb_offset + 1] << 8;
                    pixel |= (uint32_t)fb_addr[fb_offset + 2] << 16;
                    break;
                case 32: pixel = *(uint32_t *)(fb_addr + fb_offset); break;
                default: halt(); break;
            }
            uint32_t r = (r_size == 0) ? 0 : ((pixel >> r_shift) & ((1u << r_size) - 1)) * 255 / ((1u << r_size) - 1);
            uint32_t g = (g_size == 0) ? 0 : ((pixel >> g_shift) & ((1u << g_size) - 1)) * 255 / ((1u << g_size) - 1);
            uint32_t b = (b_size == 0) ? 0 : ((pixel >> b_shift) & ((1u << b_size) - 1)) * 255 / ((1u << b_size) - 1);
            back_buffer[bb_row_offset + x] = (r << 16) | (g << 8) | b;
        }
    }
}

static void flush_backbuffer(struct limine_framebuffer *fb) {
    if (!back_buffer_initialized || !back_buffer || !back_buffer_available) return;
    if (!fb || !fb->address) return;

    uint64_t width = fb->width;
    uint64_t height = fb->height;

    // Validate framebuffer dimensions
    if (width == 0 || height == 0 || width > 8192 || height > 8192) return;

    // Lazy re-sync: if /dev/fb0 was written by userspace since we last touched
    // the cached image, the cache is now stale. Pull fresh pixels out of the
    // live framebuffer BEFORE flushing so we don't spray the user's garbage
    // with the pre-write terminal image.
    if (back_buffer_dirty) {
        backbuffer_reload_from_fb(fb);
        back_buffer_dirty = false;
    }

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

    // Lazy resync: if userspace wrote to /dev/fb0 since the last full
    // redraw, the back buffer is stale EVERYWHERE — not just in this rect.
    // Repainting a small region from the cached (pre-corruption) image
    // would bleed old terminal text through the user's garbage. Pull fresh
    // pixels out of the live fb first, then the region push is correct.
    if (back_buffer_dirty) {
        backbuffer_reload_from_fb(fb);
        back_buffer_dirty = false;
    }

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

static void scroll_region_both(int n_lines, uint32_t bg) {
    if (!current_font_h || n_lines == 0) return;
    if (!fb_req.response || fb_req.response->framebuffer_count < 1) return;
    struct limine_framebuffer *fb = fb_req.response->framebuffers[0];
    uint64_t lh = (uint64_t)(n_lines < 0 ? -n_lines : n_lines) * current_font_h;
    if (lh == 0) return;

    uint64_t top = region_set ? region_top : 0;
    uint64_t bot = region_set ? region_bottom : fb->height;
    if (bot <= top || bot > fb->height) { top = 0; bot = fb->height; }
    if (lh >= bot - top) {
        // Whole region cleared
        if (back_buffer_available) fill_rect_backbuffer(0, top, back_buffer_width, bot - top, bg);
        for (uint64_t y = top; y < bot; y++)
            for (uint64_t x = 0; x < fb->width; x++)
                put_pixel_fb(x, y, bg);
        return;
    }

    uint64_t reg_height = bot - top;

    // 1. Scroll the backbuffer (RAM, fast memmove)
    if (back_buffer_available) {
        uint64_t reg_bytes = back_buffer_pitch;
        if (n_lines > 0) {
            memmove(back_buffer + top * back_buffer_width,
                    back_buffer + (top + lh) * back_buffer_width,
                    (reg_height - lh) * reg_bytes);
            fill_rect_backbuffer(0, bot - lh, back_buffer_width, lh, bg);
        } else {
            memmove(back_buffer + (top + lh) * back_buffer_width,
                    back_buffer + top * back_buffer_width,
                    (reg_height - lh) * reg_bytes);
            fill_rect_backbuffer(0, top, back_buffer_width, lh, bg);
        }
    }

    // 2. Scroll the live framebuffer directly (raw byte memmove on VRAM,
    //    much faster than pixel-by-pixel flush_backbuffer).
    if (fb->address) {
        uint8_t *fb_addr = (uint8_t *)fb->address;
        uint64_t pitch = fb->pitch;
        if (n_lines > 0) {
            memmove(fb_addr + top * pitch,
                    fb_addr + (top + lh) * pitch,
                    (reg_height - lh) * pitch);
        } else {
            memmove(fb_addr + (top + lh) * pitch,
                    fb_addr + top * pitch,
                    (reg_height - lh) * pitch);
        }
        // Clear the newly exposed line(s) on the live FB
        uint64_t clear_y = (n_lines > 0) ? (bot - lh) : top;
        for (uint64_t y = clear_y; y < clear_y + lh; y++)
            for (uint64_t x = 0; x < fb->width; x++)
                put_pixel_fb(x, y, bg);
    }
}

static void put_pixel_bb(uint32_t x, uint32_t y, uint32_t color) {
    if (!back_buffer_initialized || !back_buffer || !back_buffer_available) return;
    if (x >= back_buffer_width || y >= back_buffer_height) return;
    back_buffer[y * back_buffer_width + x] = color;
}

static void putchar_bb(char c, int x, int y, uint32_t fg, uint32_t bg) {
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
        case '\033':  // ESC - begins an escape sequence
        case '\r':    // CR
        case '\n':    // LF
        case '\b':    // BS
        case '\t':    // HT
        case '\a':    // BEL - rings the bell, never printed as ^G
        case '\0':    // NUL - ignored
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

static void double_to_str(double val, int precision, char *out, size_t out_size) {
    if (out_size < 2) return;

    uint64_t bits;
    __builtin_memcpy(&bits, &val, 8);

    int sign = (bits >> 63) & 1;
    int exp  = (int)((bits >> 52) & 0x7FF);
    uint64_t mant = bits & 0x000FFFFFFFFFFFFFULL;

    // special cases
    if (exp == 0x7FF) {
        if (mant == 0) {
            size_t i = 0;
            if (sign && i < out_size - 1) out[i++] = '-';
            const char *inf = "inf";
            while (*inf && i < out_size - 1) out[i++] = *inf++;
            out[i] = '\0';
        } else {
            size_t i = 0;
            const char *nan = "nan";
            while (*nan && i < out_size - 1) out[i++] = *nan++;
            out[i] = '\0';
        }
        return;
    }

    size_t pos = 0;
    if (sign && pos < out_size - 1) out[pos++] = '-';

    // reconstruct integer and fractional parts using integer arithmetic.
    // we work with the value as: significand * 2^(exp-1023-52)
    int e = exp - 1023 - 52;
    uint64_t sig = (exp == 0) ? mant : (mant | (1ULL << 52));

    uint64_t int_part;
    uint64_t frac_num, frac_den;

    if (e >= 0) {
        // value is an integer (or too large for frac precision)
        if (e < 63) int_part = sig << e; else int_part = 0xFFFFFFFFFFFFFFFFULL;
        frac_num = 0; frac_den = 1;
    } else if (e > -53) {
        int shift = -e;
        int_part  = sig >> shift;
        frac_num  = sig & ((1ULL << shift) - 1);
        frac_den  = 1ULL << shift;
    } else {
        int_part = 0;
        // value < 1; represent as frac_num/frac_den with capped shift
        int shift = (e < -62) ? 62 : -e;
        frac_num = sig >> (-e - shift);
        frac_den = 1ULL << shift;
    }

    // write integer part
    char ibuf[24]; int ilen = 0;
    if (int_part == 0) {
        ibuf[ilen++] = '0';
    } else {
        uint64_t tmp = int_part;
        while (tmp > 0 && ilen < 23) { ibuf[ilen++] = '0' + (int)(tmp % 10); tmp /= 10; }
        // reverse
        for (int a = 0, b = ilen - 1; a < b; a++, b--) { char t = ibuf[a]; ibuf[a] = ibuf[b]; ibuf[b] = t; }
    }
    for (int i = 0; i < ilen && pos < out_size - 1; i++) out[pos++] = ibuf[i];

    if (precision <= 0) { out[pos] = '\0'; return; }
    if (pos < out_size - 1) out[pos++] = '.';

    // write fractional digits
    for (int d = 0; d < precision && pos < out_size - 1; d++) {
        frac_num *= 10;
        uint64_t digit = frac_num / frac_den;
        frac_num %= frac_den;
        out[pos++] = '0' + (int)digit;
    }
    out[pos] = '\0';
}

void invalidate_terminal_backbuffer(void) { back_buffer_dirty = true; }

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

    if (back_buffer_available) {
        uint64_t cw = current_font_w;
        uint64_t ch = current_font_h;
        // Clamp to screen bounds
        if (cursor_x + cw > back_buffer_width)  cw = back_buffer_width - cursor_x;
        if (cursor_y + ch > back_buffer_height) ch = back_buffer_height - cursor_y;

        if (visible) {
            // Save the backbuffer pixels under the cursor so we can restore later
            cursor_saved_x = cursor_x;
            cursor_saved_y = cursor_y;
            cursor_saved_w = cw;
            cursor_saved_h = ch;
            for (uint64_t row = 0; row < ch; row++) {
                uint64_t off = (cursor_y + row) * back_buffer_width + cursor_x;
                for (uint64_t col = 0; col < cw; col++)
                    cursor_saved_pixels[row * cw + col] = back_buffer[off + col];
            }
            for (uint64_t row = 0; row < ch; row++) {
                uint64_t off = (cursor_y + row) * back_buffer_width + cursor_x;
                for (uint64_t col = 0; col < cw; col++)
                    back_buffer[off + col] ^= 0x00AAAAAAu;
            }
            // Flush just the cursor cell to the live FB
            flush_region_backbuffer(fb, cursor_x, cursor_y, cw, ch);
        } else {
            // Restore the saved pixels into the backbuffer
            if (cursor_saved_w > 0 && cursor_saved_h > 0) {
                for (uint64_t row = 0; row < cursor_saved_h; row++) {
                    uint64_t off = (cursor_saved_y + row) * back_buffer_width + cursor_saved_x;
                    for (uint64_t col = 0; col < cursor_saved_w; col++)
                        back_buffer[off + col] = cursor_saved_pixels[row * cursor_saved_w + col];
                }
                flush_region_backbuffer(fb, cursor_saved_x, cursor_saved_y, cursor_saved_w, cursor_saved_h);
            }
        }
    } else {
        // Fallback: no backbuffer, draw directly to FB
        if (visible) {
            unsigned char *glyph = &current_font[(unsigned char)' ' * current_font_h];
            for (int row_idx = 0; row_idx < current_font_h; row_idx++) {
                unsigned char row_data = glyph[row_idx];
                for (int col_idx = 0; col_idx < current_font_w; col_idx++) {
                    uint32_t color = (row_data & (0x80 >> col_idx)) ? bg_color : fg_color;
                    put_pixel_fb(cursor_x + col_idx, cursor_y + row_idx, color);
                }
            }
        } else {
            putchar_fb(' ', cursor_x, cursor_y, fg_color, bg_color);
        }
    }

    cursor_visible = visible;
}

void scroll(void) {
    if (!fb_req.response || fb_req.response->framebuffer_count < 1) return;
    struct limine_framebuffer *fb = fb_req.response->framebuffers[0];
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

    // Scroll - use direct fb memmove for speed (avoids cursor flicker)
    if (back_buffer_available) {
        scroll_region_both(1, bg_color);
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

static int putchar_unlocked(int c) {
    unsigned char ch = (unsigned char)c;

    if (state == STATE_NORMAL && is_visible_control(ch)) {
        putchar_unlocked('^');
        putchar_unlocked(visible_control_char(ch));
        return EOF;
    }

    if (!font_pending_replaying) serial_putchar(COM1, c);

    if (!current_font_w || !current_font_h) {
        if (!font_pending_replaying) {
            if (font_pending_len < FONT_PENDING_BUFFER_SIZE) {
                font_pending_buffer[font_pending_len++] = ch;
            } else {
                font_pending_overflowed = true;
            }
        }
        return EOF;
    }

    if (font_pending_len > 0 && !font_pending_replaying) {
        font_pending_replaying = true;

        if (font_pending_overflowed) font_pending_overflowed = false;

        for (size_t i = 0; i < font_pending_len; i++) {
            putchar_unlocked(font_pending_buffer[i]);
        }
        font_pending_len = 0;

        font_pending_replaying = false;
    }

    if (!fb_req.response || fb_req.response->framebuffer_count < 1) return EOF;
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
            case '\a':   break;
            case '\r':   cursor_x = 0; break;
            case '\n':   {
                // LF: move down, scroll if past bottom of scroll region.
                uint64_t bot = region_set ? region_bottom : fb->height;
                cursor_x = 0;
                if (cursor_y + current_font_h >= bot) {
                    // At bottom of region: scroll the region up by one line.
                    scroll_region_both(1, bg_color);
                } else {
                    cursor_y += current_font_h;
                }
                line_start_y = cursor_y;
                break;
            }
            case '\t': {
                uint64_t col = cursor_x / current_font_w;
                cursor_x = ((col + 8) & ~7ULL) * current_font_w;
                if (cursor_x >= fb->width) { cursor_x = 0; cursor_y += current_font_h; }
                break;
            }
            case '\b':
                // BS: move cursor one position left (do NOT erase the character)
                if (cursor_x >= current_font_w) cursor_x -= current_font_w;
                else if (cursor_y > line_start_y) { cursor_y -= current_font_h; cursor_x = fb->width - current_font_w; }
                break;
            default: {
                // Draw character - apply reverse video, then render via back buffer or direct FB
                uint32_t eff_fg = is_reverse ? bg_color : fg_color;
                uint32_t eff_bg = is_reverse ? fg_color : bg_color;
                if (back_buffer_available) {
                    putchar_bb(c, cursor_x, cursor_y, eff_fg, eff_bg);
                    flush_region_backbuffer(fb, cursor_x, cursor_y, current_font_w, current_font_h);
                } else {
                    putchar_fb(c, cursor_x, cursor_y, eff_fg, eff_bg);
                }

                cursor_x += current_font_w;
                if (cursor_x >= fb->width) { cursor_x = 0; cursor_y += current_font_h; }
                break;
            }
        }
        while (cursor_y + current_font_h > fb->height) scroll();
    } else if (state == STATE_EXPECT_BRACKET) {
        // After ESC, if it's not '[', handle single-char ESC sequences.
        if (c == '[') {
            ansi_idx = 0; state = STATE_READ_PARAMS;
        } else {
            // ESC 7/8 — DECSC/DECRC (save/restore cursor+attrs). vi uses these.
            // ESC D  — IND (index, line feed w/ scroll region respect)
            // ESC M  — RI  (reverse index — scroll down at top of region)
            // ESC E  — NEL (next line)
            switch (c) {
                case '7':  // DECSC
                    saved_cursor_x = cursor_x;
                    saved_cursor_y = cursor_y;
                    saved_fg = fg_color;
                    saved_bg = bg_color;
                    saved_bold = is_bold;
                    saved_reverse = is_reverse;
                    break;
                case '8':  // DECRC
                    cursor_x   = saved_cursor_x;
                    cursor_y   = saved_cursor_y;
                    fg_color   = saved_fg;
                    bg_color   = saved_bg;
                    is_bold    = saved_bold;
                    is_reverse = saved_reverse;
                    break;
                case 'D':  // IND: cursor down; scroll region if at bottom
                    if (cursor_y + current_font_h >= region_bottom) {
                        scroll_region_both(1, bg_color);
                    } else if (cursor_y + current_font_h < fb->height) {
                        cursor_y += current_font_h;
                    }
                    break;
                case 'M':  // RI: cursor up; scroll region down if at top
                    if (cursor_y < region_top + current_font_h) {
                        scroll_region_both(-1, bg_color);
                    } else if (cursor_y >= current_font_h) {
                        cursor_y -= current_font_h;
                    }
                    break;
                case 'E':  // NEL: CR + LF
                    cursor_x = 0;
                    if (cursor_y + current_font_h >= region_bottom) {
                        scroll_region_both(1, bg_color);
                    } else if (cursor_y + current_font_h < fb->height) {
                        cursor_y += current_font_h;
                    }
                    break;
            }
            state = STATE_NORMAL;
        }
    } else if (state == STATE_READ_PARAMS) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '@') {
            ansi_buffer[ansi_idx] = '\0';

            // Parse integer params up front. Skip a leading '?' (priv modes).
            bool priv_mode = (ansi_buffer[0] == '?');
            int pp[8];
            int npp = 0;
            if (!priv_mode) {
                const char *q = ansi_buffer;
                while (npp < 8 && *q) {
                    int v = 0; bool have = false;
                    while (*q >= '0' && *q <= '9') { v = v*10 + (*q - '0'); have = true; q++; }
                    pp[npp++] = have ? v : 1;
                    if (*q == ';') q++; else break;
                }
            }
            if (c == 'm') {
                char *p = ansi_buffer;
                if (*p == '\0') {
                    // SGR with no params (e.g. \033[m) → reset all (equivalent to \033[0m])
                    fg_color = default_color;
                    bg_color = 0;
                    is_bold = false;
                    is_reverse = false;
                }
                while (*p) {
                    int val = 0;
                    while (*p >= '0' && *p <= '9') val = (val * 10) + (*p++ - '0');

                    if (val == 0) { fg_color = default_color; bg_color = 0; is_bold = false; is_reverse = false; }
                    else if (val == 1) is_bold = true;
                    else if (val == 7) is_reverse = true;
                    else if (val == 27) is_reverse = false;
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
                // ED: Erase in Display
                int param = (ansi_buffer[0] >= '0' && ansi_buffer[0] <= '9') ? (ansi_buffer[0] - '0') : 0;
                if (param == 0) {
                    // Erase from cursor to end of screen
                    if (back_buffer_available) {
                        fill_rect_backbuffer(cursor_x, cursor_y, fb->width - cursor_x, current_font_h, bg_color);
                        if (cursor_y + current_font_h < fb->height)
                            fill_rect_backbuffer(0, cursor_y + current_font_h, fb->width, fb->height - cursor_y - current_font_h, bg_color);
                        flush_backbuffer(fb);
                    }
                } else if (param == 1) {
                    // Erase from start of screen to cursor
                    if (back_buffer_available) {
                        if (cursor_y > 0)
                            fill_rect_backbuffer(0, 0, fb->width, cursor_y, bg_color);
                        fill_rect_backbuffer(0, cursor_y, cursor_x + current_font_w, current_font_h, bg_color);
                        flush_backbuffer(fb);
                    }
                } else if (param == 2 || param == 3) {
                    // Clear entire screen
                    if (back_buffer_available) {
                        fill_rect_backbuffer(0, 0, fb->width, fb->height, bg_color);
                        flush_backbuffer(fb);
                    } else {
                        for (uint64_t y = 0; y < fb->height; y++)
                            for (uint64_t x = 0; x < fb->width; x++)
                                put_pixel_fb(x, y, bg_color);
                    }
                    if (param == 2) {
                        cursor_x = 0;
                        cursor_y = 0;
                        line_start_y = 0;
                    }
                }
            } else if (c == 'K') {
                // EL: Erase in Line
                int param = (npp > 0) ? pp[0] : 0;
                if (back_buffer_available) {
                    if (param == 0) {
                        // Erase from cursor to end of line
                        fill_rect_backbuffer(cursor_x, cursor_y, fb->width - cursor_x, current_font_h, bg_color);
                    } else if (param == 1) {
                        // Erase from start of line to cursor
                        fill_rect_backbuffer(0, cursor_y, cursor_x + current_font_w, current_font_h, bg_color);
                    } else if (param == 2) {
                        // Erase entire line
                        fill_rect_backbuffer(0, cursor_y, fb->width, current_font_h, bg_color);
                    }
                    flush_backbuffer(fb);
                }
            } else if (c == 'H' || c == 'f') {
                // CUP / HVP: Cursor Position  [row;col]
                int row = (npp > 0 && pp[0] > 0) ? pp[0] - 1 : 0;
                int col = (npp > 1 && pp[1] > 0) ? pp[1] - 1 : 0;
                uint64_t max_rows = (region_set ? region_bottom : fb->height) / current_font_h;
                uint64_t max_cols = fb->width / current_font_w;
                if ((uint64_t)row >= max_rows) row = max_rows - 1;
                if ((uint64_t)col >= max_cols) col = max_cols - 1;
                uint64_t base_y = region_set ? region_top : 0;
                cursor_y = base_y + (uint64_t)row * current_font_h;
                cursor_x = (uint64_t)col * current_font_w;
            } else if (c == 'A') {
                // CUU: Cursor Up
                int n = (npp > 0 && pp[0] > 0) ? pp[0] : 1;
                uint64_t top = region_set ? region_top : 0;
                uint64_t dy = (uint64_t)n * current_font_h;
                cursor_y = (cursor_y - top >= dy) ? cursor_y - dy : top;
            } else if (c == 'B') {
                // CUD: Cursor Down
                int n = (npp > 0 && pp[0] > 0) ? pp[0] : 1;
                uint64_t bot = region_set ? region_bottom : fb->height;
                uint64_t dy = (uint64_t)n * current_font_h;
                if (cursor_y + current_font_h + dy <= bot) cursor_y += dy;
                else cursor_y = bot - current_font_h;
            } else if (c == 'C') {
                // CUF: Cursor Forward
                int n = (npp > 0 && pp[0] > 0) ? pp[0] : 1;
                uint64_t dx = (uint64_t)n * current_font_w;
                if (cursor_x + dx <= fb->width - current_font_w) cursor_x += dx;
                else cursor_x = fb->width - current_font_w;
            } else if (c == 'D') {
                // CUB: Cursor Back
                int n = (npp > 0 && pp[0] > 0) ? pp[0] : 1;
                uint64_t dx = (uint64_t)n * current_font_w;
                cursor_x = (cursor_x >= dx) ? cursor_x - dx : 0;
            } else if (c == 'G') {
                // CHA: Cursor Horizontal Absolute (1-based column)
                int col = (npp > 0 && pp[0] > 0) ? pp[0] - 1 : 0;
                uint64_t max_cols = fb->width / current_font_w;
                if ((uint64_t)col >= max_cols) col = max_cols - 1;
                cursor_x = (uint64_t)col * current_font_w;
            } else if (c == 'd') {
                // VPA: Cursor Vertical Absolute (1-based row)
                int row = (npp > 0 && pp[0] > 0) ? pp[0] - 1 : 0;
                uint64_t base_y = region_set ? region_top : 0;
                uint64_t bot    = region_set ? region_bottom : fb->height;
                uint64_t max_rows = (bot - base_y) / current_font_h;
                if ((uint64_t)row >= max_rows) row = max_rows - 1;
                cursor_y = base_y + (uint64_t)row * current_font_h;
            } else if (c == 'L') {
                // IL: Insert Lines (within scroll region)
                int n = (npp > 0 && pp[0] > 0) ? pp[0] : 1;
                if (back_buffer_available) {
                    uint64_t top = region_set ? region_top : 0;
                    uint64_t bot = region_set ? region_bottom : fb->height;
                    uint64_t lh = (uint64_t)n * current_font_h;
                    if (lh < bot - top && cursor_y >= top && cursor_y < bot) {
                        // Shift lines down from cursor_y to bottom
                        uint64_t move_sz = bot - cursor_y - lh;
                        if (move_sz > 0) {
                            memmove(back_buffer + (cursor_y + lh) * back_buffer_width,
                                    back_buffer + cursor_y * back_buffer_width,
                                    move_sz * back_buffer_pitch);
                        }
                        fill_rect_backbuffer(0, cursor_y, back_buffer_width, lh, bg_color);
                        flush_backbuffer(fb);
                    }
                }
            } else if (c == 'M') {
                // DL: Delete Lines (within scroll region)
                int n = (npp > 0 && pp[0] > 0) ? pp[0] : 1;
                if (back_buffer_available) {
                    uint64_t top = region_set ? region_top : 0;
                    uint64_t bot = region_set ? region_bottom : fb->height;
                    uint64_t lh = (uint64_t)n * current_font_h;
                    if (lh < bot - top && cursor_y >= top && cursor_y < bot) {
                        uint64_t move_sz = bot - cursor_y - lh;
                        if (move_sz > 0) {
                            memmove(back_buffer + cursor_y * back_buffer_width,
                                    back_buffer + (cursor_y + lh) * back_buffer_width,
                                    move_sz * back_buffer_pitch);
                        }
                        fill_rect_backbuffer(0, bot - lh, back_buffer_width, lh, bg_color);
                        flush_backbuffer(fb);
                    }
                }
            } else if (c == '@') {
                // ICH: Insert Characters at cursor
                int n = (npp > 0 && pp[0] > 0) ? pp[0] : 1;
                if (back_buffer_available) {
                    uint64_t eol = fb->width;
                    uint64_t ins = (uint64_t)n * current_font_w;
                    if (cursor_x + ins < eol) {
                        uint64_t move_sz = eol - cursor_x - ins;
                        memmove(back_buffer + cursor_y * back_buffer_width + cursor_x + ins,
                                back_buffer + cursor_y * back_buffer_width + cursor_x,
                                move_sz * sizeof(uint32_t));
                    }
                    fill_rect_backbuffer(cursor_x, cursor_y, ins, current_font_h, bg_color);
                    flush_region_backbuffer(fb, cursor_x, cursor_y, eol - cursor_x, current_font_h);
                }
            } else if (c == 'P') {
                // DCH: Delete Characters at cursor
                int n = (npp > 0 && pp[0] > 0) ? pp[0] : 1;
                if (back_buffer_available) {
                    uint64_t eol = fb->width;
                    uint64_t del = (uint64_t)n * current_font_w;
                    if (cursor_x + del < eol) {
                        uint64_t move_sz = eol - cursor_x - del;
                        memmove(back_buffer + cursor_y * back_buffer_width + cursor_x,
                                back_buffer + cursor_y * back_buffer_width + cursor_x + del,
                                move_sz * sizeof(uint32_t));
                    }
                    fill_rect_backbuffer(eol - del, cursor_y, del, current_font_h, bg_color);
                    flush_region_backbuffer(fb, cursor_x, cursor_y, eol - cursor_x, current_font_h);
                }
            } else if (c == 'S') {
                // SU: Scroll Up (entire screen / region)
                int n = (npp > 0 && pp[0] > 0) ? pp[0] : 1;
                scroll_region_both(n, bg_color);
            } else if (c == 'T') {
                // SD: Scroll Down (entire screen / region)
                int n = (npp > 0 && pp[0] > 0) ? pp[0] : 1;
                scroll_region_both(-n, bg_color);
            } else if (c == 'r') {
                // DECSTBM: Set Top and Bottom Margins
                int top = (npp > 0 && pp[0] > 0) ? pp[0] - 1 : 0;
                int bot = (npp > 1 && pp[1] > 0) ? pp[1] - 1 : (int)(fb->height / current_font_h - 1);
                if (top < 0) top = 0;
                if (bot < top) bot = top;
                uint64_t max_rows = fb->height / current_font_h;
                if ((uint64_t)top >= max_rows) top = max_rows - 1;
                if ((uint64_t)bot >= max_rows) bot = max_rows - 1;
                region_set = true;
                region_top = (uint64_t)top * current_font_h;
                region_bottom = ((uint64_t)bot + 1) * current_font_h;
                // Home cursor (VT100 spec: DECSTBM moves cursor home)
                cursor_x = 0;
                cursor_y = region_set ? region_top : 0;
            } else if (c == 's') {
                // SCP: Save Cursor Position (standard CSI s, not DECSC)
                saved_cursor_x = cursor_x;
                saved_cursor_y = cursor_y;
                saved_fg = fg_color;
                saved_bg = bg_color;
                saved_bold = is_bold;
                saved_reverse = is_reverse;
            } else if (c == 'u') {
                // RCP: Restore Cursor Position (standard CSI u, not DECRC)
                cursor_x   = saved_cursor_x;
                cursor_y   = saved_cursor_y;
                fg_color   = saved_fg;
                bg_color   = saved_bg;
                is_bold    = saved_bold;
                is_reverse = saved_reverse;
            } else if (c == 'h' || c == 'l') {
                if (priv_mode) {
                    if (strcmp(ansi_buffer, "?25") == 0) {
                        cursor_enabled = (c == 'h');
                    } else if (strcmp(ansi_buffer, "?1049") == 0 || strcmp(ansi_buffer, "?47") == 0 || strcmp(ansi_buffer, "?1047") == 0) {
                        // Alt screen: save/restore cursor + attrs, clear screen on entry
                        if (c == 'h') {
                            if (!alt_active) {
                                alt_saved_cursor_x = cursor_x;
                                alt_saved_cursor_y = cursor_y;
                                alt_saved_fg = fg_color;
                                alt_saved_bg = bg_color;
                                alt_saved_bold = is_bold;
                                alt_saved_reverse = is_reverse;
                                alt_active = true;
                                if (back_buffer_available) {
                                    fill_rect_backbuffer(0, 0, fb->width, fb->height, bg_color);
                                    flush_backbuffer(fb);
                                }
                                cursor_x = 0;
                                cursor_y = 0;
                            }
                        } else {
                            if (alt_active) {
                                cursor_x   = alt_saved_cursor_x;
                                cursor_y   = alt_saved_cursor_y;
                                fg_color   = alt_saved_fg;
                                bg_color   = alt_saved_bg;
                                is_bold    = alt_saved_bold;
                                is_reverse = alt_saved_reverse;
                                alt_active = false;
                            }
                        }
                    }
                }
            }
            state = STATE_NORMAL;
        } else if (c == '?') {
            if (ansi_idx < 15) ansi_buffer[ansi_idx++] = c;
        } else if (ansi_idx < 15) ansi_buffer[ansi_idx++] = c;
    }
    if (cursor_enabled) show_cursor(true);
    return ch;
}

int putchar(int c) {
    uint64_t rflags;
    spin_lock_irqsave(&term_lock, &rflags);
    int ret = putchar_unlocked(c);
    spin_unlock_irqrestore(&term_lock, rflags);
    return ret;
}

int puts(const char *s) {
    if (!s) return EOF;

    uint64_t rflags;
    spin_lock_irqsave(&term_lock, &rflags);

    while (*s) { putchar_unlocked(*s); s++; }
    putchar_unlocked('\n');

    spin_unlock_irqrestore(&term_lock, rflags);

    return 0;
}

int vprintf(const char *fmt, va_list args) {
    int total_written = 0;
    uint64_t rflags;
    spin_lock_irqsave(&term_lock, &rflags);

    #define PUTC(c) do { putchar_unlocked(c); total_written++; } while(0)

    for (const char *p = fmt; *p != '\0'; p++) {
        if (*p != '%') {
            PUTC(*p);
            continue;
        }
        p++;
        if (*p == '\0') { PUTC('%'); break; }
        bool left_align = false;
        int width = 0;
        char pad_char = ' ';
        bool is_long = false;
        bool is_size = false;

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
        // Size modifier (%z for size_t / ssize_t)
        if (*p == 'z') {
            is_size = true;
            p++;
        }

        switch (*p) {
            case 's': {
                char *s = va_arg(args, char *);
                if (!s) s = "(null)";
                int len = strlen(s);
                if (!left_align)
                    while (width > len) { PUTC(pad_char); width--; }

                while (*s) { PUTC(*s); s++; }

                if (left_align)
                    while (width > len) { PUTC(' '); width--; }
                break;
            }
            case 'o':
            case 'i':
            case 'd':
            case 'u':
            case 'b':
            case 'x': case 'X': {
                uint64_t val;
                if (is_long) {
                    val = va_arg(args, uint64_t);
                } else if (is_size) {
                    val = (uint64_t)va_arg(args, size_t);
                } else {
                    if (*p == 'd' || *p == 'i') val = (uint64_t)va_arg(args, int);
                    else           val = (uint64_t)va_arg(args, unsigned int);
                }
                bool is_neg = (*p == 'd' || *p == 'D' || *p == 'i') && (int64_t)val < 0;
                if (is_neg) val = -(int64_t)val;
                int base = (*p=='x'||*p=='X') ? 16 : (*p=='o'||*p=='O') ? 8 : (*p=='b') ? 2 : 10;
                char buf[64];
                int_to_str(val, buf, 64, base, (*p == 'X'));
                int len = 0;
                while (buf[len]) len++;
                if (is_neg) len++; // account for '-'
                if (!left_align)
                    while (width > len) { PUTC(pad_char); width--; }
                if (is_neg) PUTC('-');
                char *ptr = buf;
                while (*ptr) PUTC(*ptr++);
                if (left_align)
                    while (width > len) { PUTC(' '); width--; }
                break;
            }
            case 'p': {
                uint64_t x = va_arg(args, uint64_t);
                char buf[64];
                int_to_str(x, buf, 64, 16, false);
                PUTC('0'); PUTC('x');
                int len = 0;
                while (buf[len]) len++;
                for (int i = 0; i < (16 - len); i++) PUTC('0');
                char *ptr = buf;
                while (*ptr) PUTC(*ptr++);
                break;
            }
            case 'f': case 'F': {
                double fval = va_arg(args, double);
                int prec = 6;
                char fbuf[64];
                double_to_str(fval, prec, fbuf, sizeof(fbuf));
                int len = 0; while (fbuf[len]) len++;
                if (!left_align)
                    while (width > len) { PUTC(pad_char); width--; }
                char *fp = fbuf; while (*fp) PUTC(*fp++);
                if (left_align)
                    while (width > len) { PUTC(' '); width--; }
                break;
            }
            case 'c':
                PUTC((char)va_arg(args, int));
                break;
            case '%':
                PUTC('%');
                break;
            default:
                PUTC('%');
                PUTC(*p);
                break;
        }
    }
    
    #undef PUTC

    spin_unlock_irqrestore(&term_lock, rflags);
    return total_written;
}

int printf(const char *fmt, ...) {
    // No spinlocks here since vprintf already has spinlocks
    va_list args;
    va_start(args, fmt);
    int ret = vprintf(fmt, args);
    va_end(args);
    return ret;
}
