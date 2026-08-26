#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <signal.h>
#include <main/log.h>
#include <main/string.h>
#include <main/limine_req.h>
#include <main/spinlocks.h>
#include <main/halt.h>
#include <io/fb.h>
#include <io/fonts.h>
#include <io/serial.h>
#include <io/terminal.h>
#include <io/tty.h>
#include <mm/mm.h>

static parser_state_t state = STATE_NORMAL;
static char ansi_buffer[32];
static int ansi_idx = 0;
static bool is_bold = false;
static bool is_reverse = false;
static uint32_t reverse_bg = 0;
static int last_printable_char = ' ';
static bool acs_active = false;
static bool expect_charset_designator = false;
static bool cursor_visible = false;
static bool cursor_enabled = true;
static spinlock_t term_lock = SPINLOCK_INIT;
static uint64_t tab_stops[TAB_STOP_WORDS];
static bool tab_stops_initialized = false;

static uint32_t cursor_saved_pixels[32 * 32]; // max glyph: 32w × 32h
static uint64_t cursor_saved_x = 0;
static uint64_t cursor_saved_y = 0;
static uint64_t cursor_saved_w = 0;
static uint64_t cursor_saved_h = 0;

static bool     region_set = false;
static uint64_t region_top = 0;       // first row (pixels)
static uint64_t region_bottom = 0;    // one-past last row (pixels)

static uint64_t saved_cursor_x = 0;
static uint64_t saved_cursor_y = 0;
static uint32_t saved_fg = 0;
static uint32_t saved_bg = 0;
static bool     saved_bold = false;
static bool     saved_reverse = false;

static bool     alt_active = false;
static uint64_t alt_saved_cursor_x = 0;
static uint64_t alt_saved_cursor_y = 0;
static uint32_t alt_saved_fg = 0;
static uint32_t alt_saved_bg = 0;
static bool     alt_saved_bold = false;
static bool     alt_saved_reverse = false;
static uint32_t *alt_back_buffer = NULL;

static terminal_cell_t *cell_buffer = NULL;
static terminal_cell_t *alt_cell_buffer = NULL;
static uint64_t cell_columns = 0;
static uint64_t cell_rows = 0;
static uint8_t cell_font_w = 0;
static uint8_t cell_font_h = 0;
static uint64_t rendered_font_generation = 0;

static char   font_pending_buffer[FONT_PENDING_BUFFER_SIZE];
static size_t font_pending_len = 0;
static bool   font_pending_overflowed = false;
static bool   font_pending_replaying = false;
static uint32_t fb_batch_depth = 0;
static bool fb_update_pending = false;
static uint64_t fb_update_x = 0;
static uint64_t fb_update_y = 0;
static uint64_t fb_update_right = 0;
static uint64_t fb_update_bottom = 0;
static terminal_vt_t terminal_vts[NUM_TTYS];
static int active_terminal_tty = 1;
static bool terminal_display_enabled = true;

uint32_t *back_buffer = NULL;
uint64_t back_buffer_width = 0;
uint64_t back_buffer_height = 0;
uint64_t back_buffer_pitch = 0;
bool     back_buffer_initialized = false;
bool     back_buffer_available = false;
bool     back_buffer_dirty = false;

uint64_t cursor_x = 0;
uint64_t cursor_y = 0;
uint32_t fg_color = 0x00AAAAAA;
uint32_t bg_color = 0x00000000;
uint32_t default_color = 0x00AAAAAA;
uint64_t line_start_y = 0; // Track where current input line started

static const unsigned char acs_table[] = {
    0x20, 0x04, 0xB1, 0x20, 0x0C, 0x0D, 0x0A, 0xF8,
    0xF1, 0x20, 0x20, 0xD9, 0xBF, 0xDA, 0xC0, 0xC5,
    0xC4, 0xC4, 0xC4, 0xC4, 0xC4, 0xC3, 0xB4, 0xC1,
    0xC2, 0xB3, 0xF3, 0xF2, 0xE3, 0xF7, 0x9C, 0xFA,
};

static inline unsigned char acs_translate(unsigned char c) {
    if (acs_active && c >= 0x5F && c <= 0x7E) return acs_table[c - 0x5F];
    return c;
}

static void reset_tab_stops(void) {
    memset(tab_stops, 0, sizeof(tab_stops));
    for (uint64_t column = 0; column < TERMINAL_MAX_COLUMNS; column += 8) {
        tab_stops[column / TAB_STOP_WORD_BITS] |= 1ULL << (column % TAB_STOP_WORD_BITS);
    }
    tab_stops_initialized = true;
}

static void set_tab_stop(uint64_t column) {
    if (!tab_stops_initialized) reset_tab_stops();
    if (column >= TERMINAL_MAX_COLUMNS) return;
    tab_stops[column / TAB_STOP_WORD_BITS] |= 1ULL << (column % TAB_STOP_WORD_BITS);
}

static void clear_all_tab_stops(void) {
    memset(tab_stops, 0, sizeof(tab_stops));
    tab_stops_initialized = true;
}

static uint64_t next_tab_stop(uint64_t column, uint64_t columns) {
    if (!tab_stops_initialized) reset_tab_stops();
    if (!columns) return 0;

    uint64_t limit = columns < TERMINAL_MAX_COLUMNS ? columns : TERMINAL_MAX_COLUMNS;
    for (uint64_t next = column + 1; next < limit; next++) {
        if (tab_stops[next / TAB_STOP_WORD_BITS] & (1ULL << (next % TAB_STOP_WORD_BITS))) return next;
    }
    return columns - 1;
}

static void backbuffer_reload_from_fb(struct limine_framebuffer *fb);
static bool init_terminal_vt(terminal_vt_t *vt);
static void reset_terminal_vt(terminal_vt_t *vt);
static void save_terminal_vt(int tty_idx);
static void load_terminal_vt(int tty_idx);
static void select_terminal_vt(int tty_idx, bool display);

static void begin_fb_batch(void) { fb_batch_depth++; }

static void update_terminal_fb(uint64_t x, uint64_t y, uint64_t width, uint64_t height) {
    if (!terminal_display_enabled) return;
    if (!fb_batch_depth) { (void)update_fb(x, y, width, height); return; }
    uint64_t right = x + width;
    uint64_t bottom = y + height;
    if (!fb_update_pending) {
        fb_update_x = x;
        fb_update_y = y;
        fb_update_right = right;
        fb_update_bottom = bottom;
        fb_update_pending = true;
        return;
    }
    if (x < fb_update_x) fb_update_x = x;
    if (y < fb_update_y) fb_update_y = y;
    if (right > fb_update_right) fb_update_right = right;
    if (bottom > fb_update_bottom) fb_update_bottom = bottom;
}

static void end_fb_batch(void) {
    if (!fb_batch_depth || --fb_batch_depth || !fb_update_pending) return;
    if (!terminal_display_enabled) { fb_update_pending = false; return; }
    (void)update_fb(fb_update_x, fb_update_y, fb_update_right - fb_update_x, fb_update_bottom - fb_update_y);
    fb_update_pending = false;
}

static uint64_t resize_first_row(uint64_t old_rows, uint64_t new_rows, uint64_t cursor_row) {
    if (new_rows >= old_rows || cursor_row <= new_rows) return 0;
    if (old_rows - cursor_row < new_rows) return old_rows - new_rows;
    return cursor_row - new_rows / 2;
}

static void copy_resized_cells(uint32_t *dst, uint64_t new_width, uint64_t new_height, const uint32_t *src, uint64_t old_width, uint64_t old_height, uint64_t first_row) {
    if (!dst || !src || !current_font_w || !current_font_h) return;

    uint64_t old_cols = old_width / current_font_w;
    uint64_t old_rows = old_height / current_font_h;
    uint64_t new_cols = new_width / current_font_w;
    uint64_t new_rows = new_height / current_font_h;
    uint64_t copy_cols = old_cols < new_cols ? old_cols : new_cols;
    uint64_t available_rows = first_row < old_rows ? old_rows - first_row : 0;
    uint64_t copy_rows = available_rows < new_rows ? available_rows : new_rows;
    uint64_t copy_width = copy_cols * current_font_w;

    for (uint64_t cell_row = 0; cell_row < copy_rows; cell_row++) {
        uint64_t src_y = (first_row + cell_row) * current_font_h;
        uint64_t dst_y = cell_row * current_font_h;
        for (uint64_t glyph_row = 0; glyph_row < (uint64_t)current_font_h; glyph_row++) {
            memcpy(dst + (dst_y + glyph_row) * new_width, src + (src_y + glyph_row) * old_width, copy_width * sizeof(uint32_t));
        }
    }
}

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
        uint64_t backbuffer_row_offset = y * back_buffer_width;
        for (uint64_t x = 0; x < back_buffer_width; x++) {
            uint64_t fb_offset = fb_row_offset + x * ((bpp + 7) / 8);

            uint32_t pixel = 0;
            switch (bpp) {
                case 15:
                case 16: pixel = *(uint16_t *)(fb_addr + fb_offset); break;
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
            back_buffer[backbuffer_row_offset + x] = (r << 16) | (g << 8) | b;
        }
    }
}

static inline bool fb_format_matches_backbuffer(struct limine_framebuffer *fb) {
    return fb->bpp == 32 &&
           fb->red_mask_size   == 8 && fb->red_mask_shift   == 16 &&
           fb->green_mask_size == 8 && fb->green_mask_shift == 8  &&
           fb->blue_mask_size  == 8 && fb->blue_mask_shift  == 0;
}

static void flush_backbuffer(struct limine_framebuffer *fb) {
    if (!terminal_display_enabled) return;
    if (!back_buffer_initialized || !back_buffer || !back_buffer_available) return;
    if (!fb || !fb->address) return;

    uint64_t width = fb->width;
    uint64_t height = fb->height;

    // Validate framebuffer dimensions
    if (width == 0 || height == 0 || width > 8192 || height > 8192) return;
    if (width != back_buffer_width || height != back_buffer_height) return;

    if (back_buffer_dirty) {
        backbuffer_reload_from_fb(fb);
        back_buffer_dirty = false;
    }

    uint8_t *fb_addr = (uint8_t *)fb->address;
    uint64_t bpp = fb->bpp;

    // Fast path: 32bpp XRGB — backbuffer IS native pixel format, just memcpy.
    if (fb_format_matches_backbuffer(fb)) {
        if (fb->pitch == back_buffer_pitch) {
            memcpy(fb_addr, back_buffer, back_buffer_pitch * back_buffer_height);
        } else {
            for (uint64_t y = 0; y < back_buffer_height; y++) {
                memcpy(fb_addr + y * fb->pitch,
                       back_buffer + y * back_buffer_width,
                       back_buffer_pitch);
            }
        }
        update_terminal_fb(0, 0, back_buffer_width, back_buffer_height);
        return;
    }

    uint8_t r_size = fb->red_mask_size;
    uint8_t r_shift = fb->red_mask_shift;
    uint8_t g_size = fb->green_mask_size;
    uint8_t g_shift = fb->green_mask_shift;
    uint8_t b_size = fb->blue_mask_size;
    uint8_t b_shift = fb->blue_mask_shift;

    for (uint64_t y = 0; y < height; y++) {
        uint64_t fb_row_offset = y * fb->pitch;
        uint64_t backbuffer_row_offset = y * width;

        for (uint64_t x = 0; x < width; x++) {
            uint32_t color = back_buffer[backbuffer_row_offset + x];
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
    update_terminal_fb(0, 0, width, height);
}

static void flush_region_backbuffer(struct limine_framebuffer *fb, uint64_t x, uint64_t y, uint64_t w, uint64_t h) {
    if (!terminal_display_enabled) return;
    if (!back_buffer_initialized || !back_buffer || !back_buffer_available) return;
    if (!fb || !fb->address) return;
    if (x >= fb->width || y >= fb->height) return;
    if (w == 0 || h == 0 || w > 8192 || h > 8192) return;

    if (back_buffer_dirty) {
        backbuffer_reload_from_fb(fb);
        back_buffer_dirty = false;
    }

    // Clamp region to framebuffer bounds
    if (x + w > fb->width) w = fb->width - x;
    if (y + h > fb->height) h = fb->height - y;

    uint8_t *fb_addr = (uint8_t *)fb->address;
    uint64_t bpp = fb->bpp;

    // Fast path: 32bpp XRGB — memcpy each row slice directly.
    if (fb_format_matches_backbuffer(fb)) {
        for (uint64_t row = 0; row < h; row++) {
            memcpy(fb_addr + (y + row) * fb->pitch + x * sizeof(uint32_t),
                   back_buffer + (y + row) * back_buffer_width + x,
                   w * sizeof(uint32_t));
        }
        update_terminal_fb(x, y, w, h);
        return;
    }

    uint8_t r_size = fb->red_mask_size;
    uint8_t r_shift = fb->red_mask_shift;
    uint8_t g_size = fb->green_mask_size;
    uint8_t g_shift = fb->green_mask_shift;
    uint8_t b_size = fb->blue_mask_size;
    uint8_t b_shift = fb->blue_mask_shift;

    for (uint64_t row = 0; row < h; row++) {
        uint64_t fb_row_offset = (y + row) * fb->pitch;
        uint64_t backbuffer_row_offset = (y + row) * back_buffer_width;

        for (uint64_t col = 0; col < w; col++) {
            uint32_t color = back_buffer[backbuffer_row_offset + (x + col)];
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
    update_terminal_fb(x, y, w, h);
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

static void blank_cells(terminal_cell_t *cells, uint64_t count, uint32_t background) {
    if (!cells) return;
    for (uint64_t i = 0; i < count; i++) {
        cells[i].character = ' ';
        cells[i].foreground = default_color;
        cells[i].background = background;
    }
}

static bool init_terminal_vt(terminal_vt_t *vt) {
    if (!vt || !back_buffer_width || !back_buffer_height ||
        back_buffer_width > 8192 || back_buffer_height > 8192 ||
        back_buffer_width > UINT64_MAX / back_buffer_height) return false;
    uint64_t pixel_count = back_buffer_width * back_buffer_height;
    if (cell_columns && cell_rows > UINT64_MAX / cell_columns) return false;
    uint64_t cell_count = cell_columns * cell_rows;
    if (!pixel_count || pixel_count > UINT64_MAX / sizeof(uint32_t) ||
        (cell_count && cell_count > UINT64_MAX / sizeof(terminal_cell_t))) return false;
    memset(vt, 0, sizeof(*vt));
    vt->back_buffer = (uint32_t *)malloc(pixel_count * sizeof(uint32_t));
    vt->alt_back_buffer = (uint32_t *)malloc(pixel_count * sizeof(uint32_t));
    vt->cell_buffer = cell_count ? (terminal_cell_t *)malloc(cell_count * sizeof(terminal_cell_t)) : NULL;
    vt->alt_cell_buffer = cell_count ? (terminal_cell_t *)malloc(cell_count * sizeof(terminal_cell_t)) : NULL;
    if (!vt->back_buffer || !vt->alt_back_buffer || (cell_count && (!vt->cell_buffer || !vt->alt_cell_buffer))) {
        reset_terminal_vt(vt);
        return false;
    }
    for (uint64_t i = 0; i < pixel_count; i++) { vt->back_buffer[i] = 0; vt->alt_back_buffer[i] = 0; }
    blank_cells(vt->cell_buffer, cell_count, 0);
    blank_cells(vt->alt_cell_buffer, cell_count, 0);
    vt->state = STATE_NORMAL;
    vt->last_printable_char = ' ';
    vt->cursor_enabled = true;
    vt->fg_color = 0x00AAAAAA;
    vt->default_color = 0x00AAAAAA;
    vt->initialized = true;
    return true;
}

static void reset_terminal_vt(terminal_vt_t *vt) {
    free(vt->back_buffer);
    free(vt->alt_back_buffer);
    free(vt->cell_buffer);
    free(vt->alt_cell_buffer);
    memset(vt, 0, sizeof(*vt));
}

static void save_terminal_vt(int tty_idx) {
    terminal_vt_t *vt = &terminal_vts[tty_idx];
    vt->state = state;
    memcpy(vt->ansi_buffer, ansi_buffer, sizeof(ansi_buffer));
    vt->ansi_idx = ansi_idx;
    vt->is_bold = is_bold; vt->is_reverse = is_reverse; vt->reverse_bg = reverse_bg;
    vt->last_printable_char = last_printable_char; vt->acs_active = acs_active; vt->expect_charset_designator = expect_charset_designator;
    vt->cursor_visible = cursor_visible; vt->cursor_enabled = cursor_enabled;
    memcpy(vt->tab_stops, tab_stops, sizeof(tab_stops)); vt->tab_stops_initialized = tab_stops_initialized;
    memcpy(vt->cursor_saved_pixels, cursor_saved_pixels, sizeof(cursor_saved_pixels));
    vt->cursor_saved_x = cursor_saved_x; vt->cursor_saved_y = cursor_saved_y; vt->cursor_saved_w = cursor_saved_w; vt->cursor_saved_h = cursor_saved_h;
    vt->region_set = region_set; vt->region_top = region_top; vt->region_bottom = region_bottom;
    vt->saved_cursor_x = saved_cursor_x; vt->saved_cursor_y = saved_cursor_y; vt->saved_fg = saved_fg; vt->saved_bg = saved_bg;
    vt->saved_bold = saved_bold; vt->saved_reverse = saved_reverse;
    vt->alt_active = alt_active; vt->alt_saved_cursor_x = alt_saved_cursor_x; vt->alt_saved_cursor_y = alt_saved_cursor_y;
    vt->alt_saved_fg = alt_saved_fg; vt->alt_saved_bg = alt_saved_bg; vt->alt_saved_bold = alt_saved_bold; vt->alt_saved_reverse = alt_saved_reverse;
    vt->back_buffer = back_buffer; vt->alt_back_buffer = alt_back_buffer; vt->cell_buffer = cell_buffer; vt->alt_cell_buffer = alt_cell_buffer;
    vt->cursor_x = cursor_x; vt->cursor_y = cursor_y; vt->fg_color = fg_color; vt->bg_color = bg_color; vt->default_color = default_color; vt->line_start_y = line_start_y;
    vt->initialized = true;
}

static void load_terminal_vt(int tty_idx) {
    terminal_vt_t *vt = &terminal_vts[tty_idx];
    state = vt->state;
    memcpy(ansi_buffer, vt->ansi_buffer, sizeof(ansi_buffer));
    ansi_idx = vt->ansi_idx;
    is_bold = vt->is_bold; is_reverse = vt->is_reverse; reverse_bg = vt->reverse_bg;
    last_printable_char = vt->last_printable_char; acs_active = vt->acs_active; expect_charset_designator = vt->expect_charset_designator;
    cursor_visible = vt->cursor_visible; cursor_enabled = vt->cursor_enabled;
    memcpy(tab_stops, vt->tab_stops, sizeof(tab_stops)); tab_stops_initialized = vt->tab_stops_initialized;
    memcpy(cursor_saved_pixels, vt->cursor_saved_pixels, sizeof(cursor_saved_pixels));
    cursor_saved_x = vt->cursor_saved_x; cursor_saved_y = vt->cursor_saved_y; cursor_saved_w = vt->cursor_saved_w; cursor_saved_h = vt->cursor_saved_h;
    region_set = vt->region_set; region_top = vt->region_top; region_bottom = vt->region_bottom;
    saved_cursor_x = vt->saved_cursor_x; saved_cursor_y = vt->saved_cursor_y; saved_fg = vt->saved_fg; saved_bg = vt->saved_bg;
    saved_bold = vt->saved_bold; saved_reverse = vt->saved_reverse;
    alt_active = vt->alt_active; alt_saved_cursor_x = vt->alt_saved_cursor_x; alt_saved_cursor_y = vt->alt_saved_cursor_y;
    alt_saved_fg = vt->alt_saved_fg; alt_saved_bg = vt->alt_saved_bg; alt_saved_bold = vt->alt_saved_bold; alt_saved_reverse = vt->alt_saved_reverse;
    back_buffer = vt->back_buffer; alt_back_buffer = vt->alt_back_buffer; cell_buffer = vt->cell_buffer; alt_cell_buffer = vt->alt_cell_buffer;
    cursor_x = vt->cursor_x; cursor_y = vt->cursor_y; fg_color = vt->fg_color; bg_color = vt->bg_color; default_color = vt->default_color; line_start_y = vt->line_start_y;
}

// Replay the current cell_buffer to serial so the serial console shows
// what is actually on screen after a VT switch.
static void serial_replay_screen(void) {
    if (!cell_buffer || !cell_columns || !cell_rows) return;

    // Clear the serial terminal and move cursor to home
    serial_puts(COM1, "\033[2J\033[H");

    for (uint64_t row = 0; row < cell_rows; row++) {
        // Find the last non-empty cell in this row to avoid trailing spaces
        uint64_t last_col = 0;
        for (uint64_t col = 0; col < cell_columns; col++) {
            terminal_cell_t *cell = &cell_buffer[row * cell_columns + col];
            if (cell->character && cell->character != ' ') last_col = col + 1;
        }

        for (uint64_t col = 0; col < last_col; col++) {
            terminal_cell_t *cell = &cell_buffer[row * cell_columns + col];
            unsigned char ch = cell->character;
            if (ch < 0x20 || ch == 0x7F) ch = ' '; // sanitize control chars
            serial_putchar(COM1, ch);
        }
        serial_putchar(COM1, '\n');
    }
}

static void select_terminal_vt(int tty_idx, bool display) {
    if (tty_idx < 0 || tty_idx >= NUM_TTYS) return;
    if (!terminal_vts[tty_idx].initialized && !init_terminal_vt(&terminal_vts[tty_idx])) return;
    uint64_t flags;
    spin_lock_irqsave(&term_lock, &flags);
    bool actually_switched = (active_terminal_tty != tty_idx);
    if (actually_switched) {
        // Only hide/show cursor on the live framebuffer when we are actually
        // switching what the user sees. For background-TTY writes (display=false)
        // we must NOT touch the cursor on screen — that is what caused the flash.
        if (display && cursor_visible) show_cursor(false);
        save_terminal_vt(active_terminal_tty);
        load_terminal_vt(tty_idx);
        active_terminal_tty = tty_idx;
    }
    if (display && fb_req.response && fb_req.response->framebuffer_count > 0) {
        flush_backbuffer(fb_req.response->framebuffers[0]);
        if (cursor_enabled) show_cursor(true);
    }
    spin_unlock_irqrestore(&term_lock, flags);
}

static void clear_cell_range(uint64_t row, uint64_t first, uint64_t end, uint32_t background) {
    if (!cell_buffer || row >= cell_rows || first >= cell_columns) return;
    if (end > cell_columns) end = cell_columns;
    if (end <= first) return;
    blank_cells(cell_buffer + row * cell_columns + first, end - first, background);
}

static void clear_cell_rows(uint64_t first, uint64_t end, uint32_t background) {
    if (!cell_buffer || first >= cell_rows) return;
    if (end > cell_rows) end = cell_rows;
    if (end <= first) return;
    blank_cells(cell_buffer + first * cell_columns, (end - first) * cell_columns, background);
}

static void scroll_cell_region(int n_lines, uint32_t background) {
    if (!cell_buffer || !cell_columns || !cell_rows || n_lines == 0 || !current_font_h) return;
    uint64_t top = region_set ? region_top / current_font_h : 0;
    uint64_t bottom = region_set ? region_bottom / current_font_h : cell_rows;
    if (bottom > cell_rows || bottom <= top) { top = 0; bottom = cell_rows; }
    uint64_t count = n_lines < 0 ? (uint64_t)-n_lines : (uint64_t)n_lines;
    uint64_t height = bottom - top;
    if (count >= height) {
        clear_cell_rows(top, bottom, background);
        return;
    }
    uint64_t move_cells = (height - count) * cell_columns;
    if (n_lines > 0) {
        memmove(cell_buffer + top * cell_columns, cell_buffer + (top + count) * cell_columns, move_cells * sizeof(terminal_cell_t));
        clear_cell_rows(bottom - count, bottom, background);
    } else {
        memmove(cell_buffer + (top + count) * cell_columns, cell_buffer + top * cell_columns, move_cells * sizeof(terminal_cell_t));
        clear_cell_rows(top, top + count, background);
    }
}

static void insert_cell_lines(uint64_t row, uint64_t bottom, uint64_t count, uint32_t background) {
    if (!cell_buffer || row >= cell_rows) return;
    if (bottom > cell_rows) bottom = cell_rows;
    if (bottom <= row) return;
    if (count >= bottom - row) { clear_cell_rows(row, bottom, background); return; }
    memmove(cell_buffer + (row + count) * cell_columns, cell_buffer + row * cell_columns, (bottom - row - count) * cell_columns * sizeof(terminal_cell_t));
    clear_cell_rows(row, row + count, background);
}

static void delete_cell_lines(uint64_t row, uint64_t bottom, uint64_t count, uint32_t background) {
    if (!cell_buffer || row >= cell_rows) return;
    if (bottom > cell_rows) bottom = cell_rows;
    if (bottom <= row) return;
    if (count >= bottom - row) { clear_cell_rows(row, bottom, background); return; }
    memmove(cell_buffer + row * cell_columns, cell_buffer + (row + count) * cell_columns, (bottom - row - count) * cell_columns * sizeof(terminal_cell_t));
    clear_cell_rows(bottom - count, bottom, background);
}

static void insert_cells(uint64_t row, uint64_t column, uint64_t count, uint32_t background) {
    if (!cell_buffer || row >= cell_rows || column >= cell_columns) return;
    if (count >= cell_columns - column) { clear_cell_range(row, column, cell_columns, background); return; }
    terminal_cell_t *line = cell_buffer + row * cell_columns;
    memmove(line + column + count, line + column, (cell_columns - column - count) * sizeof(terminal_cell_t));
    blank_cells(line + column, count, background);
}

static void delete_cells(uint64_t row, uint64_t column, uint64_t count, uint32_t background) {
    if (!cell_buffer || row >= cell_rows || column >= cell_columns) return;
    if (count >= cell_columns - column) { clear_cell_range(row, column, cell_columns, background); return; }
    terminal_cell_t *line = cell_buffer + row * cell_columns;
    memmove(line + column, line + column + count, (cell_columns - column - count) * sizeof(terminal_cell_t));
    blank_cells(line + cell_columns - count, count, background);
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
    uint64_t reg_height = bot - top;
    scroll_cell_region(n_lines, bg);
    if (lh >= reg_height) {
        // Whole region cleared
        if (back_buffer_available) {
            fill_rect_backbuffer(0, top, back_buffer_width, bot - top, bg);
            flush_backbuffer(fb);
        } else {
            for (uint64_t y = top; y < bot; y++)
                for (uint64_t x = 0; x < fb->width; x++)
                    put_pixel_fb(x, y, bg);
        }
        return;
    }

    if (back_buffer_available) {
        // Fast path: only touch the backbuffer (RAM), then flush to VRAM
        // in one shot via flush_backbuffer (memcpy for 32bpp XRGB).
        // This avoids the catastrophic VRAM memmove that was here before.
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
        flush_backbuffer(fb);
    } else {
        // No backbuffer: direct VRAM operations (legacy path)
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
        for (uint64_t y = (n_lines > 0) ? (bot - lh) : top;
             y < ((n_lines > 0) ? bot : (top + lh)); y++)
            for (uint64_t x = 0; x < fb->width; x++)
                put_pixel_fb(x, y, bg);
    }
}

static void put_pixel_backbuffer(uint32_t x, uint32_t y, uint32_t color) {
    if (!back_buffer_initialized || !back_buffer || !back_buffer_available) return;
    if (x >= back_buffer_width || y >= back_buffer_height) return;
    back_buffer[y * back_buffer_width + x] = color;
}

static void putchar_backbuffer(char c, int x, int y, uint32_t fg, uint32_t bg) {
    if (!current_font_w || !current_font_h) return;
    if (!back_buffer_initialized || !back_buffer || !back_buffer_available) return;

    unsigned char *glyph = &current_font[(unsigned char)c * current_font_h];

    for (int row = 0; row < current_font_h; row++) {
        unsigned char row_data = glyph[row];
        for (int col = 0; col < current_font_w; col++) {
            if (row_data & (0x80 >> col)) {
                put_pixel_backbuffer(x + col, y + row, fg);
            } else {
                put_pixel_backbuffer(x + col, y + row, bg);
            }
        }
    }
}

static void render_cells_to_buffer(uint32_t *pixels, uint64_t width, uint64_t height, const terminal_cell_t *cells, uint64_t columns, uint64_t rows, uint32_t outside_background) {
    if (!pixels || !cells || !current_font_w || !current_font_h) return;
    uint64_t total = width * height;
    for (uint64_t i = 0; i < total; i++) pixels[i] = outside_background;
    for (uint64_t row = 0; row < rows; row++) {
        for (uint64_t col = 0; col < columns; col++) {
            const terminal_cell_t *cell = &cells[row * columns + col];
            const unsigned char *glyph = &current_font[cell->character * current_font_h];
            uint64_t x = col * current_font_w;
            uint64_t y = row * current_font_h;
            for (uint64_t gy = 0; gy < current_font_h && y + gy < height; gy++) {
                unsigned char bits = glyph[gy];
                for (uint64_t gx = 0; gx < current_font_w && x + gx < width; gx++) {
                    pixels[(y + gy) * width + x + gx] = (bits & (0x80 >> gx)) ? cell->foreground : cell->background;
                }
            }
        }
    }
}

static void copy_cell_grid(terminal_cell_t *destination, uint64_t new_columns, uint64_t new_rows, const terminal_cell_t *source, uint64_t old_columns, uint64_t old_rows, uint64_t first_row) {
    if (!destination || !source || first_row >= old_rows) return;
    uint64_t columns = old_columns < new_columns ? old_columns : new_columns;
    uint64_t available_rows = old_rows - first_row;
    uint64_t rows = available_rows < new_rows ? available_rows : new_rows;
    for (uint64_t row = 0; row < rows; row++) memcpy(destination + row * new_columns, source + (first_row + row) * old_columns, columns * sizeof(terminal_cell_t));
}

static uint64_t remap_cell_position(uint64_t position, uint8_t old_size, uint8_t new_size, uint64_t first, uint64_t limit) {
    uint64_t cell = old_size ? position / old_size : 0;
    cell = cell >= first ? cell - first : 0;
    if (cell >= limit) cell = limit ? limit - 1 : 0;
    return cell * new_size;
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

static int putchar_unlocked(int c) {
    unsigned char ch = (unsigned char)c;

    if (ch == '\0' || ch == 0x7F || ch >= 0x80) return 0;
    if (ch < 0x20) {
        switch (ch) {
            case '\a':
            case '\b':
            case '\t':
            case '\n':
            case '\v':
            case '\f':
            case '\r':
            case '\x0E':
            case '\x0F':
            case '\033':
                break;
            default:
                return 0;
        }
    }

    if (!font_pending_replaying) serial_putchar(COM1, ch);

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

    show_cursor(false);

    // Consume the byte following ESC( — picks the G0 charset.
    if (expect_charset_designator) {
        expect_charset_designator = false;
        if (c == '0') acs_active = true;       // DEC Special Graphics
        else if (c == 'B') acs_active = false; // US ASCII
        if (cursor_enabled) show_cursor(true);
        return ch; // designator byte itself is never drawn
    }

    if (state == STATE_NORMAL) {
        switch (c) {
            case '\a':   break;
            case '\033': state = STATE_EXPECT_BRACKET; break;
            case '\r':   cursor_x = 0; break;
            case '\n':
            case '\v':
            case '\f':   {
                // LF: move down, scroll if past bottom of scroll region.
                // ONLCR: also move to column 0 (CR) on output.
                cursor_x = 0;
                uint64_t bot = region_set ? region_bottom : fb->height;
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
                uint64_t columns = fb->width / current_font_w;
                cursor_x = next_tab_stop(col, columns) * current_font_w;
                break;
            }
            case '\b':
                // BS: move cursor one position left (do NOT erase the character)
                if (cursor_x >= current_font_w) cursor_x -= current_font_w;
                else if (cursor_y > line_start_y) { cursor_y -= current_font_h; cursor_x = fb->width - current_font_w; }
                break;
            default: {
                unsigned char draw_c = acs_translate((unsigned char)c);
                uint32_t eff_fg = is_reverse ? bg_color : fg_color;
                uint32_t eff_bg = is_reverse ? fg_color : bg_color;
                uint64_t cell_col = cursor_x / current_font_w;
                uint64_t cell_row = cursor_y / current_font_h;
                if (cell_buffer && cell_col < cell_columns && cell_row < cell_rows) {
                    terminal_cell_t *cell = &cell_buffer[cell_row * cell_columns + cell_col];
                    cell->character = draw_c;
                    cell->foreground = eff_fg;
                    cell->background = eff_bg;
                }
                if (back_buffer_available) {
                    putchar_backbuffer(draw_c, cursor_x, cursor_y, eff_fg, eff_bg);
                    flush_region_backbuffer(fb, cursor_x, cursor_y, current_font_w, current_font_h);
                } else {
                    putchar_fb(draw_c, cursor_x, cursor_y, eff_fg, eff_bg);
                }
                last_printable_char = draw_c;

                cursor_x += current_font_w;
                if (cursor_x >= fb->width) { cursor_x = 0; cursor_y += current_font_h; }
                break;
            }
            case '\x0E': acs_active = true;  break; // Shift Out -> enter ACS
            case '\x0F': acs_active = false; break; // Shift In  -> leave ACS
        }
        while (cursor_y + current_font_h > fb->height) scroll();
    } else if (state == STATE_EXPECT_BRACKET) {
        // After ESC, if it's not '[', handle single-char ESC sequences.
        if (c == '[') {
            ansi_idx = 0; state = STATE_READ_PARAMS;
        } else if (c == '(') {
            // ESC ( X — select G0 charset. Next byte picks the charset:
            // '0' = DEC Special Graphics (line drawing), 'B' = US ASCII.
            expect_charset_designator = true;
            state = STATE_NORMAL; // consume the designator byte via the flag below
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
                {
                    uint64_t bot = region_set ? region_bottom : fb->height;
                    if (cursor_y + current_font_h >= bot) {
                        scroll_region_both(1, bg_color);
                    } else {
                        cursor_y += current_font_h;
                    }
                    break;
                }
                case 'M':  // RI: cursor up; scroll region down if at top
                {
                    uint64_t top = region_set ? region_top : 0;
                    if (cursor_y < top + current_font_h) {
                        scroll_region_both(-1, bg_color);
                    } else if (cursor_y >= current_font_h) {
                        cursor_y -= current_font_h;
                    }
                    break;
                }
                case 'E':  // NEL: CR + LF
                {
                    uint64_t bot = region_set ? region_bottom : fb->height;
                    cursor_x = 0;
                    if (cursor_y + current_font_h >= bot) {
                        scroll_region_both(1, bg_color);
                    } else {
                        cursor_y += current_font_h;
                    }
                    break;
                }
                case 'H':  // HTS: set a horizontal tab stop at the cursor
                    set_tab_stop(cursor_x / current_font_w);
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
                int pending_fg_idx = -1, pending_bg_idx = -1;
                bool pending_fg_set = false, pending_bg_set = false;
                if (*p == '\0') {
                    // SGR with no params (e.g. \033[m) → reset all (equivalent to \033[0m))
                    fg_color = default_color;
                    is_bold = false;
                    is_reverse = false;
                }
                while (*p) {
                    int val = 0;
                    while (*p >= '0' && *p <= '9') val = (val * 10) + (*p++ - '0');

                    if (val == 0) {
                        fg_color = default_color;
                        bg_color = 0x00000000;
                        is_bold = false;
                        is_reverse = false;
                    } else if (val == 1) is_bold = true;
                    else if (val == 2) is_bold = false;  // dim, treat as non-bold
                    else if (val == 3) {}                // italic, no font support, ignore
                    else if (val == 4) {}                // underline, no font support, ignore
                    else if (val == 7) { reverse_bg = bg_color; is_reverse = true; }
                    else if (val == 22) is_bold = false; // SGR 22: normal intensity (off bold & dim)
                    else if (val == 23) {}               // not italic
                    else if (val == 24) {}               // not underlined
                    else if (val == 25) {}               // not blinking
                    else if (val == 27) { is_reverse = false; bg_color = fg_color; }
                    else if (val == 39) fg_color = default_color;  // SGR 39: default foreground
                    else if (val == 49) {}              // SGR 49: default background - don't reset (matches xterm behavior for 256-color bg)
                    else if ((val >= 30 && val <= 37) || (val >= 90 && val <= 97)) { pending_fg_idx = val; pending_fg_set = true; }
                    else if ((val >= 40 && val <= 47) || (val >= 100 && val <= 107)) { pending_bg_idx = val; pending_bg_set = true; }
                    else if (val == 38 || val == 48) {
                        bool fg = (val == 38);
                        if (*p == ';') p++;
                        int mode = 0;
                        while (*p >= '0' && *p <= '9') mode = (mode * 10) + (*p++ - '0');

                        if (mode == 5) { // 256 Colors
                            if (*p == ';') p++;
                            int color = 0;
                            while (*p >= '0' && *p <= '9') color = (color * 10) + (*p++ - '0');
                            if (fg) fg_color = ansi_to_hex(color, false); else { bg_color = ansi_to_hex(color, false); if (is_reverse) reverse_bg = bg_color; }
                        } else if (mode == 2) { // 24-bit RGB
                            int r = 0, g = 0, b = 0;
                            if (*p == ';') p++; 
                            while (*p >= '0' && *p <= '9') r = (r * 10) + (*p++ - '0');
                            
                            if (*p == ';') p++;
                            while (*p >= '0' && *p <= '9') g = (g * 10) + (*p++ - '0');
                            
                            if (*p == ';') p++;
                            while (*p >= '0' && *p <= '9') b = (b * 10) + (*p++ - '0');
                            
                            if (fg) fg_color = rgb_to_hex(r, g, b); else { bg_color = rgb_to_hex(r, g, b); if (is_reverse) reverse_bg = bg_color; }
                        }
                    }
                    if (pending_fg_set) fg_color = ansi_to_hex(pending_fg_idx, is_bold);
                    if (pending_bg_set) bg_color = ansi_to_hex(pending_bg_idx, false);
                    if (*p == ';') p++; else break;
                }
            } else if (c == 'J') {
                // ED: Erase in Display
                int param = (ansi_buffer[0] >= '0' && ansi_buffer[0] <= '9') ? (ansi_buffer[0] - '0') : 0;
                uint32_t erase_color = is_reverse ? fg_color : bg_color;
                if (param == 0) {
                    uint64_t row = cursor_y / current_font_h;
                    clear_cell_range(row, cursor_x / current_font_w, cell_columns, erase_color);
                    clear_cell_rows(row + 1, cell_rows, erase_color);
                    // Erase from cursor to end of screen
                    if (back_buffer_available) {
                        fill_rect_backbuffer(cursor_x, cursor_y, fb->width - cursor_x, current_font_h, erase_color);
                        if (cursor_y + current_font_h < fb->height)
                            fill_rect_backbuffer(0, cursor_y + current_font_h, fb->width, fb->height - cursor_y - current_font_h, erase_color);
                        flush_backbuffer(fb);
                    } else {
                        for (uint64_t y = cursor_y; y < cursor_y + current_font_h && y < fb->height; y++)
                            for (uint64_t x = cursor_x; x < fb->width; x++) put_pixel_fb(x, y, erase_color);
                        for (uint64_t y = cursor_y + current_font_h; y < fb->height; y++)
                            for (uint64_t x = 0; x < fb->width; x++) put_pixel_fb(x, y, erase_color);
                    }
                } else if (param == 1) {
                    uint64_t row = cursor_y / current_font_h;
                    clear_cell_rows(0, row, erase_color);
                    clear_cell_range(row, 0, cursor_x / current_font_w + 1, erase_color);
                    // Erase from start of screen to cursor
                    if (back_buffer_available) {
                        if (cursor_y > 0)
                            fill_rect_backbuffer(0, 0, fb->width, cursor_y, erase_color);
                        fill_rect_backbuffer(0, cursor_y, cursor_x + current_font_w, current_font_h, erase_color);
                        flush_backbuffer(fb);
                    } else {
                        for (uint64_t y = 0; y < cursor_y && y < fb->height; y++)
                            for (uint64_t x = 0; x < fb->width; x++) put_pixel_fb(x, y, erase_color);
                        for (uint64_t y = cursor_y; y < cursor_y + current_font_h && y < fb->height; y++)
                            for (uint64_t x = 0; x < cursor_x + current_font_w && x < fb->width; x++) put_pixel_fb(x, y, erase_color);
                    }
                } else if (param == 2 || param == 3) {
                    clear_cell_rows(0, cell_rows, erase_color);
                    // Clear entire screen
                    if (back_buffer_available) {
                        fill_rect_backbuffer(0, 0, fb->width, fb->height, erase_color);
                        flush_backbuffer(fb);
                    } else {
                        for (uint64_t y = 0; y < fb->height; y++)
                            for (uint64_t x = 0; x < fb->width; x++)
                                put_pixel_fb(x, y, erase_color);
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
                uint32_t erase_color = is_reverse ? fg_color : bg_color;
                uint64_t row = cursor_y / current_font_h;
                uint64_t col = cursor_x / current_font_w;
                if (param == 0) clear_cell_range(row, col, cell_columns, erase_color);
                else if (param == 1) clear_cell_range(row, 0, col + 1, erase_color);
                else if (param == 2) clear_cell_range(row, 0, cell_columns, erase_color);
                if (back_buffer_available) {
                    if (param == 0) {
                        // Erase from cursor to end of line
                        fill_rect_backbuffer(cursor_x, cursor_y, fb->width - cursor_x, current_font_h, erase_color);
                    } else if (param == 1) {
                        // Erase from start of line to cursor
                        fill_rect_backbuffer(0, cursor_y, cursor_x + current_font_w, current_font_h, erase_color);
                    } else if (param == 2) {
                        // Erase entire line
                        fill_rect_backbuffer(0, cursor_y, fb->width, current_font_h, erase_color);
                    }
                    flush_backbuffer(fb);
                } else {
                    if (param == 0) {
                        for (uint64_t y = cursor_y; y < cursor_y + current_font_h && y < fb->height; y++)
                            for (uint64_t x = cursor_x; x < fb->width; x++) put_pixel_fb(x, y, erase_color);
                    } else if (param == 1) {
                        for (uint64_t y = cursor_y; y < cursor_y + current_font_h && y < fb->height; y++)
                            for (uint64_t x = 0; x < cursor_x + current_font_w && x < fb->width; x++) put_pixel_fb(x, y, erase_color);
                    } else if (param == 2) {
                        for (uint64_t y = cursor_y; y < cursor_y + current_font_h && y < fb->height; y++)
                            for (uint64_t x = 0; x < fb->width; x++) put_pixel_fb(x, y, erase_color);
                    }
                }
            } else if (c == 'H' || c == 'f') {
                // CUP / HVP: Cursor Position  [row;col]
                int row = (npp > 0 && pp[0] > 0) ? pp[0] - 1 : 0;
                int col = (npp > 1 && pp[1] > 0) ? pp[1] - 1 : 0;
                uint64_t max_rows = fb->height / current_font_h;
                uint64_t max_cols = fb->width / current_font_w;
                if ((uint64_t)row >= max_rows) row = max_rows - 1;
                if ((uint64_t)col >= max_cols) col = max_cols - 1;
                cursor_y = (uint64_t)row * current_font_h;
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
                uint64_t max_rows = fb->height / current_font_h;
                if ((uint64_t)row >= max_rows) row = max_rows - 1;
                cursor_y = (uint64_t)row * current_font_h;
            } else if (c == 'L') {
                // IL: Insert Lines (within scroll region)
                int n = (npp > 0 && pp[0] > 0) ? pp[0] : 1;
                uint64_t cell_top = region_set ? region_top / current_font_h : 0;
                uint64_t cell_bottom = region_set ? region_bottom / current_font_h : cell_rows;
                uint64_t cell_row = cursor_y / current_font_h;
                if (cell_row >= cell_top && cell_row < cell_bottom) insert_cell_lines(cell_row, cell_bottom, (uint64_t)n, bg_color);
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
                } else {
                    uint64_t top = region_set ? region_top : 0;
                    uint64_t bot = region_set ? region_bottom : fb->height;
                    uint64_t lh = (uint64_t)n * current_font_h;
                    if (lh < bot - top && cursor_y >= top && cursor_y < bot) {
                        uint64_t move_bytes = (bot - cursor_y - lh) * fb->pitch;
                        if (move_bytes > 0) {
                            memmove((uint8_t *)fb->address + (cursor_y + lh) * fb->pitch,
                                    (uint8_t *)fb->address + cursor_y * fb->pitch,
                                    move_bytes);
                        }
                        for (uint64_t y = cursor_y; y < cursor_y + lh && y < fb->height; y++)
                            for (uint64_t x = 0; x < fb->width; x++) put_pixel_fb(x, y, bg_color);
                    }
                }
            } else if (c == 'M') {
                // DL: Delete Lines (within scroll region)
                int n = (npp > 0 && pp[0] > 0) ? pp[0] : 1;
                uint64_t cell_top = region_set ? region_top / current_font_h : 0;
                uint64_t cell_bottom = region_set ? region_bottom / current_font_h : cell_rows;
                uint64_t cell_row = cursor_y / current_font_h;
                if (cell_row >= cell_top && cell_row < cell_bottom) delete_cell_lines(cell_row, cell_bottom, (uint64_t)n, bg_color);
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
                } else {
                    uint64_t top = region_set ? region_top : 0;
                    uint64_t bot = region_set ? region_bottom : fb->height;
                    uint64_t lh = (uint64_t)n * current_font_h;
                    if (lh < bot - top && cursor_y >= top && cursor_y < bot) {
                        uint64_t move_bytes = (bot - cursor_y - lh) * fb->pitch;
                        if (move_bytes > 0) {
                            memmove((uint8_t *)fb->address + cursor_y * fb->pitch,
                                    (uint8_t *)fb->address + (cursor_y + lh) * fb->pitch,
                                    move_bytes);
                        }
                        for (uint64_t y = bot - lh; y < bot && y < fb->height; y++)
                            for (uint64_t x = 0; x < fb->width; x++) put_pixel_fb(x, y, bg_color);
                    }
                }
            } else if (c == '@') {
                // ICH: Insert Characters at cursor
                int n = (npp > 0 && pp[0] > 0) ? pp[0] : 1;
                insert_cells(cursor_y / current_font_h, cursor_x / current_font_w, (uint64_t)n, bg_color);
                if (back_buffer_available) {
                    uint64_t eol = fb->width;
                    uint64_t ins = (uint64_t)n * current_font_w;
                    if (cursor_x + ins < eol) {
                        uint64_t move_sz = eol - cursor_x - ins;
                        for (uint64_t y = cursor_y; y < cursor_y + current_font_h && y < fb->height; y++) {
                            memmove(back_buffer + y * back_buffer_width + cursor_x + ins,
                                    back_buffer + y * back_buffer_width + cursor_x,
                                    move_sz * sizeof(uint32_t));
                        }
                    }
                    fill_rect_backbuffer(cursor_x, cursor_y, ins, current_font_h, bg_color);
                    flush_region_backbuffer(fb, cursor_x, cursor_y, eol - cursor_x, current_font_h);
                } else {
                    uint64_t eol = fb->width;
                    uint64_t ins = (uint64_t)n * current_font_w;
                    if (cursor_x + ins < eol) {
                        uint64_t move_pixels = eol - cursor_x - ins;
                        uint64_t bpp_bytes = (fb->bpp + 7) / 8;
                        for (uint64_t y = cursor_y; y < cursor_y + current_font_h && y < fb->height; y++) {
                            memmove((uint8_t *)fb->address + y * fb->pitch + (cursor_x + ins) * bpp_bytes,
                                    (uint8_t *)fb->address + y * fb->pitch + cursor_x * bpp_bytes,
                                    move_pixels * bpp_bytes);
                        }
                    }
                    for (uint64_t y = cursor_y; y < cursor_y + current_font_h && y < fb->height; y++)
                        for (uint64_t x = cursor_x; x < cursor_x + ins && x < fb->width; x++) put_pixel_fb(x, y, bg_color);
                }
            } else if (c == 'P') {
                // DCH: Delete Characters at cursor
                int n = (npp > 0 && pp[0] > 0) ? pp[0] : 1;
                delete_cells(cursor_y / current_font_h, cursor_x / current_font_w, (uint64_t)n, bg_color);
                if (back_buffer_available) {
                    uint64_t eol = fb->width;
                    uint64_t del = (uint64_t)n * current_font_w;
                    if (cursor_x + del < eol) {
                        uint64_t move_sz = eol - cursor_x - del;
                        for (uint64_t y = cursor_y; y < cursor_y + current_font_h && y < fb->height; y++) {
                            memmove(back_buffer + y * back_buffer_width + cursor_x,
                                    back_buffer + y * back_buffer_width + cursor_x + del,
                                    move_sz * sizeof(uint32_t));
                        }
                    }
                    fill_rect_backbuffer(eol - del, cursor_y, del, current_font_h, bg_color);
                    flush_region_backbuffer(fb, cursor_x, cursor_y, eol - cursor_x, current_font_h);
                } else {
                    uint64_t eol = fb->width;
                    uint64_t del = (uint64_t)n * current_font_w;
                    if (cursor_x + del < eol) {
                        uint64_t move_pixels = eol - cursor_x - del;
                        uint64_t bpp_bytes = (fb->bpp + 7) / 8;
                        for (uint64_t y = cursor_y; y < cursor_y + current_font_h && y < fb->height; y++) {
                            memmove((uint8_t *)fb->address + y * fb->pitch + cursor_x * bpp_bytes,
                                    (uint8_t *)fb->address + y * fb->pitch + (cursor_x + del) * bpp_bytes,
                                    move_pixels * bpp_bytes);
                        }
                    }
                    for (uint64_t y = cursor_y; y < cursor_y + current_font_h && y < fb->height; y++)
                        for (uint64_t x = (eol > del ? eol - del : 0); x < eol; x++) put_pixel_fb(x, y, bg_color);
                }
            } else if (c == 'b') {
                // REP: Repeat the last printable character n times.
                int n = (npp > 0 && pp[0] > 0) ? pp[0] : 1;
                if (last_printable_char != 0) {
                    state = STATE_NORMAL;
                    for (int i = 0; i < n; i++) {
                        putchar_unlocked(last_printable_char);
                    }
                }
            } else if (c == 'S') {
                // SU: Scroll Up (entire screen / region)
                int n = (npp > 0 && pp[0] > 0) ? pp[0] : 1;
                scroll_region_both(n, bg_color);
            } else if (c == 'T') {
                // SD: Scroll Down (entire screen / region)
                int n = (npp > 0 && pp[0] > 0) ? pp[0] : 1;
                scroll_region_both(-n, bg_color);
            } else if (c == 'g') {
                // Match the Linux virtual console: CSI 0 g sets a stop at the
                // current column, while CSI 3 g clears every tab stop.
                int param = npp > 0 ? pp[0] : 0;
                if (param == 0) set_tab_stop(cursor_x / current_font_w);
                else if (param == 3) clear_all_tab_stops();
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
                                if (cell_buffer && alt_cell_buffer) {
                                    memcpy(alt_cell_buffer, cell_buffer, cell_columns * cell_rows * sizeof(terminal_cell_t));
                                    blank_cells(cell_buffer, cell_columns * cell_rows, bg_color);
                                }
                                if (back_buffer_available && alt_back_buffer) {
                                    size_t backbuffer_size = back_buffer_width * back_buffer_height * sizeof(uint32_t);
                                    memcpy(alt_back_buffer, back_buffer, backbuffer_size);
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
                                // Don't leak the app's DECSTBM scroll region into the main screen
                                region_set = false;
                                region_top = 0;
                                region_bottom = 0;
                                if (cell_buffer && alt_cell_buffer) memcpy(cell_buffer, alt_cell_buffer, cell_columns * cell_rows * sizeof(terminal_cell_t));
                                if (back_buffer_available && alt_back_buffer) {
                                    size_t backbuffer_size = back_buffer_width * back_buffer_height * sizeof(uint32_t);
                                    memcpy(back_buffer, alt_back_buffer, backbuffer_size);
                                    flush_backbuffer(fb);
                                }
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

void sync_terminal(void) {
    if (!fb_req.response || fb_req.response->framebuffer_count < 1) return;
    struct limine_framebuffer *fb = fb_req.response->framebuffers[0];
    bool framebuffer_resized = back_buffer_initialized && (fb->width != back_buffer_width || fb->height != back_buffer_height);
    bool font_changed = back_buffer_initialized && current_font_generation != rendered_font_generation;
    bool resized = framebuffer_resized || font_changed;
    uint8_t old_font_w = cell_font_w;
    uint8_t old_font_h = cell_font_h;

    uint32_t *prepared_back_buffer = NULL;
    uint32_t *prepared_alt_back_buffer = NULL;
    terminal_cell_t *prepared_cell_buffer = NULL;
    terminal_cell_t *prepared_alt_cell_buffer = NULL;
    uint64_t prepared_columns = current_font_w ? fb->width / current_font_w : 0;
    uint64_t prepared_rows = current_font_h ? fb->height / current_font_h : 0;
    if (resized) {
        uint64_t required_size = fb->width * sizeof(uint32_t) * fb->height;
        prepared_back_buffer = (uint32_t *)malloc(required_size);
        prepared_alt_back_buffer = (uint32_t *)malloc(required_size);
        uint64_t required_cell_size = prepared_columns * prepared_rows * sizeof(terminal_cell_t);
        prepared_cell_buffer = required_cell_size ? (terminal_cell_t *)malloc(required_cell_size) : NULL;
        prepared_alt_cell_buffer = required_cell_size ? (terminal_cell_t *)malloc(required_cell_size) : NULL;
        if (prepared_cell_buffer && prepared_alt_cell_buffer) {
            blank_cells(prepared_cell_buffer, prepared_columns * prepared_rows, bg_color);
            blank_cells(prepared_alt_cell_buffer, prepared_columns * prepared_rows, alt_saved_bg);
        } else {
            free(prepared_cell_buffer);
            free(prepared_alt_cell_buffer);
            prepared_cell_buffer = NULL;
            prepared_alt_cell_buffer = NULL;
        }
        if (prepared_back_buffer && prepared_alt_back_buffer) {
            uint64_t total_pixels = fb->width * fb->height;
            for (uint64_t i = 0; i < total_pixels; i++) {
                prepared_back_buffer[i] = bg_color;
                prepared_alt_back_buffer[i] = bg_color;
            }
        } else {
            free(prepared_back_buffer);
            free(prepared_alt_back_buffer);
            prepared_back_buffer = NULL;
            prepared_alt_back_buffer = NULL;
        }
    }

    if (resized) {
        for (int i = 0; i < NUM_TTYS; i++) {
            if (i != active_terminal_tty) reset_terminal_vt(&terminal_vts[i]);
        }
    }

    uint64_t rflags;
    spin_lock_irqsave(&term_lock, &rflags);

    uint32_t *old_back_buffer = NULL;
    uint32_t *old_alt_back_buffer = NULL;
    terminal_cell_t *old_cell_buffer = NULL;
    terminal_cell_t *old_alt_cell_buffer = NULL;
    if (resized) {
        uint64_t old_width = back_buffer_width;
        uint64_t old_height = back_buffer_height;
        uint64_t old_rows = cell_rows;
        uint64_t new_rows = prepared_rows;
        uint64_t cursor_row = old_font_h ? cursor_y / old_font_h : 0;
        uint64_t first_row = resize_first_row(old_rows, new_rows, cursor_row);
        uint64_t alt_cursor_row = old_font_h ? alt_saved_cursor_y / old_font_h : 0;
        uint64_t alt_first_row = alt_active ? resize_first_row(old_rows, new_rows, alt_cursor_row) : first_row;
        uint64_t new_pitch = fb->width * sizeof(uint32_t);
        uint32_t *new_back_buffer = prepared_back_buffer;
        uint32_t *new_alt_back_buffer = prepared_alt_back_buffer;

        if (prepared_cell_buffer && prepared_alt_cell_buffer) {
            copy_cell_grid(prepared_cell_buffer, prepared_columns, prepared_rows, cell_buffer, cell_columns, cell_rows, first_row);
            copy_cell_grid(prepared_alt_cell_buffer, prepared_columns, prepared_rows, alt_cell_buffer, cell_columns, cell_rows, alt_first_row);
            old_cell_buffer = cell_buffer;
            old_alt_cell_buffer = alt_cell_buffer;
            cell_buffer = prepared_cell_buffer;
            alt_cell_buffer = prepared_alt_cell_buffer;
            prepared_cell_buffer = NULL;
            prepared_alt_cell_buffer = NULL;
            cell_columns = prepared_columns;
            cell_rows = prepared_rows;
        }

        if (new_back_buffer && new_alt_back_buffer) {
            if (back_buffer_available && back_buffer && alt_back_buffer) {
                if (cursor_visible && cursor_saved_w > 0 && cursor_saved_h > 0 && cursor_saved_x < old_width && cursor_saved_y < old_height) {
                    uint64_t restore_width = cursor_saved_w;
                    uint64_t restore_height = cursor_saved_h;
                    if (restore_width > old_width - cursor_saved_x) restore_width = old_width - cursor_saved_x;
                    if (restore_height > old_height - cursor_saved_y) restore_height = old_height - cursor_saved_y;
                    for (uint64_t row = 0; row < restore_height; row++) {
                        uint64_t offset = (cursor_saved_y + row) * old_width + cursor_saved_x;
                        for (uint64_t column = 0; column < restore_width; column++) back_buffer[offset + column] = cursor_saved_pixels[row * cursor_saved_w + column];
                    }
                }

                if (!font_changed && current_font_w && current_font_h) {
                    copy_resized_cells(new_back_buffer, fb->width, fb->height, back_buffer, old_width, old_height, first_row);
                    copy_resized_cells(new_alt_back_buffer, fb->width, fb->height, alt_back_buffer, old_width, old_height, alt_first_row);
                } else {
                    uint64_t copy_width = old_width < fb->width ? old_width : fb->width;
                    uint64_t copy_height = old_height < fb->height ? old_height : fb->height;
                    for (uint64_t row = 0; row < copy_height; row++) {
                        memcpy(new_back_buffer + row * fb->width, back_buffer + row * old_width, copy_width * sizeof(uint32_t));
                        memcpy(new_alt_back_buffer + row * fb->width, alt_back_buffer + row * old_width, copy_width * sizeof(uint32_t));
                    }
                }
            }

            old_back_buffer = back_buffer;
            old_alt_back_buffer = alt_back_buffer;
            back_buffer = new_back_buffer;
            alt_back_buffer = new_alt_back_buffer;
            prepared_back_buffer = NULL;
            prepared_alt_back_buffer = NULL;
            back_buffer_width = fb->width;
            back_buffer_height = fb->height;
            back_buffer_pitch = new_pitch;
            back_buffer_available = true;
            back_buffer_dirty = false;
        } else {
            old_back_buffer = back_buffer;
            old_alt_back_buffer = alt_back_buffer;
            back_buffer = NULL;
            alt_back_buffer = NULL;
            back_buffer_width = fb->width;
            back_buffer_height = fb->height;
            back_buffer_pitch = new_pitch;
            back_buffer_available = false;
        }

        if (font_changed) {
            cursor_x = remap_cell_position(cursor_x, old_font_w, current_font_w, 0, prepared_columns);
            cursor_y = remap_cell_position(cursor_y, old_font_h, current_font_h, first_row, prepared_rows);
            line_start_y = remap_cell_position(line_start_y, old_font_h, current_font_h, first_row, prepared_rows);
            saved_cursor_x = remap_cell_position(saved_cursor_x, old_font_w, current_font_w, 0, prepared_columns);
            saved_cursor_y = remap_cell_position(saved_cursor_y, old_font_h, current_font_h, first_row, prepared_rows);
            alt_saved_cursor_x = remap_cell_position(alt_saved_cursor_x, old_font_w, current_font_w, 0, prepared_columns);
            alt_saved_cursor_y = remap_cell_position(alt_saved_cursor_y, old_font_h, current_font_h, alt_first_row, prepared_rows);
        } else {
            uint64_t row_shift = first_row * current_font_h;
            uint64_t alt_row_shift = alt_first_row * current_font_h;
            cursor_y = cursor_y >= row_shift ? cursor_y - row_shift : 0;
            line_start_y = line_start_y >= row_shift ? line_start_y - row_shift : 0;
            saved_cursor_y = saved_cursor_y >= row_shift ? saved_cursor_y - row_shift : 0;
            alt_saved_cursor_y = alt_saved_cursor_y >= alt_row_shift ? alt_saved_cursor_y - alt_row_shift : 0;
        }

        uint64_t max_x = fb->width > (uint64_t)current_font_w ? fb->width - current_font_w : 0;
        uint64_t max_y = fb->height > (uint64_t)current_font_h ? fb->height - current_font_h : 0;
        if (cursor_x > max_x) cursor_x = max_x;
        if (cursor_y > max_y) cursor_y = max_y;
        if (line_start_y > max_y) line_start_y = max_y;
        if (saved_cursor_x > max_x) saved_cursor_x = max_x;
        if (saved_cursor_y > max_y) saved_cursor_y = max_y;
        if (alt_saved_cursor_x > max_x) alt_saved_cursor_x = max_x;
        if (alt_saved_cursor_y > max_y) alt_saved_cursor_y = max_y;
        cursor_visible = false;
        cursor_saved_w = 0;
        cursor_saved_h = 0;
        region_set = false;
        region_top = 0;
        region_bottom = 0;
        cell_font_w = current_font_w;
        cell_font_h = current_font_h;
        if (font_changed && back_buffer_available && back_buffer) {
            render_cells_to_buffer(back_buffer, back_buffer_width, back_buffer_height, cell_buffer, cell_columns, cell_rows, bg_color);
            if (alt_back_buffer) render_cells_to_buffer(alt_back_buffer, back_buffer_width, back_buffer_height, alt_cell_buffer, cell_columns, cell_rows, alt_saved_bg);
        } else if (font_changed) {
            for (uint64_t y = 0; y < fb->height; y++) for (uint64_t x = 0; x < fb->width; x++) put_pixel_fb(x, y, bg_color);
            for (uint64_t row = 0; row < cell_rows; row++) for (uint64_t col = 0; col < cell_columns; col++) {
                terminal_cell_t *cell = &cell_buffer[row * cell_columns + col];
                putchar_fb(cell->character, col * current_font_w, row * current_font_h, cell->foreground, cell->background);
            }
        }
        rendered_font_generation = current_font_generation;
    }

    flush_backbuffer(fb);
    if (font_changed && cursor_enabled) show_cursor(true);
    save_terminal_vt(active_terminal_tty);
    spin_unlock_irqrestore(&term_lock, rflags);
    free(old_back_buffer);
    free(old_alt_back_buffer);
    free(old_cell_buffer);
    free(old_alt_cell_buffer);
    free(prepared_back_buffer);
    free(prepared_alt_back_buffer);
    free(prepared_cell_buffer);
    free(prepared_alt_cell_buffer);
    if (framebuffer_resized || (font_changed && (old_font_w != current_font_w || old_font_h != current_font_h))) {
        for (int i = 0; i < NUM_TTYS; i++) signal_tty_pgrp(i, SIGWINCH);
    }
}

void show_cursor(bool visible) {
    if (!current_font_w || !current_font_h) return;
    if (cursor_visible == visible) return;
    if (!fb_req.response || fb_req.response->framebuffer_count < 1) return;
    struct limine_framebuffer *fb = fb_req.response->framebuffers[0];

    if (back_buffer_available) {
        uint64_t cw = current_font_w;
        uint64_t ch = current_font_h;
        // Clamp to screen bounds
        if (cursor_x + cw > back_buffer_width)  cw = back_buffer_width - cursor_x;
        if (cursor_y + ch > back_buffer_height) ch = back_buffer_height - cursor_y;

        if (visible) {
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
        uint8_t *fb_addr = (uint8_t *)fb->address;
        uint8_t bpp = fb->bpp;
        uint8_t bpp_bytes = (bpp + 7) / 8;
        for (uint64_t row = 0; row < current_font_h; row++) {
            uint64_t fb_row = (cursor_y + row) * fb->pitch + cursor_x * bpp_bytes;
            for (uint64_t col = 0; col < current_font_w; col++) {
                uint64_t off = fb_row + col * bpp_bytes;
                switch (bpp) {
                    case 15:
                    case 16: *(uint16_t *)(fb_addr + off) ^= 0xAD55u; break;
                    case 24: fb_addr[off] ^= 0xAA; fb_addr[off + 1] ^= 0xAA; fb_addr[off + 2] ^= 0xAA; break;
                    case 32: *(uint32_t *)(fb_addr + off) ^= 0x00AAAAAAu; break;
                }
            }
        }
    }

    cursor_visible = visible;
}

void scroll(void) {
    if (!fb_req.response || fb_req.response->framebuffer_count < 1) return;
    struct limine_framebuffer *fb = fb_req.response->framebuffers[0];
    uint64_t line_height = current_font_h;

    if (back_buffer_available) {
        scroll_region_both(1, bg_color);
    } else {
        scroll_cell_region(1, bg_color);
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

    if (cursor_visible) show_cursor(false);

    clear_cell_rows(0, cell_rows, bg_color);

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

int putchar(int c) {
    uint64_t rflags;
    spin_lock_irqsave(&term_lock, &rflags);
    int ret = putchar_unlocked(c);
    spin_unlock_irqrestore(&term_lock, rflags);
    return ret;
}

uint64_t write_terminal(const char *buf, uint64_t count, bool onlcr) {
    if (!buf) return 0;
    uint64_t rflags;
    spin_lock_irqsave(&term_lock, &rflags);
    begin_fb_batch();
    for (uint64_t i = 0; i < count; i++) {
        if (onlcr && buf[i] == '\n') putchar_unlocked('\r');
        putchar_unlocked(buf[i]);
    }
    end_fb_batch();
    spin_unlock_irqrestore(&term_lock, rflags);
    return count;
}

uint64_t write_terminal_tty(int tty_idx, const char *buf, uint64_t count, bool onlcr) {
    int previous_tty = active_terminal_tty;
    if (tty_idx < 0 || tty_idx >= NUM_TTYS) tty_idx = previous_tty;
    if (tty_idx != previous_tty) {
        select_terminal_vt(tty_idx, false);
        terminal_display_enabled = false;
    }
    uint64_t result = write_terminal(buf, count, onlcr);
    if (tty_idx != previous_tty) {
        terminal_display_enabled = true;
        select_terminal_vt(previous_tty, true);
    }
    return result;
}

void switch_terminal_tty(int tty_idx) {
    select_terminal_vt(tty_idx, true);
    serial_replay_screen();
}

int puts(const char *s) {
    if (!s) return EOF;

    uint64_t rflags;
    spin_lock_irqsave(&term_lock, &rflags);
    begin_fb_batch();

    while (*s) { 
        putchar_unlocked(*s); 
        s++; 
    }

    end_fb_batch();
    spin_unlock_irqrestore(&term_lock, rflags);

    return 0;
}

int vprintf(const char *fmt, va_list args) {
    int total_written = 0;
    uint64_t rflags;
    spin_lock_irqsave(&term_lock, &rflags);
    begin_fb_batch();

    #define PUTC(c) do { putchar_unlocked(c); total_written++; } while(0)

    for (const char *p = fmt; *p != '\0'; p++) {
        if (*p != '%') {
            if (*p == '\n') PUTC('\r');
            PUTC(*p);
            continue;
        }
        p++;
        if (*p == '\0') { PUTC('%'); break; }
        bool left_align = false;
        int width = 0;
        char pad_char = ' ';
        int length_modifier = 0;

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
        if (*p == 'l') {
            length_modifier = 1;
            p++;
            if (*p == 'l') {
                length_modifier = 2;
                p++;
            }
        } else if (*p == 'z') {
            length_modifier = 3;
            p++;
        }

        switch (*p) {
            case 's': {
                const char *s = va_arg(args, const char *);
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
            case 'x': case 'X': {
                bool is_signed = *p == 'd' || *p == 'i';
                bool is_neg = false;
                uint64_t val = 0;
                if (is_signed) {
                    int64_t signed_val;
                    if (length_modifier == 2) signed_val = va_arg(args, long long);
                    else if (length_modifier == 1) signed_val = va_arg(args, long);
                    else if (length_modifier == 3) signed_val = va_arg(args, ptrdiff_t);
                    else signed_val = va_arg(args, int);
                    is_neg = signed_val < 0;
                    val = is_neg ? (uint64_t)(-(signed_val + 1)) + 1 : (uint64_t)signed_val;
                } else {
                    if (length_modifier == 2) val = va_arg(args, unsigned long long);
                    else if (length_modifier == 1) val = va_arg(args, unsigned long);
                    else if (length_modifier == 3) val = va_arg(args, size_t);
                    else val = va_arg(args, unsigned int);
                }
                int base = (*p == 'x' || *p == 'X') ? 16 : *p == 'o' ? 8 : 10;
                char buf[64];
                int_to_str(val, buf, 64, base, (*p == 'X'));
                int len = 0;
                while (buf[len]) len++;
                if (is_neg) len++; // account for '-'
                if (!left_align && pad_char == '0' && is_neg) { PUTC('-'); is_neg = false; }
                if (!left_align) while (width > len) { PUTC(pad_char); width--; }
                if (is_neg) PUTC('-');
                char *ptr = buf;
                while (*ptr) PUTC(*ptr++);
                if (left_align) while (width > len) { PUTC(' '); width--; }
                break;
            }
            case 'p': {
                uint64_t x = (uint64_t)(uintptr_t)va_arg(args, void *);
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

    end_fb_batch();
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

void init_terminal_backbuffer(void) {
    if (hhdm_offset == 0) return;
    if (!fb_req.response || fb_req.response->framebuffer_count < 1) return;
    struct limine_framebuffer *fb = fb_req.response->framebuffers[0];
    if (!fb || !fb->address || !fb->width || !fb->height || fb->width > 8192 || fb->height > 8192 ||
        fb->width > UINT64_MAX / sizeof(uint32_t) || fb->pitch < fb->width * sizeof(uint32_t) ||
        fb->pitch > UINT64_MAX / fb->height) return;

    uint64_t prepared_columns = current_font_w ? fb->width / current_font_w : 0;
    uint64_t prepared_rows = current_font_h ? fb->height / current_font_h : 0;
    if (prepared_columns && prepared_rows > UINT64_MAX / prepared_columns) return;
    uint64_t cell_count = prepared_columns * prepared_rows;
    if (cell_count > UINT64_MAX / sizeof(terminal_cell_t)) return;
    back_buffer_width = fb->width;
    back_buffer_height = fb->height;
    back_buffer_pitch = fb->width * sizeof(uint32_t);

    uint64_t required_size = back_buffer_pitch * fb->height;
    uint32_t *prepared_back_buffer = (uint32_t *)malloc(required_size);
    uint32_t *prepared_alt_back_buffer = (uint32_t *)malloc(required_size);
    uint64_t cell_size = cell_count * sizeof(terminal_cell_t);
    terminal_cell_t *prepared_cell_buffer = cell_size ? (terminal_cell_t *)malloc(cell_size) : NULL;
    terminal_cell_t *prepared_alt_cell_buffer = cell_size ? (terminal_cell_t *)malloc(cell_size) : NULL;

    if (prepared_cell_buffer && prepared_alt_cell_buffer) {
        cell_buffer = prepared_cell_buffer;
        alt_cell_buffer = prepared_alt_cell_buffer;
        cell_columns = prepared_columns;
        cell_rows = prepared_rows;
        cell_font_w = current_font_w;
        cell_font_h = current_font_h;
        rendered_font_generation = current_font_generation;
        blank_cells(cell_buffer, cell_columns * cell_rows, bg_color);
        blank_cells(alt_cell_buffer, cell_columns * cell_rows, bg_color);
    } else {
        free(prepared_cell_buffer);
        free(prepared_alt_cell_buffer);
    }

    if (prepared_back_buffer && prepared_alt_back_buffer) {
        back_buffer = prepared_back_buffer;
        alt_back_buffer = prepared_alt_back_buffer;
        back_buffer_initialized = true;
        back_buffer_available = true;
        backbuffer_reload_from_fb(fb); // seed from current VRAM contents
    } else {
        free(prepared_back_buffer);
        free(prepared_alt_back_buffer);
        back_buffer = NULL;
        alt_back_buffer = NULL;
        back_buffer_initialized = true;
        back_buffer_available = false;
    }
    save_terminal_vt(active_terminal_tty);
    log("terminal: initialized terminal backbuffer\n");
}
