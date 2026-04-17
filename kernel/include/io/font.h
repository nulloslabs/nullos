#pragma once

#include <freestanding/stdint.h>

extern unsigned char current_font[16384];
extern uint8_t current_font_w;
extern uint8_t current_font_h;

void change_font(const char *path, uint8_t w, uint8_t h);