#include <stdint.h>
#include <stdbool.h>
#include <main/log.h>
#include <io/ps2_keyboard.h>
#include <io/keyboard.h>
#include <io/io.h>
#include <io/tty.h>

static uint8_t ps2_repeat_key = 0;
static int ps2_repeat_timer = 0;
static bool ps2_key_held[128] = { false };
static uint8_t ps2_pending_leds = 0;
static uint8_t ps2_transaction_leds = 0;
static uint8_t ps2_applied_leds = 0;
static uint8_t ps2_led_phase = 0;
static uint8_t ps2_led_retries = 0;
static bool ps2_leds_applied = false;
static bool ps2_controller_present = true;

void set_ps2_keyboard_leds(uint8_t leds) {
    ps2_pending_leds = 0;
    if (leds & KBD_LED_SCROLL_LOCK) ps2_pending_leds |= 1u << 0;
    if (leds & KBD_LED_NUM_LOCK)    ps2_pending_leds |= 1u << 1;
    if (leds & KBD_LED_CAPS_LOCK)   ps2_pending_leds |= 1u << 2;
    if (!ps2_controller_present) return;
    if (ps2_led_phase != 0) return;
    if (ps2_leds_applied && ps2_pending_leds == ps2_applied_leds) return;
    uint8_t status = inb(0x64);
    if (status == 0xFF || (status & 0x02)) return;

    outb(0x60, 0xED);
    ps2_led_retries = 0;
    ps2_led_phase = 1;
}

void handle_ps2_scancode(uint8_t sc) {
    if (ps2_led_phase != 0) {
        if (sc == 0xFA) {
            ps2_led_retries = 0;
            if (ps2_led_phase == 1) {
                ps2_transaction_leds = ps2_pending_leds;
                outb(0x60, ps2_transaction_leds);
                ps2_led_phase = 2;
            } else {
                ps2_applied_leds = ps2_transaction_leds;
                ps2_leds_applied = true;
                ps2_led_phase = 0;
                if (ps2_pending_leds != ps2_applied_leds) {
                    outb(0x60, 0xED);
                    ps2_led_phase = 1;
                }
            }
            return;
        }

        if (sc == 0xFE) {
            if (++ps2_led_retries <= 3) {
                outb(0x60, ps2_led_phase == 1 ? 0xED : ps2_transaction_leds);
            } else {
                ps2_led_phase = 0;
            }
            return;
        }

        ps2_led_phase = 0;
    }

    if (sc & 0x80) {
        uint8_t key = sc & 0x7F;
        if (key < 128) ps2_key_held[key] = false;
        ps2_repeat_key = 0;
        ps2_repeat_timer = 0;
        uint32_t next = (key_head + 1) & 127;
        if (next != key_tail) { key_buffer[key_head] = sc; key_head = next; }
    } else {
        bool was_held = sc < 128 && ps2_key_held[sc];
        if (sc < 128) ps2_key_held[sc] = true;
        ps2_repeat_key = sc;
        ps2_repeat_timer = 0;
        uint32_t next = (key_head + 1) & 127;
        if (next != key_tail) { key_buffer[key_head] = sc; key_head = next; }
        if (!was_held) handle_keyboard_lock_scancode(sc);
    }
    // Feed the active TTY's ring buffer with the ASCII character
    tty_process_scancode(sc);
    set_ps2_keyboard_leds(get_keyboard_led_state());
}

void init_ps2_keyboard(void) {
    uint8_t status = inb(0x64);
    if (status == 0xFF) {
        ps2_controller_present = false;
        log("ps2 keyboard: no controller found\n");
        return;
    }

    for (int i = 0; i < 256 && (inb(0x64) & 1); i++) inb(0x60);
    set_ps2_keyboard_leds(get_keyboard_led_state());
    log("ps2 keyboard: initialized ps2 keyboard\n");
}
