#include <main/mp.h>
#include <io/apic.h>
#include <io/ioapic.h>
#include <main/idt.h>
#include <main/gdt.h>
#include <main/halt.h>
#include <main/scheduler.h>
#include <mm/mm.h>
#include <mm/vmm.h>
#include <main/string.h>
#include <main/limine_req.h>
#include <io/terminal.h>
#include <syscalls/syscalls.h>
#include <main/sse.h>

cpu_t cpus[MAX_CPUS];
int cpu_count = 0;
volatile int ap_ready_count = 0;

// Called by Limine on each AP
static void ap_entry(struct limine_mp_info *info) {
    int idx = -1;
    for (int i = 0; i < cpu_count; i++) {
        if (cpus[i].lapic_id == (uint32_t)info->lapic_id) { idx = i; break; }
    }
    if (idx < 0) halt();

    init_gdt_for_cpu(idx);
    load_idt_for_cpu();              // ← load shared IDT into this AP's IDTR

    void *stack = vmalloc(32768);
    cpus[idx].kernel_stack = (void*)((uint64_t)stack + 32768);
    tss_set_kernel_stack_for_cpu(idx, cpus[idx].kernel_stack);

    init_sse();                      // Enable SSE/FPU on this AP (CR0/CR4 are per-CPU)
    init_syscalls();                 // Set EFER.SCE + STAR/LSTAR/SFMASK on this AP (MSRs are per-CPU)
    init_apic();
    init_apic_timer(100);

    cpus[idx].active = 1;
    __sync_fetch_and_add(&ap_ready_count, 1);

    sti();
    idle();
}

void init_mp(void) {
    if (current_apic_mode == APIC_NONE) {
        // No APIC, single CPU mode
        cpu_count = 1;
        cpus[0].lapic_id = 0;
        cpus[0].current_task = 0;
        cpus[0].active = 1;
        printf("MP: No APIC, running single CPU.\n");
        return;
    }

    if (!mp_req.response) {
        cpu_count = 1;
        cpus[0].lapic_id = get_apic_id();
        cpus[0].current_task = 0;
        cpus[0].active = 1;
        return;
    }

    struct limine_mp_response *mp = mp_req.response;
    cpu_count = mp->cpu_count;
    if (cpu_count > MAX_CPUS) cpu_count = MAX_CPUS;

    uint32_t bsp_id = get_apic_id();

    // Initialize CPU array
    for (int i = 0; i < cpu_count; i++) {
        cpus[i].lapic_id = mp->cpus[i]->lapic_id;
        cpus[i].current_task = -1;
        cpus[i].active = 0;
    }

    // Mark BSP as active
    for (int i = 0; i < cpu_count; i++) {
        if (cpus[i].lapic_id == bsp_id) {
            cpus[i].active = 1;
            cpus[i].current_task = 0;
            break;
        }
    }

    // Start APs via Limine MP
    for (int i = 0; i < (int)mp->cpu_count && i < MAX_CPUS; i++) {
        if (mp->cpus[i]->lapic_id == bsp_id) continue;
        
        // The goto_address field is used to boot the AP
        __atomic_store_n(&mp->cpus[i]->goto_address, ap_entry, __ATOMIC_SEQ_CST);
    }

    // Wait for all APs to come online (with timeout)
    int expected = cpu_count - 1;
    for (volatile int timeout = 0; timeout < 100000000 && ap_ready_count < expected; timeout++) {
        asm volatile("pause");
    }

    if (ap_ready_count < expected) {
        printf("MP: Warning: only %d/%d APs came online.\n", ap_ready_count, expected);
    } else {
        if (ap_ready_count > 0) {
            printf("MP: All %d APs online.\n", ap_ready_count);
        } else {
            printf("MP: No APs available.\n");
        }
    }
}

cpu_t *get_cpu(void) {
    uint32_t id = get_apic_id();
    for (int i = 0; i < cpu_count; i++) {
        if (cpus[i].lapic_id == id) return &cpus[i];
    }
    return &cpus[0]; // Fallback
}

int get_cpu_index(void) {
    uint32_t id = get_apic_id();
    for (int i = 0; i < cpu_count; i++) {
        if (cpus[i].lapic_id == id) return i;
    }
    return 0;
}
