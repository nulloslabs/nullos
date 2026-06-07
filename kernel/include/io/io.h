#pragma once

#include <freestanding/stdint.h>

void outb(uint8_t val, uint16_t port);
uint8_t inb(uint16_t port);
void outw(uint16_t val, uint16_t port);
uint16_t inw(uint16_t port);
void outl(uint32_t val, uint16_t port);
uint32_t inl(uint16_t port);
void io_wait(void);
