#include <main/log.h>
#include <io/io.h>
#include <io/pic.h>

void mask_pic_irq(uint8_t irq) {
    if (irq > 15) return;
    uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = irq < 8 ? irq : irq - 8;
    uint8_t mask = inb(port);
    outb(port, mask | (1 << bit));
}

void unmask_pic_irq(uint8_t irq) {
    if (irq > 15) return;
    uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = irq < 8 ? irq : irq - 8;
    uint8_t mask = inb(port);
    outb(port, mask & ~(1 << bit));
}

void eoi_pic(void) {
    // Send EOI (End of interrupt) to master controller
    outb(PIC1_CMD, 0x20);
    // Send EOI to slave controller
    outb(PIC2_CMD, 0x20);
}

void disable_pic(void) {
    // Mask all IRQs on both PICs
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
    log("pic: disabled pic\n");
}

void remap_pic(void) {
    outb(PIC1_CMD, 0x11);
    outb(PIC2_CMD, 0x11);
    io_wait();
    outb(PIC1_DATA, 0x20);
    outb(PIC2_DATA, 0x28);
    io_wait();
    outb(PIC1_DATA, 0x04);
    outb(PIC2_DATA, 0x02);
    io_wait();
    outb(PIC1_DATA, 0x01);
    outb(PIC2_DATA, 0x01);
    io_wait();
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);

    mask_pic_irq(0);
    mask_pic_irq(1);
    mask_pic_irq(11);

    log("pic: remapped pic\n");
}
