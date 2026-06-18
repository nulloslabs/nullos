#include <freestanding/stdint.h>
#include <freestanding/errno.h>
#include <limine/limine.h>
#include <main/limine_req.h>
#include <main/string.h>
#include <main/halt.h>
#include <io/framebuffer.h>
#include <io/fonts.h>

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
    if (!fb_req.response || fb_req.response->framebuffer_count < 1) return;
    struct limine_framebuffer *fb = fb_req.response->framebuffers[0];

    // Standard color is 0xRRGGBB. Extract components.
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;

    // Scale components to target bit depths and shift into place
    uint32_t pixel = 0;
    pixel |= (uint32_t)((r * ((1 << fb->red_mask_size) - 1)) / 255) << fb->red_mask_shift;
    pixel |= (uint32_t)((g * ((1 << fb->green_mask_size) - 1)) / 255) << fb->green_mask_shift;
    pixel |= (uint32_t)((b * ((1 << fb->blue_mask_size) - 1)) / 255) << fb->blue_mask_shift;

    uint8_t *fb_ptr = (uint8_t *)fb->address;
    uint64_t offset = y * fb->pitch + x * ((fb->bpp + 7) / 8);

    switch (fb->bpp) {
        case 15:
        case 16:
            *(uint16_t *)(fb_ptr + offset) = (uint16_t)pixel;
            break;
        case 24:
            fb_ptr[offset + 0] = (uint8_t)(pixel & 0xFF);
            fb_ptr[offset + 1] = (uint8_t)((pixel >> 8) & 0xFF);
            fb_ptr[offset + 2] = (uint8_t)((pixel >> 16) & 0xFF);
            break;
        case 32:
            *(uint32_t *)(fb_ptr + offset) = pixel;
            break;
        default:
            halt();
            break;
    }
}

void putchar_fb(char c, int x, int y, uint32_t fg, uint32_t bg) {
    if (!fb_req.response || fb_req.response->framebuffer_count < 1) return; // If there's no framebuffer don't even bother drawing.
    if (!current_font_w || !current_font_h) return; // If there's no font don't even bother drawing.

    // Use the index for the offset calculation
    unsigned char *glyph = &current_font[(unsigned char)c * current_font_h];

    for (int row = 0; row < current_font_h; row++) {
        unsigned char row_data = glyph[row];
        for (int col = 0; col < current_font_w; col++) {
            if (row_data & (0x80 >> col)) put_pixel_fb(x + col, y + row, fg);
            else put_pixel_fb(x + col, y + row, bg);
        }
    }
}
