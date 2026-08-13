#include <stddef.h>
#include <main/spinlocks.h>
#include <mm/kstack.h>
#include <mm/pmm.h>

static bool kernel_stack_slots[KERNEL_STACK_SLOT_COUNT];
static spinlock_t kernel_stack_lock = SPINLOCK_INIT;

static uint64_t kernel_stack_slot_base(uint64_t slot) { return KERNEL_STACK_AREA_BASE + slot * KERNEL_STACK_SLOT_SIZE; }

static int kernel_stack_slot(void *stack) {
    uint64_t address = (uint64_t)stack;
    uint64_t first_stack = KERNEL_STACK_AREA_BASE + KERNEL_STACK_GUARD_SIZE;
    uint64_t area_size = KERNEL_STACK_SLOT_COUNT * KERNEL_STACK_SLOT_SIZE;
    if (address < first_stack || address >= KERNEL_STACK_AREA_BASE + area_size) return -1;
    uint64_t offset = address - first_stack;
    if (offset % KERNEL_STACK_SLOT_SIZE) return -1;
    return offset / KERNEL_STACK_SLOT_SIZE;
}

void *alloc_kernel_stack(void) {
    uint64_t irq;
    spin_lock_irqsave(&kernel_stack_lock, &irq);
    int slot = -1;
    for (int i = 0; i < KERNEL_STACK_SLOT_COUNT; i++) {
        if (kernel_stack_slots[i]) continue;
        kernel_stack_slots[i] = true;
        slot = i;
        break;
    }
    spin_unlock_irqrestore(&kernel_stack_lock, irq);
    if (slot < 0) return NULL;

    uint64_t stack = kernel_stack_slot_base(slot) + KERNEL_STACK_GUARD_SIZE;
    uint64_t mapped = 0;
    for (; mapped < KERNEL_STACK_PAGES; mapped++) {
        void *phys = pmalloc();
        if (!phys || !map_vmm(&kernel_context, stack + mapped * PAGE_SIZE, (uint64_t)phys, VMM_WRITABLE | VMM_NX)) {
            if (phys) pfree(phys);
            while (mapped) {
                mapped--;
                unmap_vmm(&kernel_context, stack + mapped * PAGE_SIZE);
            }
            spin_lock_irqsave(&kernel_stack_lock, &irq);
            kernel_stack_slots[slot] = false;
            spin_unlock_irqrestore(&kernel_stack_lock, irq);
            return NULL;
        }
    }
    return (void *)stack;
}

void free_kernel_stack(void *stack) {
    int slot = kernel_stack_slot(stack);
    if (slot < 0) return;
    uint64_t irq;
    spin_lock_irqsave(&kernel_stack_lock, &irq);
    if (!kernel_stack_slots[slot]) {
        spin_unlock_irqrestore(&kernel_stack_lock, irq);
        return;
    }
    for (uint64_t page = 0; page < KERNEL_STACK_PAGES; page++) unmap_vmm(&kernel_context, (uint64_t)stack + page * PAGE_SIZE);
    kernel_stack_slots[slot] = false;
    spin_unlock_irqrestore(&kernel_stack_lock, irq);
}

void *kernel_stack_top(void *stack) { return stack ? (void *)((uint64_t)stack + KERNEL_STACK_SIZE) : NULL; }

bool is_kernel_stack_guard(uint64_t address) {
    uint64_t area_size = KERNEL_STACK_SLOT_COUNT * KERNEL_STACK_SLOT_SIZE;
    if (address < KERNEL_STACK_AREA_BASE || address >= KERNEL_STACK_AREA_BASE + area_size) return false;
    uint64_t offset = (address - KERNEL_STACK_AREA_BASE) % KERNEL_STACK_SLOT_SIZE;
    return offset < KERNEL_STACK_GUARD_SIZE || offset >= KERNEL_STACK_GUARD_SIZE + KERNEL_STACK_SIZE;
}
