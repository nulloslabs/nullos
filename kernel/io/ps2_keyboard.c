#include <freestanding/stdint.h>
#include <io/ps2_keyboard.h>
#include <io/keyboard.h>
#include <io/io.h>
#include <io/ttys.h>
#include <main/log.h>

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
    // Feed the active TTY's ring buffer with the ASCII character
    tty_process_scancode(sc);
}

void flush_ps2_keyboard_controller(void) {
    while (inb(0x64) & 1) inb(0x60);
    log("flushed ps2 keyboard controller");
}
