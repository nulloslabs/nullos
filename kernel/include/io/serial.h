#pragma once

#include <freestanding/stdint.h>

#define COM1 0x3F8
#define COM2 0x2F8
#define COM3 0x3E8
#define COM4 0x2E8

extern int g_debug_port;

void serial_putc(uint16_t port, char c);
void serial_puts(uint16_t port, const char *s);
void serial_printf(uint16_t port, const char *fmt, ...);