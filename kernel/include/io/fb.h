#pragma once

#include <stdint.h>

typedef enum {
    FB_NONE = 0,   // No framebuffer driver
    FB_LIMINE,     // Framebuffer provided by Limine, VESA/GOP
    FB_BGA,        // Bochs Graphics Adapter
    FB_QXL,        // Red Hat QXL
    FB_SVGA_II,    // VMware SVGA II
    FB_VIRTIO_GPU, // Red Hat virtio-gpu
} fb_driver_t;

extern fb_driver_t current_fb_driver;
extern uint64_t fb_xres_virtual;
extern uint64_t fb_yres_virtual;
extern uint64_t fb_xoffset;
extern uint64_t fb_yoffset;

uint64_t fb_read_index(int idx, void *buf, uint64_t count, uint64_t offset);
uint64_t fb_write_index(int idx, const void *buf, uint64_t count, uint64_t offset);
int set_fb_resolution(uint64_t xres, uint64_t yres, uint64_t xres_virtual, uint64_t yres_virtual, uint64_t xoffset, uint64_t yoffset, uint16_t bpp);
int update_fb(uint64_t x, uint64_t y, uint64_t width, uint64_t height);
void put_pixel_fb(uint32_t x, uint32_t y, uint32_t color);
void putchar_fb(char c, int x, int y, uint32_t fg, uint32_t bg);
