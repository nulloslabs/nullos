#pragma once

#include <freestanding/stdint.h>
#include <freestanding/stdbool.h>

// ============================================================================
// Shared scancode ring buffer (filled by PS/2 ISR and USB keyboard driver)
// ============================================================================
extern uint8_t key_buffer[128];
extern volatile uint32_t key_head;
extern volatile uint32_t key_tail;

// ============================================================================
// Unified keyboard API
// ============================================================================
uint8_t get_scancode(void);            // non-blocking, returns 0 if none
char    scancode_to_ascii(uint8_t sc); // basic US QWERTY
char    getc(void);                    // blocking: waits for a printable char