#include <main/idt.h>
#include <io/terminal.h>

__attribute__((aligned(0x10))) 
static struct idt_entry idt[256];
static struct idt_ptr idtr;

// Normal exceptions
extern void isr0(void);
extern void isr4(void);
extern void isr5(void);
extern void isr6(void);
extern void isr7(void);
extern void isr8(void);
extern void isr13(void);
extern void isr14(void);
extern void isr30(void);

// Reserved exceptions (22-27, 31)
extern void isrrsv(void);

// Timer, keyboard, mouse etc.
extern void isr32(void);
extern void isr33(void);
extern void isr43(void);

// Catch-all for unhandled hardware interrupts
extern void isr_spurious(void);

void idt_set_descriptor(uint8_t vector, void* isr, uint8_t flags) {
    struct idt_entry* descriptor = &idt[vector];
    descriptor->isr_low = (uint64_t)isr & 0xFFFF;
    descriptor->kernel_cs = 0x08; // Custom Kernel CS
    descriptor->ist = 0;
    descriptor->attributes = flags;
    descriptor->isr_mid = ((uint64_t)isr >> 16) & 0xFFFF;
    descriptor->isr_high = ((uint64_t)isr >> 32) & 0xFFFFFFFF;
    descriptor->reserved = 0;
}

void load_idt_for_cpu(void) {
    asm volatile("lidt %0" : : "m"(idtr));
}

void init_idt(void) {
    idtr.base = (uint64_t)&idt[0];
    idtr.limit = (uint16_t)sizeof(struct idt_entry) * 256 - 1;

    // Fill ALL vectors with the spurious handler first so no IDT entry
    // is ever "not present" — prevents #GP on unexpected hardware IRQs.
    // 0x8E = Present (0x80) | DPL 0 (0x00) | Interrupt Gate (0x0E)
    for (int i = 0; i < 256; i++) {
        idt_set_descriptor(i, isr_spurious, 0x8E);
    }

    // Now overwrite specific vectors with their real handlers.
    // 0xEE = Present (0x80) | DPL 3 (0x60) | Interrupt Gate (0x0E)
    idt_set_descriptor(0, isr0, 0xEE);
    idt_set_descriptor(4, isr4, 0xEE);
    idt_set_descriptor(5, isr5, 0xEE);
    idt_set_descriptor(6, isr6, 0xEE);
    idt_set_descriptor(7, isr7, 0xEE);
    idt_set_descriptor(8, isr8, 0xEE);
    idt[8].ist = 1; // Use IST1 (dedicated double-fault stack)
    idt_set_descriptor(13, isr13, 0xEE);
    idt_set_descriptor(14, isr14, 0xEE);
    for (int i = 22; i < 27; i++) {
        idt_set_descriptor(i, isrrsv, 0xEE);
    }
    idt_set_descriptor(30, isr30, 0xEE);
    idt_set_descriptor(31, isrrsv, 0xEE);
    idt_set_descriptor(32, isr32, 0xEE);
    idt_set_descriptor(33, isr33, 0xEE);
    idt_set_descriptor(43, isr43, 0xEE);

    asm volatile("lidt %0" : : "m"(idtr));
    printf("IDT: Initialized IDT.\n");
}