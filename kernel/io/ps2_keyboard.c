#include <freestanding/stdint.h>
#include <freestanding/stdbool.h>
#include <io/ps2_keyboard.h>
#include <io/keyboard.h>
#include <io/io.h>

// ============================================================================
// PS/2 Keyboard State
// ============================================================================
static uint8_t ps2_repeat_key = 0;
static int ps2_repeat_timer = 0;
static bool ps2_key_held[128] = { false };

// ============================================================================
// PS/2 Scancode Handler (called from keyboard ISR)
// ============================================================================
void handle_ps2_scancode(uint8_t sc) {
    if (sc & 0x80) { // Release
        uint8_t key = sc & 0x7F;
        if (ps2_key_held[key]) {
            ps2_key_held[key] = false;
        }
        if (key == (ps2_repeat_key & 0x7F)) {
            ps2_repeat_key = 0;
            ps2_repeat_timer = 0;
        }
        // Inject break code into shared ring buffer
        uint32_t next = (key_head + 1) & 127;
        if (next != key_tail) {
            key_buffer[key_head] = sc;
            key_head = next;
        }
    } else { // Press (make)
        // Filter hardware repeat: if already held, ignore
        if (ps2_key_held[sc]) {
            return;
        }
        ps2_key_held[sc] = true;
        ps2_repeat_key = sc;
        ps2_repeat_timer = 0;
        // Inject make code into shared ring buffer
        uint32_t next = (key_head + 1) & 127;
        if (next != key_tail) {
            key_buffer[key_head] = sc;
            key_head = next;
        }
    }
}

// ============================================================================
// PS/2 Software Key Repeat (called from polling loop)
// ============================================================================
void poll_ps2_keyboard(void) {
    if (ps2_repeat_key != 0) {
        ps2_repeat_timer++;
        // 4ms poll rate. 500ms delay = 125 polls. 32ms repeat = 8 polls.
        if (ps2_repeat_timer >= 125) {
            if ((ps2_repeat_timer - 125) % 8 == 0) {
                uint32_t next = (key_head + 1) & 127;
                if (next != key_tail) {
                    key_buffer[key_head] = ps2_repeat_key;
                    key_head = next;
                }
            }
        }
    }
}
