#include <freestanding/stdint.h>
#include <io/io.h>
#include <io/pit.h>
#include <io/terminal.h>

void init_pit(uint32_t frequency) {
    uint32_t divisor = 1193182 / frequency;
    outb(0x43, 0x36);
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
    printf("PIT: Initialized PIT.\n");
}
