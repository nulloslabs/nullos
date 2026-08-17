#include <stddef.h>
#include <main/assert.h>
#include <main/spinlocks.h>
#include <mm/kstack.h>
#include <mm/pmm.h>

static bool kstack_slots[KSTACK_SLOT_COUNT];
static spinlock_t kstack_lock = SPINLOCK_INIT;

static uint64_t kstack_slot_base(uint64_t slot) { return KSTACK_AREA_BASE + slot * KSTACK_SLOT_SIZE; }

static int kstack_slot(void *stack) {
    uint64_t address = (uint64_t)stack;
    uint64_t first_stack = KSTACK_AREA_BASE + KSTACK_GUARD_SIZE;
    uint64_t area_size = KSTACK_SLOT_COUNT * KSTACK_SLOT_SIZE;
    if (address < first_stack || address >= KSTACK_AREA_BASE + area_size) return -1;
    uint64_t offset = address - first_stack;
    if (offset % KSTACK_SLOT_SIZE) return -1;
    return offset / KSTACK_SLOT_SIZE;
}

void *alloc_kstack(void) {
    uint64_t irq;
    spin_lock_irqsave(&kstack_lock, &irq);
    int slot = -1;
    for (int i = 0; i < KSTACK_SLOT_COUNT; i++) {
        if (kstack_slots[i]) continue;
        kstack_slots[i] = true;
        slot = i;
        break;
    }
    spin_unlock_irqrestore(&kstack_lock, irq);
    if (slot < 0) return NULL;

    uint64_t stack = kstack_slot_base(slot) + KSTACK_GUARD_SIZE;
    uint64_t mapped = 0;
    for (; mapped < KSTACK_PAGES; mapped++) {
        void *phys = pmalloc();
        if (!phys || !map_vmm(&kernel_context, stack + mapped * PAGE_SIZE, (uint64_t)phys, VMM_WRITABLE | VMM_NX)) {
            if (phys) pfree(phys);
            while (mapped) {
                mapped--;
                unmap_vmm(&kernel_context, stack + mapped * PAGE_SIZE);
            }
            spin_lock_irqsave(&kstack_lock, &irq);
            kstack_slots[slot] = false;
            spin_unlock_irqrestore(&kstack_lock, irq);
            return NULL;
        }
    }
    return (void *)stack;
}

void free_kstack(void *stack) {
    assert(stack != NULL);
    int slot = kstack_slot(stack);
    assert(slot >= 0);
    uint64_t irq;
    spin_lock_irqsave(&kstack_lock, &irq);
    if (!kstack_slots[slot]) {
        spin_unlock_irqrestore(&kstack_lock, irq);
        return;
    }
    for (uint64_t page = 0; page < KSTACK_PAGES; page++) unmap_vmm(&kernel_context, (uint64_t)stack + page * PAGE_SIZE);
    kstack_slots[slot] = false;
    spin_unlock_irqrestore(&kstack_lock, irq);
}

void *kstack_top(void *stack) { return stack ? (void *)((uint64_t)stack + KSTACK_SIZE) : NULL; }

bool is_kstack_guard(uint64_t address) {
    uint64_t area_size = KSTACK_SLOT_COUNT * KSTACK_SLOT_SIZE;
    if (address < KSTACK_AREA_BASE || address >= KSTACK_AREA_BASE + area_size) return false;
    uint64_t offset = (address - KSTACK_AREA_BASE) % KSTACK_SLOT_SIZE;
    return offset < KSTACK_GUARD_SIZE || offset >= KSTACK_GUARD_SIZE + KSTACK_SIZE;
}
