#pragma once

#include <freestanding/stdint.h>
#include <freestanding/stdarg.h>

#define COM1 0x3F8
#define COM2 0x2F8
#define COM3 0x3E8
#define COM4 0x2E8

int serial_putchar(uint16_t port, int c);
int serial_puts(uint16_t port, const char *s);
int serial_vprintf(uint16_t port, const char *fmt, va_list args);
int serial_printf(uint16_t port, const char *fmt, ...);
void init_serial_ports(void);
