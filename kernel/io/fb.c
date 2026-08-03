#include <stdint.h>
#include <errno.h>
#include <main/limine_req.h>
#include <main/string.h>
#include <main/halt.h>
#include <io/fb.h>
#include <io/bga.h>
#include <io/svga_ii.h>
#include <io/fonts.h>
#include <io/terminal.h>
#include <limine.h>

fb_driver_t current_fb_driver = FB_NONE;
uint64_t fb_xres_virtual = 0;
uint64_t fb_yres_virtual = 0;
uint64_t fb_xoffset = 0;
uint64_t fb_yoffset = 0;

uint64_t fb_read_index(int idx, void* buf, uint64_t count, uint64_t offset) {
    if (!fb_req.response || idx >= (int)fb_req.response->framebuffer_count) return (uint64_t)-ENODEV;
    struct limine_framebuffer *fb = fb_req.response->framebuffers[idx];
    uint64_t size = (idx == 0 && fb_yres_virtual ? fb_yres_virtual : fb->height) * fb->pitch;
    if (offset >= size) return 0;
    if (offset + count > size) count = size - offset;
    memcpy(buf, (const uint8_t*)fb->address + offset, count);
    return count;
}

uint64_t fb_write_index(int idx, const void* buf, uint64_t count, uint64_t offset) {
    if (!fb_req.response || idx >= (int)fb_req.response->framebuffer_count) return (uint64_t)-ENODEV;
    struct limine_framebuffer *fb = fb_req.response->framebuffers[idx];
    uint64_t size = (idx == 0 && fb_yres_virtual ? fb_yres_virtual : fb->height) * fb->pitch;
    // Writing past the end of the framebuffer is "no space left on device",
    // not "EOF". Tools like `cat urandom > /dev/fb0` rely on this to know
    // when to surface ENOSPC instead of looping forever on a 0-byte write.
    if (offset >= size) return (uint64_t)-ENOSPC;
    if (count > size - offset) count = size - offset;
    memcpy((uint8_t*)fb->address + offset, buf, count);
    if (idx == 0) {
        back_buffer_dirty = true;
        if (count) {
            uint64_t first_y = offset / fb->pitch;
            uint64_t last_y = (offset + count - 1) / fb->pitch;
            if (first_y < fb->height) {
                if (last_y >= fb->height) last_y = fb->height - 1;
                (void)update_fb(0, first_y, fb->width, last_y - first_y + 1);
            }
        }
    }
    return count;
}

int set_fb_resolution(uint64_t xres, uint64_t yres, uint64_t xres_virtual, uint64_t yres_virtual, uint64_t xoffset, uint64_t yoffset, uint16_t bpp) {
    switch (current_fb_driver) {
        case FB_NONE:
            return -ENODEV;
        case FB_LIMINE:
            return -EOPNOTSUPP;
        case FB_BGA:
            return set_bga_resolution(xres, yres, xres_virtual, yres_virtual, xoffset, yoffset, bpp);
        case FB_SVGA_II:
            return set_svga_ii_resolution(xres, yres, xres_virtual, yres_virtual, xoffset, yoffset, bpp);
        default:
            // whar
            return -EINVAL;
    }
}

int update_fb(uint64_t x, uint64_t y, uint64_t width, uint64_t height) {
    switch (current_fb_driver) {
        case FB_NONE:
            return -ENODEV;
        case FB_LIMINE:
        case FB_BGA:
            return 0;
        case FB_SVGA_II:
            return update_svga_ii(x, y, width, height);
        default:
            // whar
            return -EINVAL;
    }
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

    if (fb->bpp == 8) {
        fb_ptr[offset] = bga_palette_index(color);
        return;
    }

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
