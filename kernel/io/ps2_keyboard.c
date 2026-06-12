#include <freestanding/stdint.h>
#include <freestanding/stdbool.h>
#include <io/ps2_keyboard.h>
#include <io/keyboard.h>
#include <io/io.h>

static uint8_t ps2_repeat_key = 0;
static int ps2_repeat_timer = 0;
static bool ps2_key_held[128] = { false };

void handle_ps2_scancode(uint8_t sc) {
    if (sc & 0x80) {
        uint8_t key = sc & 0x7F;
        ps2_key_held[key] = false;
        ps2_repeat_key = 0;
        ps2_repeat_timer = 0;
        uint32_t next = (key_head + 1) & 127;
        if (next != key_tail) { key_buffer[key_head] = sc; key_head = next; }
    } else {
        ps2_key_held[sc] = true;
        ps2_repeat_key = sc;
        ps2_repeat_timer = 0;
        uint32_t next = (key_head + 1) & 127;
        if (next != key_tail) { key_buffer[key_head] = sc; key_head = next; }
    }
}