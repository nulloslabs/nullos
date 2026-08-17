#include <stdint.h>
#include <errno.h>
#include <main/assert.h>
#include <main/sched.h>
#include <main/string.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <mm/vma.h>
#include <mm/pf.h>

int handle_pf(uint64_t cr2, uint64_t error_code) {
    assert(current_task_ptr != NULL);
    // error_code bit 0: 0 = not-present fault, 1 = protection violation
    if (error_code & 1) return -EACCES;

    uint64_t page_addr = cr2 & ~(uint64_t)(PAGE_SIZE - 1);

    // Check if the PTE has our VMM_DEMAND marker, meaning mmap reserved
    // this address but hasn't backed it with a physical page yet.
    uint64_t pte = get_vmm_pte(current_task_ptr->ctx, page_addr);
    if (!(pte & VMM_DEMAND)) return -EFAULT;

    // Recover the permissions stored in the PTE by reserve_vmm.
    // Since present=0, all bits were free for us to use.
    uint64_t stored_flags = pte & ~VMM_DEMAND;

    // Allocate a physical page.
    void *phys = pmalloc();
    if (!phys) return -ENOMEM;

    // Zero it via the HHDM so the process sees a clean page.
    memset(phys_to_virt((uint64_t)phys), 0, PAGE_SIZE);
    map_vmm(current_task_ptr->ctx, page_addr, (uint64_t)phys, stored_flags);

    return 0;
}
