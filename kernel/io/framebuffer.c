#include <freestanding/stdint.h>
#include <io/framebuffer.h>
#include <limine/limine.h>
#include <main/limine_req.h>
#include <io/font.h>

void put_pixel_fb(struct limine_framebuffer *fb, uint32_t x, uint32_t y, uint32_t color) {
    uint32_t *fb_ptr = (uint32_t *)fb->address;
    fb_ptr[y * (fb->pitch / 4) + x] = color;
}

void putc_fb(struct limine_framebuffer *fb, char c, int x, int y, uint32_t fg, uint32_t bg) {
    if (!current_font_w || !current_font_h) return;
    if ((unsigned char)c < 0x20) return;

    // Use the index for the offset calculation
    unsigned char *glyph = &current_font[(unsigned char)c * current_font_h];

    for (int row = 0; row < current_font_h; row++) {
        unsigned char row_data = glyph[row];
        for (int col = 0; col < current_font_w; col++) {
            if (row_data & (0x80 >> col)) {
                put_pixel_fb(fb, x + col, y + row, fg);
            } else {
                put_pixel_fb(fb, x + col, y + row, bg);
            }
        }
    }
}