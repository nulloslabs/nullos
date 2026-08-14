#pragma once

#include <stdint.h>

extern unsigned char current_font[16384];
extern uint8_t current_font_w;
extern uint8_t current_font_h;
extern uint64_t current_font_generation;

void init_default_font(void);
int change_font_data(const unsigned char *data, uint8_t w, uint8_t h);
int change_font(const char *path, uint8_t w, uint8_t h);
