#pragma once

#include <stdint.h>

// Shared scancode ring buffer (filled by PS/2 ISR and USB keyboard driver)
extern uint8_t key_buffer[128];
extern volatile uint32_t key_head;
extern volatile uint32_t key_tail;

#define KBD_LED_NUM_LOCK    (1u << 0)
#define KBD_LED_CAPS_LOCK   (1u << 1)
#define KBD_LED_SCROLL_LOCK (1u << 2)

// Unified keyboard API
uint8_t get_scancode(void);            // non-blocking, returns 0 if none
char    scancode_to_ascii(uint8_t sc); // basic US QWERTY
char    getc(void);                    // blocking: waits for a printable char
bool    kbd_alt_pressed(void);         // Alt key held?
uint8_t get_keyboard_led_state(void);
void    handle_keyboard_lock_scancode(uint8_t sc);
