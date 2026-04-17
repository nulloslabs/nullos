#include <main/gdt.h>
#include <main/string.h>
#include <io/terminal.h>

cpu_gdt_t cpu_gdts[MAX_CPUS];

extern void gdt_flush(uint64_t gdtr_ptr);
extern void tss_flush(void);

static void set_gdt_entry(uint64_t *gdt, int num, uint32_t base, uint32_t limit,
                           uint8_t access, uint8_t gran) {
    gdt[num] = (limit & 0xFFFF)
             | ((base  & 0xFFFFULL)   << 16)
             | ((base >> 16 & 0xFFULL) << 32)
             | ((uint64_t)access       << 40)
             | ((limit >> 16 & 0x0FULL)<< 48)
             | ((uint64_t)(gran & 0xF0)<< 48)
             | ((base >> 24 & 0xFFULL) << 56);
}

static void write_tss(cpu_gdt_t *g) {
    uint64_t base  = (uint64_t)&g->tss;
    uint32_t limit = sizeof(struct tss) - 1;

    // Lower 8 bytes of TSS descriptor
    set_gdt_entry(g->entries, 5, (uint32_t)base, limit, 0x89, 0x00);

    // Upper 8 bytes (bits 63:32 of base)
    g->entries[6] = (base >> 32) & 0xFFFFFFFFULL;

    memset(&g->tss, 0, sizeof(struct tss));
    g->tss.iopb_offset = sizeof(struct tss);
    g->tss.ist1 = (uint64_t)&g->df_stack[sizeof(g->df_stack)]; // IST1 for #DF
}

void init_gdt_for_cpu(int cpu_index) {
    cpu_gdt_t *g = &cpu_gdts[cpu_index];

    set_gdt_entry(g->entries, 0, 0, 0, 0x00, 0x00);   // 0x00: Null
    set_gdt_entry(g->entries, 1, 0, 0, 0x9A, 0x20);   // 0x08: Kernel CS (64-bit)
    set_gdt_entry(g->entries, 2, 0, 0, 0x92, 0x00);   // 0x10: Kernel DS
    set_gdt_entry(g->entries, 3, 0, 0, 0xF2, 0x00);   // 0x18: User DS
    set_gdt_entry(g->entries, 4, 0, 0, 0xFA, 0x20);   // 0x20: User CS (64-bit)
    write_tss(g);                                      // 0x28: TSS (slots 5+6)

    struct gdt_ptr ptr = {
        .limit = sizeof(g->entries) - 1,
        .base  = (uint64_t)g->entries
    };

    gdt_flush((uint64_t)&ptr);
    tss_flush();
}

void tss_set_kernel_stack_for_cpu(int cpu_index, void *stack) {
    cpu_gdts[cpu_index].tss.rsp0 = (uint64_t)stack;
}

void set_tss_kernel_stack(void *stack) {
    tss_set_kernel_stack_for_cpu(0, stack);
}

void init_gdt(void) {
    init_gdt_for_cpu(0);
    printf("GDT: Initialized GDT.\n");
}