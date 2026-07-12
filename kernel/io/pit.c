#include <freestanding/stdint.h>
#include <main/log.h>
#include <io/io.h>
#include <io/pit.h>

uint16_t read_pit_counter(void) {
    outb(0x43, 0x00);
    uint8_t lo = inb(0x40);
    uint8_t hi = inb(0x40);
    return (uint16_t)(hi << 8) | lo;
}

void init_pit(void) {
    uint32_t divisor = 1193182 / 250;
    outb(0x43, 0x36);
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
    log("initialized pit");
}
