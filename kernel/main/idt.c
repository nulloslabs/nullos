#include <main/log.h>
#include <main/gdt.h>
#include <main/idt.h>

__attribute__((aligned(16)))
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
extern void reserved_isr(void);

// Timer, keyboard.
extern void isr32(void);
extern void isr33(void);

// Legacy PCI INTx / GSI vectors 48..63 (GSIs 16-31; 32-47 stay legacy-device territory).
#define DECL_LEGACY(v) extern void isr##v(void);
DECL_LEGACY(48) DECL_LEGACY(49) DECL_LEGACY(50) DECL_LEGACY(51) DECL_LEGACY(52) DECL_LEGACY(53)
DECL_LEGACY(54) DECL_LEGACY(55) DECL_LEGACY(56) DECL_LEGACY(57) DECL_LEGACY(58) DECL_LEGACY(59)
DECL_LEGACY(60) DECL_LEGACY(61) DECL_LEGACY(62) DECL_LEGACY(63)
#undef DECL_LEGACY

// MSI vectors 64..111 — declared via macro to avoid 48 lines of extern.
#define DECL_MSI(v) extern void isr##v(void);
DECL_MSI(64) DECL_MSI(65) DECL_MSI(66) DECL_MSI(67) DECL_MSI(68) DECL_MSI(69)
DECL_MSI(70) DECL_MSI(71) DECL_MSI(72) DECL_MSI(73) DECL_MSI(74) DECL_MSI(75)
DECL_MSI(76) DECL_MSI(77) DECL_MSI(78) DECL_MSI(79) DECL_MSI(80) DECL_MSI(81)
DECL_MSI(82) DECL_MSI(83) DECL_MSI(84) DECL_MSI(85) DECL_MSI(86) DECL_MSI(87)
DECL_MSI(88) DECL_MSI(89) DECL_MSI(90) DECL_MSI(91) DECL_MSI(92) DECL_MSI(93)
DECL_MSI(94) DECL_MSI(95) DECL_MSI(96) DECL_MSI(97) DECL_MSI(98) DECL_MSI(99)
DECL_MSI(100) DECL_MSI(101) DECL_MSI(102) DECL_MSI(103) DECL_MSI(104) DECL_MSI(105)
DECL_MSI(106) DECL_MSI(107) DECL_MSI(108) DECL_MSI(109) DECL_MSI(110) DECL_MSI(111)
#undef DECL_MSI

// Catch-all for unhandled hardware interrupts
extern void spurious_isr(void);

void idt_set_descriptor(uint8_t vector, void* isr, uint8_t flags) {
    struct idt_entry* descriptor = &idt[vector];
    descriptor->isr_low = (uint64_t)isr & 0xFFFF;
    descriptor->kernel_cs = GDT_KERNEL_CS;
    descriptor->ist = 0;
    descriptor->attributes = flags;
    descriptor->isr_mid = ((uint64_t)isr >> 16) & 0xFFFF;
    descriptor->isr_high = ((uint64_t)isr >> 32) & 0xFFFFFFFF;
    descriptor->reserved = 0;
}

void load_idt_for_cpu(void) { __asm__ volatile ("lidt %0" : : "m"(idtr)); }

void init_idt(void) {
    idtr.base = (uint64_t)&idt[0];
    idtr.limit = (uint16_t)sizeof(struct idt_entry) * 256 - 1;

    // Fill ALL vectors with the spurious handler first so no IDT entry
    // is ever "not present" — prevents #GP on unexpected hardware IRQs.
    // 0x8E = Present (0x80) | DPL 0 (0x00) | Interrupt Gate (0x0E)
    for (int i = 0; i < 256; i++) { idt_set_descriptor(i, spurious_isr, 0x8E); }

    // Hardware exceptions and IRQs are kernel-only interrupt gates.
    idt_set_descriptor(0, isr0, 0x8E);
    idt_set_descriptor(4, isr4, 0x8E);
    idt_set_descriptor(5, isr5, 0x8E);
    idt_set_descriptor(6, isr6, 0x8E);
    idt_set_descriptor(7, isr7, 0x8E);
    idt_set_descriptor(8, isr8, 0x8E);
    idt[8].ist = 1; // Use IST1 (dedicated double-fault stack)
    idt_set_descriptor(13, isr13, 0x8E);
    idt_set_descriptor(14, isr14, 0x8E);
    for (int i = 22; i < 27; i++) { idt_set_descriptor(i, reserved_isr, 0x8E); }
    idt_set_descriptor(30, isr30, 0x8E);
    idt_set_descriptor(31, reserved_isr, 0x8E);
    idt_set_descriptor(32, isr32, 0x8E);
    idt_set_descriptor(33, isr33, 0x8E);

    // Legacy INTx / GSI vectors.
#define SET_LEGACY(v) idt_set_descriptor(v, isr##v, 0x8E);
    SET_LEGACY(48) SET_LEGACY(49) SET_LEGACY(50) SET_LEGACY(51) SET_LEGACY(52) SET_LEGACY(53)
    SET_LEGACY(54) SET_LEGACY(55) SET_LEGACY(56) SET_LEGACY(57) SET_LEGACY(58) SET_LEGACY(59)
    SET_LEGACY(60) SET_LEGACY(61) SET_LEGACY(62) SET_LEGACY(63)
#undef SET_LEGACY

    // MSI vectors. Use DPL 0 (0x8E) — userspace shouldn't be able to
    // raise a device interrupt via INT n.
#define SET_MSI(v) idt_set_descriptor(v, isr##v, 0x8E);
    SET_MSI(64) SET_MSI(65) SET_MSI(66) SET_MSI(67) SET_MSI(68) SET_MSI(69)
    SET_MSI(70) SET_MSI(71) SET_MSI(72) SET_MSI(73) SET_MSI(74) SET_MSI(75)
    SET_MSI(76) SET_MSI(77) SET_MSI(78) SET_MSI(79) SET_MSI(80) SET_MSI(81)
    SET_MSI(82) SET_MSI(83) SET_MSI(84) SET_MSI(85) SET_MSI(86) SET_MSI(87)
    SET_MSI(88) SET_MSI(89) SET_MSI(90) SET_MSI(91) SET_MSI(92) SET_MSI(93)
    SET_MSI(94) SET_MSI(95) SET_MSI(96) SET_MSI(97) SET_MSI(98) SET_MSI(99)
    SET_MSI(100) SET_MSI(101) SET_MSI(102) SET_MSI(103) SET_MSI(104) SET_MSI(105)
    SET_MSI(106) SET_MSI(107) SET_MSI(108) SET_MSI(109) SET_MSI(110) SET_MSI(111)
#undef SET_MSI

    __asm__ volatile ("lidt %0" : : "m"(idtr));
    log("idt: initialized idt\n");
}
