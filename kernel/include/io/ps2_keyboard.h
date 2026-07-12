#pragma once

#include <freestanding/stdint.h>

void handle_ps2_scancode(uint8_t sc);
void flush_ps2_keyboard_controller(void);
