#include <main/log.h>
#include <io/io.h>
#include <io/pic.h>

void eoi_pic(void) {
    // Send EOI (End of interrupt) to master controller
    outb(0x20, 0x20);
    // Send EOI to slave controller
    outb(0xA0, 0x20);
}

void disable_pic(void) {
    // Mask all IRQs on both PICs
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
    log("disabled pic");
}

void mask_pic_irq(uint8_t irq) {
    if (irq > 15) return;
    uint16_t port = irq < 8 ? 0x21 : 0xA1;
    uint8_t bit = irq < 8 ? irq : irq - 8;
    uint8_t mask = inb(port);
    outb(port, mask | (1 << bit));
}

void unmask_pic_irq(uint8_t irq) {
    if (irq > 15) return;
    uint16_t port = irq < 8 ? 0x21 : 0xA1;
    uint8_t bit = irq < 8 ? irq : irq - 8;
    uint8_t mask = inb(port);
    outb(port, mask & ~(1 << bit));
}

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
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);

    mask_pic_irq(0);
    mask_pic_irq(1);
    mask_pic_irq(11);

    log("remapped pic");
}
