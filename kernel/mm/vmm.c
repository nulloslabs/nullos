#include <mm/vmm.h>
#include <mm/pmm.h>
#include <mm/mm.h>
#include <main/string.h>
#include <main/limine_req.h>
#include <io/terminal.h>
#include <main/spinlock.h>

vmm_context_t kernel_context;
static uint64_t vmalloc_cursor = 0xffffc00000000000;
static uint64_t vuser_cursor   = 0x0000700000000000;
static spinlock_t vmm_lock = SPINLOCK_INIT;

// Helper: Get virtual address of a physical page using HHDM
void* phys_to_virt(uint64_t phys) {
    return (void*)(phys + hhdm_req.response->offset);
}

uint64_t virt_to_phys(void* virt) {
    uintptr_t addr = (uintptr_t)virt;
    
    // Check if it's in the kernel executable range
    if (addr >= 0xffffffff80000000) {
        if (eaddr_req.response) {
            return addr - eaddr_req.response->virtual_base + eaddr_req.response->physical_base;
        }
    }
    
    // Check if it's in the vmalloc range (e.g. malloc buffers pushing into controllers)
    if (addr >= 0xffffc00000000000 && addr < 0xffffffff80000000) {
        return get_vmm_phys(&kernel_context, addr);
    }
    
    // HHDM (Higher Half Direct Map) range
    return addr - hhdm_req.response->offset;
}

// Helper: Walk the table and return the entry for a virtual address.
// When allocating, 'flags' are OR'd into existing intermediate entries so that
// e.g. VMM_USER propagates through PML4E -> PDPTE -> PDE for user-mode pages.
static uint64_t* get_vmm_next_level(uint64_t* current_level, uint64_t index, bool allocate, uint64_t flags) {
    if (!current_level) {
        return NULL;
    }
    if (current_level[index] & VMM_PRESENT) {
        // Propagate permission bits (User, Writable) to existing intermediate entries
        current_level[index] |= (flags & (VMM_WRITABLE | VMM_USER));
        return (uint64_t*)phys_to_virt(current_level[index] & ~0xFFFULL);
    }

    if (!allocate) return NULL;

    void* next_level_phys = pmalloc();
    if (!next_level_phys) return NULL;

    uint64_t* next_level_virt = (uint64_t*)phys_to_virt((uint64_t)next_level_phys);
    memset(next_level_virt, 0, PAGE_SIZE);

    // Set the entry in the current table pointing to the new one
    current_level[index] = (uint64_t)next_level_phys | VMM_PRESENT | VMM_WRITABLE | VMM_USER;
    
    return next_level_virt;
}

// Make an existing page mapping accessible from user mode (Ring 3).
// Walks the page table and ORs VMM_USER into every level (PML4E, PDPTE, PDE, PTE).
// Correctly handles 2MB and 1GB large pages (PS bit = bit 7).
void set_vmm_user(vmm_context_t* ctx, uint64_t virt) {
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    // PML4E
    if (!(ctx->pml4[pml4_idx] & VMM_PRESENT)) return;
    ctx->pml4[pml4_idx] |= VMM_USER;
    uint64_t* pdpt = (uint64_t*)phys_to_virt(ctx->pml4[pml4_idx] & ~0xFFFULL);

    // PDPTE — could be a 1GB large page
    if (!(pdpt[pdpt_idx] & VMM_PRESENT)) return;
    pdpt[pdpt_idx] |= VMM_USER;
    if (pdpt[pdpt_idx] & (1ULL << 7)) goto flush; // 1GB page, done
    uint64_t* pd = (uint64_t*)phys_to_virt(pdpt[pdpt_idx] & ~0xFFFULL);

    // PDE — could be a 2MB large page
    if (!(pd[pd_idx] & VMM_PRESENT)) return;
    pd[pd_idx] |= VMM_USER;
    if (pd[pd_idx] & (1ULL << 7)) goto flush; // 2MB page, done
    uint64_t* pt = (uint64_t*)phys_to_virt(pd[pd_idx] & ~0xFFFULL);

    // PTE — 4KB page
    if (!(pt[pt_idx] & VMM_PRESENT)) return;
    pt[pt_idx] |= VMM_USER;

flush:
    asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

bool map_vmm(vmm_context_t* ctx, uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t flags_irq;
    spin_lock_irqsave(&vmm_lock, &flags_irq);

    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    uint64_t* pdpt = get_vmm_next_level(ctx->pml4, pml4_idx, true, flags);
    if (!pdpt) { spin_unlock_irqrestore(&vmm_lock, flags_irq); return false; }
    uint64_t* pd   = get_vmm_next_level(pdpt, pdpt_idx, true, flags);
    if (!pd)   { spin_unlock_irqrestore(&vmm_lock, flags_irq); return false; }
    uint64_t* pt   = get_vmm_next_level(pd, pd_idx, true, flags);
    if (!pt)   { spin_unlock_irqrestore(&vmm_lock, flags_irq); return false; }

    pt[pt_idx] = phys | flags | VMM_PRESENT;
    
    // Invalidate the TLB for this address
    asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
    
    spin_unlock_irqrestore(&vmm_lock, flags_irq);
    return true;
}

void unmap_vmm(vmm_context_t* ctx, uint64_t virt) {
    uint64_t flags_irq;
    spin_lock_irqsave(&vmm_lock, &flags_irq);

    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx = (virt >> 21) & 0x1FF;
    uint64_t pt_idx = (virt >> 12) & 0x1FF;

    uint64_t* pdpt = get_vmm_next_level(ctx->pml4, pml4_idx, false, 0);
    if (!pdpt) goto out;
    uint64_t* pd = get_vmm_next_level(pdpt, pdpt_idx, false, 0);
    if (!pd) goto out;
    uint64_t* pt = get_vmm_next_level(pd, pd_idx, false, 0);
    if (!pt) goto out;

    // Get the physical address so we can free it in the PMM
    uint64_t phys = pt[pt_idx] & ~0xFFFULL;
    if (phys) {
        pfree((void*)phys);
    }

    // Clear the entry and flush TLB
    pt[pt_idx] = 0;
    asm volatile("invlpg (%0)" : : "r"(virt) : "memory");

out:
    spin_unlock_irqrestore(&vmm_lock, flags_irq);
}

uint64_t get_vmm_phys(vmm_context_t* ctx, uint64_t virt) {
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx = (virt >> 21) & 0x1FF;
    uint64_t pt_idx = (virt >> 12) & 0x1FF;
    uint64_t offset = virt & 0xFFF;

    // Walk the levels without allocating new tables
    uint64_t* pdpt = get_vmm_next_level(ctx->pml4, pml4_idx, false, 0);
    if (!pdpt) return 0;

    uint64_t* pd = get_vmm_next_level(pdpt, pdpt_idx, false, 0);
    if (!pd) return 0;

    uint64_t* pt = get_vmm_next_level(pd, pd_idx, false, 0);
    if (!pt) return 0;

    // The entry contains the physical address + flags
    uint64_t entry = pt[pt_idx];
    if (!(entry & VMM_PRESENT)) return 0;

    // Mask out the flags to get the pure physical address, then add the page offset
    return (entry & ~0xFFFULL) + offset;
}

void read_vmm(vmm_context_t* ctx, void* dest, uint64_t virt_src, size_t size) {
    uint8_t* d = (uint8_t*)dest;
    size_t remaining = size;
    uint64_t curr_src = virt_src;

    while (remaining > 0) {
        uint64_t phys = get_vmm_phys(ctx, curr_src & ~0xFFFULL);
        if (!phys) return;

        uint64_t offset = curr_src & 0xFFF;
        size_t to_copy = 4096 - offset;
        if (to_copy > remaining) to_copy = remaining;

        memcpy(d, (uint8_t*)phys_to_virt(phys) + offset, to_copy);

        d += to_copy;
        curr_src += to_copy;
        remaining -= to_copy;
    }
}

void write_vmm(vmm_context_t* ctx, uint64_t virt_dest, const void* src, size_t size) {
    const uint8_t* s = (const uint8_t*)src;
    size_t remaining = size;
    uint64_t curr_dest = virt_dest;

    while (remaining > 0) {
        uint64_t phys = get_vmm_phys(ctx, curr_dest & ~0xFFFULL);
        if (!phys) return;

        uint64_t offset = curr_dest & 0xFFF;
        size_t to_copy = 4096 - offset;
        if (to_copy > remaining) to_copy = remaining;

        memcpy((uint8_t*)phys_to_virt(phys) + offset, s, to_copy);

        s += to_copy;
        curr_dest += to_copy;
        remaining -= to_copy;
    }
}

void memset_vmm(vmm_context_t* ctx, uint64_t virt_dest, int val, size_t size) {
    size_t remaining = size;
    uint64_t curr_dest = virt_dest;

    while (remaining > 0) {
        uint64_t phys = get_vmm_phys(ctx, curr_dest & ~0xFFFULL);
        if (!phys) return;

        uint64_t offset = curr_dest & 0xFFF;
        size_t to_copy = 4096 - offset;
        if (to_copy > remaining) to_copy = remaining;

        memset((uint8_t*)phys_to_virt(phys) + offset, val, to_copy);

        curr_dest += to_copy;
        remaining -= to_copy;
    }
}

void switch_vmm_context(vmm_context_t* ctx) {
    // Get physical address of the PML4 (remove the HHDM offset)
    uint64_t phys_pml4 = (uint64_t)ctx->pml4 - hhdm_req.response->offset;
    asm volatile("mov %0, %%cr3" : : "r"(phys_pml4) : "memory");
}

vmm_context_t* create_vmm_context(void) {
    // allocate context
    vmm_context_t* ctx = malloc(sizeof(vmm_context_t));
    if (!ctx) return NULL;

    // allocate physical page for PML4
    void* pml4_raw = pmalloc();
    if (!pml4_raw) {
        free(ctx);
        return NULL;
    }
    uint64_t pml4_phys = (uint64_t)pml4_raw;

    // get virtual address
    ctx->pml4 = (uint64_t*)phys_to_virt(pml4_phys);

    // zero it out
    memset(ctx->pml4, 0, PAGE_SIZE);

    // copy kernel mappings (top 256 entries)
    for (int i = 256; i < 512; i++) {
        ctx->pml4[i] = kernel_context.pml4[i];
    }

    return ctx;
}

vmm_context_t* clone_vmm_context(vmm_context_t* parent) {
    if (!parent) return NULL;
    if (!parent->pml4) {
        return NULL;
    }

    vmm_context_t* child = create_vmm_context();
    if (!child) return NULL;

    // Traverse the parent's page tables and copy all user pages (indices 0-255)
    for (uint64_t pml4_i = 0; pml4_i < 256; pml4_i++) {
        if (!(parent->pml4[pml4_i] & VMM_PRESENT)) continue;
        uint64_t* pdpt = (uint64_t*)phys_to_virt(parent->pml4[pml4_i] & ~0xFFFULL);

        for (uint64_t pdpt_i = 0; pdpt_i < 512; pdpt_i++) {
            if (!(pdpt[pdpt_i] & VMM_PRESENT)) continue;
            // Assuming no 1GB pages in user space
            uint64_t* pd = (uint64_t*)phys_to_virt(pdpt[pdpt_i] & ~0xFFFULL);

            for (uint64_t pd_i = 0; pd_i < 512; pd_i++) {
                if (!(pd[pd_i] & VMM_PRESENT)) continue;
                // Assuming no 2MB pages in user space mapping (vmm_map only makes 4K pages)
                uint64_t* pt = (uint64_t*)phys_to_virt(pd[pd_i] & ~0xFFFULL);

                for (uint64_t pt_i = 0; pt_i < 512; pt_i++) {
                    uint64_t entry = pt[pt_i];
                    if (!(entry & VMM_PRESENT)) continue;
                    if (!(entry & VMM_USER)) continue; // Only copy user pages

                    // Calculate the original virtual address
                    uint64_t virt = (pml4_i << 39) | (pdpt_i << 30) | (pd_i << 21) | (pt_i << 12);

                    // Allocate a new physical page for the child
                    void* new_phys = pmalloc();
                    if (!new_phys) {
                        // In a real OS, we should rollback and free everything. 
                        // For now, just return what we have or crash.
                        return NULL;
                    }

                    // Copy the 4KB data from the parent's physical page to the new one
                    uint64_t old_phys = entry & ~0xFFFULL;
                    memcpy(phys_to_virt((uint64_t)new_phys), phys_to_virt(old_phys), PAGE_SIZE);

                    // Map it in the child context exactly identically
                    // vmm_map will automatically allocate the required intermediate tables
                    uint64_t flags = entry & 0xFFF; // Extract lower 12 bits out of the entry
                    map_vmm(child, virt, (uint64_t)new_phys, flags & ~VMM_PRESENT);
                }
            }
        }
    }

    return child;
}

void init_vmm(void) {
    // Read the current CR3 (Limine's page tables)
    uint64_t current_cr3;
    asm volatile("mov %%cr3, %0" : "=r"(current_cr3));
    
    // Use Limine's existing page tables as kernel_context
    kernel_context.pml4 = (uint64_t*)phys_to_virt(current_cr3);
    printf("VMM: Initialized VMM.\n");
}

void* vmalloc_ex(vmm_context_t* ctx, size_t size, uint64_t flags) {
    if (size == 0) return NULL;

    uint64_t total_size = size + sizeof(vmalloc_header_t);
    uint64_t num_pages = (total_size + 4095) / 4096;
    
    uint64_t flags_irq;
    spin_lock_irqsave(&vmm_lock, &flags_irq);
    
    // Check if another vmalloc on another core moved the cursor while we waited
    uint64_t *cursor = (flags & VMM_USER) ? &vuser_cursor : &vmalloc_cursor;
    void* start_addr = (void*)*cursor;
    
    // We reserve the virtual address space immediately so it's thread-safe
    *cursor += (num_pages * PAGE_SIZE);
    
    spin_unlock_irqrestore(&vmm_lock, flags_irq);

    void* first_phys = NULL;
    uint64_t curr_addr = (uint64_t)start_addr;

    for (uint64_t i = 0; i < num_pages; i++) {
        void* phys = pmalloc();
        if (!phys) return NULL; 
        if (i == 0) first_phys = phys;

        map_vmm(ctx, curr_addr, (uint64_t)phys, flags | VMM_PRESENT);
        curr_addr += PAGE_SIZE;
    }

    // Heed the warning: 'start_addr' is a virtual address in 'ctx', which might 
    // not be the current address space. Use HHDM to write the header directly.
    vmalloc_header_t* header = (vmalloc_header_t*)phys_to_virt((uint64_t)first_phys);
    header->page_count = num_pages;

    return (void*)((uintptr_t)start_addr + sizeof(vmalloc_header_t));
}

void* vmap_mmio(uint64_t phys, size_t num_pages) {
    if (num_pages == 0) return NULL;
    
    uint64_t flags_irq;
    spin_lock_irqsave(&vmm_lock, &flags_irq);
    
    void* start_addr = (void*)vmalloc_cursor;
    vmalloc_cursor += (num_pages * PAGE_SIZE);
    
    spin_unlock_irqrestore(&vmm_lock, flags_irq);

    uint64_t curr_addr = (uint64_t)start_addr;
    uint64_t curr_phys = phys & ~0xFFFULL; // Align to page

    for (uint64_t i = 0; i < num_pages; i++) {
        map_vmm(&kernel_context, curr_addr, curr_phys, VMM_PRESENT | VMM_WRITABLE | VMM_PWT | VMM_PCD);
        curr_addr += PAGE_SIZE;
        curr_phys += PAGE_SIZE;
    }

    return (void*)((uintptr_t)start_addr + (phys & 0xFFFULL)); // Return aligned + offset
}

void* vmalloc_user_ex(vmm_context_t* ctx, size_t size) {
    return vmalloc_ex(ctx, size, VMM_WRITABLE | VMM_USER);
}

void* vmalloc(size_t size) {
    return vmalloc_ex(&kernel_context, size, VMM_WRITABLE);
}

void* vmalloc_user(size_t size) {
    return vmalloc_user_ex(&kernel_context, size);
}

void* vrealloc(void* ptr, size_t size) {
    if (!ptr) return vmalloc(size);
    
    vmalloc_header_t* header = (vmalloc_header_t*)((uintptr_t)ptr - sizeof(vmalloc_header_t));
    size_t old_data_size = (header->page_count * PAGE_SIZE) - sizeof(vmalloc_header_t);

    uint64_t new_total_size = size + sizeof(vmalloc_header_t);
    uint64_t new_num_pages = (new_total_size + 4095) / 4096;

    if (new_num_pages <= header->page_count) {
        // We have enough pages already, just update metadata if needed
        return ptr; 
    }

    // Otherwise, do the full move
    void* new_ptr = vmalloc(size);
    if (!new_ptr) return NULL;

    memcpy(new_ptr, ptr, old_data_size);
    vfree(ptr);

    return new_ptr;
}

void vfree(void* ptr) {
    if (!ptr) return;

    vmalloc_header_t* header = (vmalloc_header_t*)((uintptr_t)ptr - sizeof(vmalloc_header_t));
    uint64_t virt = (uintptr_t)header;

    for (uint64_t i = 0; i < header->page_count; i++) {
        // Get the physical address
        uint64_t phys = get_vmm_phys(&kernel_context, virt + (i * PAGE_SIZE));
        
        // Free the physical page
        if (phys) {
            pfree((void*)phys);
        }
    }
}