#pragma once

#include <freestanding/stdint.h>

void put_pixel_fb(uint32_t x, uint32_t y, uint32_t color);
void putc_fb(char c, int x, int y, uint32_t fg, uint32_t bg);
