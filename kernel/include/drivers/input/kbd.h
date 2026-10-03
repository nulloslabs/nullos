#pragma once

#include <freestanding/stdbool.h>
#include <freestanding/stdint.h>

#define KBD_LED_NUM_LOCK    (1u << 0)
#define KBD_LED_CAPS_LOCK   (1u << 1)
#define KBD_LED_SCROLL_LOCK (1u << 2)

extern uint8_t key_buffer[128];
extern volatile uint32_t key_head;
extern volatile uint32_t key_tail;

bool is_kbd_alt_pressed(void);
bool is_kbd_ctrl_pressed(void);
uint8_t get_kbd_led_state(void);
void set_kbd_cad_reboot(bool enabled);
void handle_kbd_lock_scancode(uint8_t sc);
void handle_kbd_cad_scancode(uint8_t sc);
uint8_t get_scancode(void);
char scancode_to_ascii(uint8_t sc);
char getc(void);
