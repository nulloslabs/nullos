#include <main/io.h>
#include <main/log.h>
#include <main/pic.h>

void mask_pic_irq(uint8_t irq) {
    if (irq > 15) return;
    uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = irq < 8 ? irq : irq - 8;
    uint8_t mask = inb(port);
    outb(mask | (1 << bit), port);
}

void unmask_pic_irq(uint8_t irq) {
    if (irq > 15) return;
    uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = irq < 8 ? irq : irq - 8;
    uint8_t mask = inb(port);
    outb(mask & ~(1 << bit), port);
}

void eoi_pic(void) {
    // Send EOI (End of interrupt) to master controller
    outb(0x20, PIC1_CMD);
    // Send EOI to slave controller
    outb(0x20, PIC2_CMD);
}

void disable_pic(void) {
    // Mask all IRQs on both PICs
    outb(0xFF, PIC1_DATA);
    outb(0xFF, PIC2_DATA);
}

void remap_pic(void) {
    outb(0x11, PIC1_CMD);
    outb(0x11, PIC2_CMD);
    wait_io();
    outb(0x20, PIC1_DATA);
    outb(0x28, PIC2_DATA);
    wait_io();
    outb(0x04, PIC1_DATA);
    outb(0x02, PIC2_DATA);
    wait_io();
    outb(0x01, PIC1_DATA);
    outb(0x01, PIC2_DATA);
    wait_io();
    outb(0xFF, PIC1_DATA);
    outb(0xFF, PIC2_DATA);

    mask_pic_irq(0);
    mask_pic_irq(1);
    mask_pic_irq(11);

    log("pic: remapped pic\n");
}
