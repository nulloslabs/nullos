#pragma once

#include <freestanding/stdint.h>

uint64_t fb_read_index(int idx, void* buf, uint64_t count, uint64_t offset);
uint64_t fb_write_index(int idx, const void* buf, uint64_t count, uint64_t offset);
void put_pixel_fb(uint32_t x, uint32_t y, uint32_t color);
void putchar_fb(char c, int x, int y, uint32_t fg, uint32_t bg);
