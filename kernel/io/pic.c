#include <io/io.h>
#include <io/pic.h>
#include <io/terminal.h>

void remap_pic(void) {
    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    io_wait();
    outb(0x21, 0x20);
    outb(0xA1, 0x28);
    io_wait();
    outb(0x21, 0x04);
    outb(0xA1, 0x02);
    io_wait();
    outb(0x21, 0x01);
    outb(0xA1, 0x01);
    io_wait();
    outb(0x21, 0xF8);
    outb(0xA1, 0xE7);

    while (inb(0x64) & 0x01) inb(0x60);
    printf("PIC: Remapped PIC.\n");
}

void disable_pic(void) {
    // Mask all IRQs on both PICs
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
    printf("PIC: Disabled PIC.\n");
}

void eoi_pic(void) {
    outb(0x20, 0x20);
    outb(0xA0, 0x20);
}
