#include <freestanding/stdint.h>
#include <io/framebuffer.h>
#include <limine/limine.h>
#include <main/limine_req.h>
#include <io/fonts.h>
#include <main/string.h>
#include <main/errno.h>

uint64_t fb_read_index(int idx, void* buf, uint64_t count, uint64_t offset) {
    if (!fb_req.response || idx >= (int)fb_req.response->framebuffer_count) return (uint64_t)-ENODEV;
    struct limine_framebuffer *fb = fb_req.response->framebuffers[idx];
    uint64_t size = fb->height * fb->pitch;
    if (offset >= size) return 0;
    if (offset + count > size) count = size - offset;
    memcpy(buf, (const uint8_t*)fb->address + offset, count);
    return count;
}

uint64_t fb_write_index(int idx, const void* buf, uint64_t count, uint64_t offset) {
    if (!fb_req.response || idx >= (int)fb_req.response->framebuffer_count) return (uint64_t)-ENODEV;
    struct limine_framebuffer *fb = fb_req.response->framebuffers[idx];
    uint64_t size = fb->height * fb->pitch;
    if (offset >= size) return 0;
    if (offset + count > size) count = size - offset;
    memcpy((uint8_t*)fb->address + offset, buf, count);
    return count;
}

void put_pixel_fb(uint32_t x, uint32_t y, uint32_t color) {
    if (!fb_req.response || fb_req.response->framebuffer_count < 1) return; // If there's no framebuffer don't even bother drawing.
    struct limine_framebuffer *fb = fb_req.response->framebuffers[0];
    uint32_t *fb_ptr = (uint32_t *)fb->address;
    fb_ptr[y * (fb->pitch / 4) + x] = color;
}

void putc_fb(char c, int x, int y, uint32_t fg, uint32_t bg) {
    if (!fb_req.response || fb_req.response->framebuffer_count < 1) return; // If there's no framebuffer don't even bother drawing.
    if (!current_font_w || !current_font_h) return; // If there's no font don't even bother drawing.
    if ((unsigned char)c < 0x20) return; // If the character is not printable, don't even bother drawing.

    // Use the index for the offset calculation
    unsigned char *glyph = &current_font[(unsigned char)c * current_font_h];

    for (int row = 0; row < current_font_h; row++) {
        unsigned char row_data = glyph[row];
        for (int col = 0; col < current_font_w; col++) {
            if (row_data & (0x80 >> col)) {
                put_pixel_fb(x + col, y + row, fg);
            } else {
                put_pixel_fb(x + col, y + row, bg);
            }
        }
    }
}
