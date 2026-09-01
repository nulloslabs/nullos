#include <stdbool.h>
#include <main/assert.h>
#include <main/log.h>
#include <main/rng.h>
#include <main/mp.h>
#include <main/idt.h>
#include <main/gdt.h>
#include <main/halt.h>
#include <main/sched.h>
#include <main/string.h>
#include <main/limine_req.h>
#include <main/sse.h>
#include <io/apic.h>
#include <io/ioapic.h>
#include <mm/mm.h>
#include <mm/kstack.h>
#include <mm/vmm.h>
#include <mm/smap.h>
#include <mm/smep.h>
#include <syscalls/syscalls.h>

cpu_t cpus[MAX_CPUS];
int cpu_count = 0;
volatile int ap_ready_count = 0;
cpu_index_map_entry_t cpu_index_map[CPU_INDEX_MAP_SIZE];

// Called by Limine on each AP
static void ap_entry(struct limine_mp_info *info) {
    int idx = -1;
    for (int i = 0; i < cpu_count; i++) {
        if (cpus[i].lapic_id == (uint32_t)info->lapic_id) { idx = i; break; }
    }
    assert(idx >= 0 && idx < cpu_count);

    init_gdt_for_cpu(idx);
    load_idt_for_cpu();
    init_apic_for_cpu();

    cpus[idx].kstack = cpus[idx].task->kstack;
    if (!cpus[idx].kstack) halt();
    set_tss_kstack_for_cpu(idx, kstack_top(cpus[idx].kstack));

    init_sse_for_cpu();
    enable_smap_for_cpu();
    enable_smep_for_cpu();
    init_syscalls_for_cpu();
    start_apic_timer_for_cpu();

    cpus[idx].active = 1;
    __sync_fetch_and_add(&ap_ready_count, 1);

    sti();
    idle();
}

uint32_t hash_cpu_index(uint32_t lapic_id) {
    static uint64_t seed;
    if (!seed) get_random_bytes(&seed, sizeof(seed));
    #define GOLDEN_RATIO 0x9E3779B97F4A7C15ULL
    return (((uint64_t)lapic_id ^ seed) * GOLDEN_RATIO) % CPU_INDEX_MAP_SIZE;
    #undef GOLDEN_RATIO
}

void clear_cpu_index_map(void) {
    for (int i = 0; i < CPU_INDEX_MAP_SIZE; i++) {
        cpu_index_map[i].used = false;
    }
}

void map_cpu_index(uint32_t lapic_id, int cpu_index) {
    assert(cpu_index >= 0 && cpu_index < MAX_CPUS);
    uint32_t slot = hash_cpu_index(lapic_id);

    for (int i = 0; i < CPU_INDEX_MAP_SIZE; i++) {
        cpu_index_map_entry_t *entry = &cpu_index_map[slot];
        if (!entry->used || entry->lapic_id == lapic_id) {
            entry->lapic_id = lapic_id;
            entry->cpu_index = cpu_index;
            entry->used = true;
            return;
        }
        slot = (slot + 1) % CPU_INDEX_MAP_SIZE;
    }
}

int get_cpu_index(void) {
    uint32_t id = get_apic_id();

    uint32_t slot = hash_cpu_index(id);
    for (int i = 0; i < CPU_INDEX_MAP_SIZE; i++) {
        cpu_index_map_entry_t *entry = &cpu_index_map[slot];
        if (!entry->used) break;
        if (entry->lapic_id == id) return entry->cpu_index;
        slot = (slot + 1) % CPU_INDEX_MAP_SIZE;
    }

    return 0;
}

cpu_t *get_cpu(void) {
    return &cpus[get_cpu_index()];
}

void init_mp(void) {
    clear_cpu_index_map();

    if (current_apic_mode == APIC_NONE) {
        // No APIC, single CPU mode
        cpu_count = 1;
        cpus[0].lapic_id = 0;
        cpus[0].task_index = current_task;
        cpus[0].task = current_task_ptr;
        cpus[0].idle_task = current_task;
        cpus[0].active = 1;
        cpus[0].rq_head = NULL;
        cpus[0].rq_count = 0;
        map_cpu_index(0, 0);
        log("mp: no apic, running single cpu\n");
        return;
    }

    if (!mp_req.response) {
        cpu_count = 1;
        cpus[0].lapic_id = get_apic_id();
        cpus[0].task_index = current_task;
        cpus[0].task = current_task_ptr;
        cpus[0].idle_task = current_task;
        cpus[0].active = 1;
        cpus[0].rq_head = NULL;
        cpus[0].rq_count = 0;
        map_cpu_index(cpus[0].lapic_id, 0);
        return;
    }

    struct limine_mp_response *mp = mp_req.response;
    cpu_count = mp->cpu_count;
    if (cpu_count > MAX_CPUS) cpu_count = MAX_CPUS;

    uint32_t bsp_id = get_apic_id();
    int bsp_task_index = current_task;
    task_t *bsp_task = current_task_ptr;

    // Initialize CPU array
    for (int i = 0; i < cpu_count; i++) {
        cpus[i].lapic_id = mp->cpus[i]->lapic_id;
        cpus[i].task_index = -1;
        cpus[i].task = NULL;
        cpus[i].idle_task = -1;
        cpus[i].minimum_virtual_runtime = 0;
        cpus[i].rq_head = NULL;
        cpus[i].rq_count = 0;
        cpus[i].active = 0;
        map_cpu_index(cpus[i].lapic_id, i);
    }

    // Mark BSP as active
    for (int i = 0; i < cpu_count; i++) {
        if (cpus[i].lapic_id == bsp_id) {
            cpus[i].active = 1;
            cpus[i].task_index = bsp_task_index;
            cpus[i].task = bsp_task;
            cpus[i].idle_task = bsp_task_index;
            break;
        }
    }

    for (int i = 0; i < cpu_count; i++) {
        if (cpus[i].lapic_id != bsp_id) prepare_scheduler_cpu(i);
    }

    // Start APs via Limine MP
    for (int i = 0; i < (int)mp->cpu_count && i < MAX_CPUS; i++) {
        if (mp->cpus[i]->lapic_id == bsp_id) continue;
        
        // The goto_address field is used to boot the AP
        __atomic_store_n(&mp->cpus[i]->goto_address, ap_entry, __ATOMIC_SEQ_CST);
    }

    // Wait for all APs to come online (with timeout)
    int expected = cpu_count - 1;
    for (volatile int timeout = 0; timeout < 100000000 && ap_ready_count < expected; timeout++) __asm__ volatile ("pause");

    if (ap_ready_count < expected) {
        log("mp: warning: only %d/%d aps came online\n", ap_ready_count, expected);
    } else {
        if (ap_ready_count > 0) {
            log("mp: all %d aps online\n", ap_ready_count);
        } else {
            log("mp: no aps available\n");
        }
    }
}
