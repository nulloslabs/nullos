#pragma once

#include <stdint.h>
#include <stdarg.h>

#define COM1 0x3F8
#define COM2 0x2F8
#define COM3 0x3E8
#define COM4 0x2E8

void putc_serial(uint16_t port, char c);
void puts_serial(uint16_t port, const char *s);
int vprintf_serial(uint16_t port, const char *fmt, va_list args);
int printf_serial(uint16_t port, const char *fmt, ...);
void init_serial_ports(void);
