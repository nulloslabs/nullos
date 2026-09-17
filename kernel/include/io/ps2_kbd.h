#pragma once

#include <stdint.h>

void set_ps2_kbd_leds(uint8_t leds);
void handle_ps2_scancode(uint8_t sc);
void init_ps2_kbd(void);
