#include <stdbool.h>
#include <main/log.h>
#include <main/string.h>
#include <main/limine_req.h>
#include <main/spinlocks.h>
#include <main/machine_info.h>
#include <main/msr.h>
#include <main/panic.h>
#include <mm/smap.h>
#include <mm/smep.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <mm/mm.h>

static uint64_t vmalloc_cursor = 0xffffc00000000000;
static uint64_t vuser_cursor = USER_MMAP_BASE;
static uint64_t vuser32_cursor = USER_MMAP32_BASE;
static spinlock_t vmm_lock = SPINLOCK_INIT;

vmm_context_t kernel_context;

static uint64_t* get_vmm_next_level(uint64_t* current_level, uint64_t index, bool allocate, uint64_t flags) {
    if (!current_level) { return NULL; }
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

static uint64_t get_or_fault_vmm_phys(vmm_context_t* ctx, uint64_t virt) {
    uint64_t phys = get_vmm_phys(ctx, virt);
    if (phys != 0) return phys;

    uint64_t page_addr = virt & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t pte = get_vmm_pte(ctx, page_addr);
    if (pte & VMM_DEMAND) {
        uint64_t stored_flags = pte & ~VMM_DEMAND;
        void *p = pmalloc();
        if (!p) return 0;
        memset(phys_to_virt((uint64_t)p), 0, PAGE_SIZE);
        map_vmm(ctx, page_addr, (uint64_t)p, stored_flags);
        return (uint64_t)p + (virt & (PAGE_SIZE - 1));
    }
    return 0;
}

static bool vmm_user_page_valid(vmm_context_t *ctx, uint64_t virt, bool write) {
    if (!ctx || !ctx->pml4) return false;
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx = (virt >> 21) & 0x1FF;
    uint64_t pt_idx = (virt >> 12) & 0x1FF;
    uint64_t required_upper = VMM_PRESENT | VMM_USER;
    if (write) required_upper |= VMM_WRITABLE;

    uint64_t pml4e = ctx->pml4[pml4_idx];
    if ((pml4e & required_upper) != required_upper) return false;
    uint64_t *pdpt = (uint64_t *)phys_to_virt(pml4e & 0x000ffffffffff000ULL);
    uint64_t pdpte = pdpt[pdpt_idx];
    if ((pdpte & required_upper) != required_upper) return false;
    if (pdpte & (1ULL << 7)) return true;
    uint64_t *pd = (uint64_t *)phys_to_virt(pdpte & 0x000ffffffffff000ULL);
    uint64_t pde = pd[pd_idx];
    if ((pde & required_upper) != required_upper) return false;
    if (pde & (1ULL << 7)) return true;
    uint64_t *pt = (uint64_t *)phys_to_virt(pde & 0x000ffffffffff000ULL);

    uint64_t pte = pt[pt_idx];
    if (pte & VMM_PRESENT) {
        return (pte & required_upper) == required_upper;
    }
    if (pte & VMM_DEMAND) {
        uint64_t required_user = VMM_USER;
        if (write) required_user |= VMM_WRITABLE;
        return (pte & required_user) == required_user;
    }
    return false;
}

// Helper: Get virtual address of a physical page using HHDM
void* phys_to_virt(uint64_t phys) { return (void*)(phys + hhdm_req.response->offset); }

uint64_t virt_to_phys(void* virt) {
    uintptr_t addr = (uintptr_t)virt;
    
    // Check if it's in the kernel executable range
    if (addr >= 0xffffffff80000000) { if (eaddr_req.response) { return addr - eaddr_req.response->virtual_base + eaddr_req.response->physical_base; } }
    
    if (addr >= KERNEL_HEAP_BASE && addr < KERNEL_HEAP_LIMIT) { return get_vmm_phys(&kernel_context, addr); }

    // Check if it's in the vmalloc range (e.g. malloc buffers pushing into controllers)
    if (addr >= 0xffffc00000000000 && addr < 0xffffffff80000000) { return get_vmm_phys(&kernel_context, addr); }
    
    // HHDM (Higher Half Direct Map) range
    return addr - hhdm_req.response->offset;
}

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
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
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
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
    
    spin_unlock_irqrestore(&vmm_lock, flags_irq);
    return true;
}

bool reserve_vmm(vmm_context_t* ctx, uint64_t virt, uint64_t flags) {
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

    // Store flags in PTE with VMM_DEMAND marker but without VMM_PRESENT.
    // The CPU ignores all bits when present=0, so we can freely use them
    // to remember what permissions this page should have once faulted in.
    pt[pt_idx] = (flags & ~VMM_PRESENT) | VMM_DEMAND;
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");

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
    // Mask out flag bits: low 12 (page flags) and bit 63 (NX)
    uint64_t entry = pt[pt_idx];
    uint64_t phys = entry & 0x000ffffffffff000ULL;
    if (phys && !(entry & VMM_EXTERNAL)) { pfree((void*)phys); }

    // Clear the entry and flush TLB
    pt[pt_idx] = 0;
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");

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
    uint64_t phys = (entry & 0x000ffffffffff000ULL) + offset;
    return phys;
}

// Read raw PTE (including non-present demand entries)
uint64_t get_vmm_pte(vmm_context_t* ctx, uint64_t virt) {
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    uint64_t* pdpt = get_vmm_next_level(ctx->pml4, pml4_idx, false, 0);
    if (!pdpt) return 0;
    uint64_t* pd = get_vmm_next_level(pdpt, pdpt_idx, false, 0);
    if (!pd) return 0;
    uint64_t* pt = get_vmm_next_level(pd, pd_idx, false, 0);
    if (!pt) return 0;
    return pt[pt_idx];
}

bool vmm_user_range_valid(vmm_context_t *ctx, uint64_t addr, size_t size, bool write) {
    if (!ctx || !ctx->pml4) return false;
    if (size == 0) return true;
    if (addr >= 0x0000800000000000ULL || size > 0x0000800000000000ULL - addr) return false;

    uint64_t page = addr & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t last = (addr + size - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    for (;;) {
        if (!vmm_user_page_valid(ctx, page, write)) return false;
        if (page == last) break;
        page += PAGE_SIZE;
    }
    return true;
}



void read_vmm(vmm_context_t* ctx, void* dest, uint64_t virt_src, size_t size) {
    uint8_t* d = (uint8_t*)dest;
    size_t remaining = size;
    uint64_t curr_src = virt_src;

    while (remaining > 0) {
        uint64_t phys = get_or_fault_vmm_phys(ctx, curr_src);
        if (!phys) return;

        uint64_t offset = curr_src & 0xFFF;
        size_t to_copy = 4096 - offset;
        if (to_copy > remaining) to_copy = remaining;

        memcpy(d, (uint8_t*)phys_to_virt(phys & ~0xFFFULL) + offset, to_copy);

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
        uint64_t phys = get_or_fault_vmm_phys(ctx, curr_dest);
        if (!phys) return;

        uint64_t offset = curr_dest & 0xFFF;
        size_t to_copy = 4096 - offset;
        if (to_copy > remaining) to_copy = remaining;

        memcpy((uint8_t*)phys_to_virt(phys & ~0xFFFULL) + offset, s, to_copy);

        s += to_copy;
        curr_dest += to_copy;
        remaining -= to_copy;
    }
}

void memset_vmm(vmm_context_t* ctx, uint64_t virt_dest, int val, size_t size) {
    size_t remaining = size;
    uint64_t curr_dest = virt_dest;

    while (remaining > 0) {
        uint64_t phys = get_or_fault_vmm_phys(ctx, curr_dest);
        if (!phys) return;

        uint64_t offset = curr_dest & 0xFFF;
        size_t to_copy = 4096 - offset;
        if (to_copy > remaining) to_copy = remaining;

        memset((uint8_t*)phys_to_virt(phys & ~0xFFFULL) + offset, val, to_copy);

        curr_dest += to_copy;
        remaining -= to_copy;
    }
}

void switch_vmm_context(vmm_context_t* ctx) {
    // Get physical address of the PML4 (remove the HHDM offset)
    uint64_t phys_pml4 = (uint64_t)ctx->pml4 - hhdm_req.response->offset;
    __asm__ volatile("mov %0, %%cr3" : : "r"(phys_pml4) : "memory");
}

vmm_context_t* create_vmm_context(void) {
    // allocate context
    vmm_context_t* ctx = malloc(sizeof(vmm_context_t));
    if (!ctx) return NULL;

    // allocate physical page for PML4
    void* pml4_raw = pmalloc();
    if (!pml4_raw) { free(ctx); return NULL; }

    // get virtual address
    ctx->pml4 = (uint64_t*)phys_to_virt((uint64_t)pml4_raw);
    ctx->refcount = 1;
    ctx->mmap_pages = 0;
    init_vma_table(&ctx->vmas);

    // zero it out
    memset(ctx->pml4, 0, PAGE_SIZE);

    // copy kernel mappings (top 256 entries)
    for (int i = 256; i < 512; i++) { ctx->pml4[i] = kernel_context.pml4[i]; }

    return ctx;
}

bool retain_vmm_context(vmm_context_t* ctx) {
    if (!ctx || ctx == &kernel_context) return ctx == &kernel_context;
    uint32_t refs = __atomic_load_n(&ctx->refcount, __ATOMIC_ACQUIRE);
    while (refs != 0 && refs != UINT32_MAX) {
        if (__atomic_compare_exchange_n(&ctx->refcount, &refs, refs + 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return true;
    }
    return false;
}

void destroy_vmm_context(vmm_context_t* ctx) {
    if (!ctx || ctx == &kernel_context) return;
    uint32_t refs = __atomic_load_n(&ctx->refcount, __ATOMIC_ACQUIRE);
    for (;;) {
        if (refs == 0) return;
        if (__atomic_compare_exchange_n(&ctx->refcount, &refs, refs - 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) break;
    }
    if (refs != 1) return;
    if (!ctx->pml4) { free(ctx); return; }
    for (int i = 0; i < 256; i++) {
        if (ctx->pml4[i] & VMM_PRESENT) {
            uint64_t pdpt_phys = ctx->pml4[i] & 0x000ffffffffff000ULL;
            uint64_t* pdpt = (uint64_t*)phys_to_virt(pdpt_phys);
            for (int j = 0; j < 512; j++) {
                if (pdpt[j] & VMM_PRESENT) {
                    if (pdpt[j] & (1ULL << 7)) continue;
                    uint64_t pd_phys = pdpt[j] & 0x000ffffffffff000ULL;
                    uint64_t* pd = (uint64_t*)phys_to_virt(pd_phys);
                    for (int k = 0; k < 512; k++) {
                        if (pd[k] & VMM_PRESENT) {
                            if (pd[k] & (1ULL << 7)) continue;
                            uint64_t pt_phys = pd[k] & 0x000ffffffffff000ULL;
                            uint64_t* pt = (uint64_t*)phys_to_virt(pt_phys);
                            for (int l = 0; l < 512; l++) {
                                if ((pt[l] & VMM_PRESENT) && !(pt[l] & VMM_EXTERNAL)) {
                                    pfree((void*)(pt[l] & 0x000ffffffffff000ULL));
                                }
                            }
                            pfree((void*)pt_phys);
                        }
                    }
                    pfree((void*)pd_phys);
                }
            }
            pfree((void*)pdpt_phys);
        }
    }
    pfree((void*)((uint64_t)ctx->pml4 - hhdm_req.response->offset));
    free(ctx);
}

vmm_context_t* clone_vmm_context(vmm_context_t* parent) {
    if (!parent) return NULL;
    if (!parent->pml4) { return NULL; }

    vmm_context_t* child = create_vmm_context();
    if (!child) return NULL;
    child->mmap_pages = parent->mmap_pages;
    memcpy(&child->vmas, &parent->vmas, sizeof(child->vmas));

    // Traverse the parent's page tables and copy all user pages (indices 0-255)
    for (uint64_t pml4_i = 0; pml4_i < 256; pml4_i++) {
        if (!(parent->pml4[pml4_i] & VMM_PRESENT)) continue;
        uint64_t* pdpt = (uint64_t*)phys_to_virt(parent->pml4[pml4_i] & ~0xFFFULL);

        for (uint64_t pdpt_i = 0; pdpt_i < 512; pdpt_i++) {
            if (!(pdpt[pdpt_i] & VMM_PRESENT)) continue;
            uint64_t* pd = (uint64_t*)phys_to_virt(pdpt[pdpt_i] & ~0xFFFULL);

            for (uint64_t pd_i = 0; pd_i < 512; pd_i++) {
                if (!(pd[pd_i] & VMM_PRESENT)) continue;
                uint64_t* pt = (uint64_t*)phys_to_virt(pd[pd_i] & ~0xFFFULL);

                for (uint64_t pt_i = 0; pt_i < 512; pt_i++) {
                    uint64_t entry = pt[pt_i];
                    uint64_t virt = (pml4_i << 39) | (pdpt_i << 30) | (pd_i << 21) | (pt_i << 12);

                    if (!(entry & VMM_PRESENT)) {
                        if (entry & VMM_DEMAND) {
                            reserve_vmm(child, virt, entry & ~VMM_PRESENT);
                        }
                        continue;
                    }
                    if (!(entry & VMM_USER)) continue;

                    if (entry & VMM_EXTERNAL) {
                        uint64_t phys = entry & 0x000ffffffffff000ULL;
                        map_vmm(child, virt, phys, entry & (0xFFFULL | VMM_NX) & ~VMM_PRESENT);
                        continue;
                    }

                    if (entry & VMM_SHARED) {
                        uint64_t phys = entry & 0x000ffffffffff000ULL;
                        if (!pref((void*)phys)) {
                            destroy_vmm_context(child);
                            return NULL;
                        }
                        if (!map_vmm(child, virt, phys, entry & (0xFFFULL | VMM_NX) & ~VMM_PRESENT)) {
                            pfree((void*)phys);
                            destroy_vmm_context(child);
                            return NULL;
                        }
                        continue;
                    }

                    void* new_phys = pmalloc();
                    if (!new_phys) {
                        destroy_vmm_context(child);
                        return NULL;
                    }

                    uint64_t old_phys = entry & 0x000ffffffffff000ULL;
                    memcpy(phys_to_virt((uint64_t)new_phys), phys_to_virt(old_phys), PAGE_SIZE);

                    uint64_t flags = entry & (0xFFFULL | VMM_NX);
                    if (!map_vmm(child, virt, (uint64_t)new_phys, flags & ~VMM_PRESENT)) {
                        pfree(new_phys);
                        destroy_vmm_context(child);
                        return NULL;
                    }
                }
            }
        }
    }

    return child;
}

void* vmalloc_ex(vmm_context_t* ctx, size_t size, uint64_t flags) {
    if (size == 0) return NULL;

    uint64_t total_size = size + sizeof(vmalloc_header_t);
    uint64_t num_pages = (total_size + 4095) / 4096;
    
    uint64_t flags_irq;
    spin_lock_irqsave(&vmm_lock, &flags_irq);
    
    uint64_t *cursor = (flags & VMM_USER) ? &vuser_cursor : &vmalloc_cursor;
    void* start_addr = (void*)*cursor;
    *cursor += (num_pages * PAGE_SIZE);
    
    spin_unlock_irqrestore(&vmm_lock, flags_irq);

    void* first_phys = NULL;
    uint64_t curr_addr = (uint64_t)start_addr;
    uint64_t mapped_count = 0;

    for (uint64_t i = 0; i < num_pages; i++) {
        void* phys = pmalloc();
        if (!phys) {
            // Rollback: unmap and free all previously allocated pages
            uint64_t rollback_addr = (uint64_t)start_addr;
            for (uint64_t j = 0; j < mapped_count; j++) {
                unmap_vmm(ctx, rollback_addr);
                rollback_addr += PAGE_SIZE;
            }
            return NULL;
        }
        if (i == 0) first_phys = phys;

        if (!map_vmm(ctx, curr_addr, (uint64_t)phys, flags | VMM_PRESENT)) {
            pfree(phys);
            uint64_t rollback_addr = (uint64_t)start_addr;
            for (uint64_t j = 0; j < mapped_count; j++) {
                unmap_vmm(ctx, rollback_addr);
                rollback_addr += PAGE_SIZE;
            }
            return NULL;
        }
        curr_addr += PAGE_SIZE;
        mapped_count++;
    }

    vmalloc_header_t* header = (vmalloc_header_t*)phys_to_virt((uint64_t)first_phys);
    header->page_count = num_pages;

    return (void*)((uintptr_t)start_addr + sizeof(vmalloc_header_t));
}

void* vmalloc_user_ex(vmm_context_t* ctx, size_t size) { return vmalloc_ex(ctx, size, VMM_WRITABLE | VMM_USER | VMM_NX); }

void* vmap_mmio(uint64_t phys, size_t num_pages) {
    if (num_pages == 0) return NULL;

    uint64_t flags_irq;
    spin_lock_irqsave(&vmm_lock, &flags_irq);

    void* start_addr = (void*)vmalloc_cursor;
    vmalloc_cursor += num_pages * PAGE_SIZE;

    spin_unlock_irqrestore(&vmm_lock, flags_irq);

    uint64_t curr_addr = (uint64_t)start_addr;
    uint64_t curr_phys = phys & ~(PAGE_SIZE - 1);

    for (size_t i = 0; i < num_pages; i++) {
        if (!map_vmm(&kernel_context, curr_addr, curr_phys, VMM_WRITABLE | VMM_PWT | VMM_PCD | VMM_NX | VMM_EXTERNAL)) {
            for (size_t j = 0; j < i; j++) unmap_vmm(&kernel_context, (uint64_t)start_addr + j * PAGE_SIZE);
            return NULL;
        }

        curr_addr += PAGE_SIZE;
        curr_phys += PAGE_SIZE;
    }

    return (void*)((uintptr_t)start_addr + (phys & (PAGE_SIZE - 1)));
}

void vunmap_mmio(void* addr, size_t num_pages) {
    if (!addr || num_pages == 0) return;

    uint64_t base = (uintptr_t)addr & ~(PAGE_SIZE - 1);

    for (size_t i = 0; i < num_pages; i++) {
        unmap_vmm(&kernel_context, base + i * PAGE_SIZE);
    }
}

void* vmap_user_at(vmm_context_t* ctx, uint64_t virt, size_t size, uint64_t flags) {
    if (!ctx || size == 0 || virt >= USER_VIRTUAL_LIMIT || size > USER_VIRTUAL_LIMIT - virt) return NULL;
    uint64_t num_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t curr_addr = virt & ~0xFFFULL;
    uint64_t map_size = num_pages * PAGE_SIZE;
    if (curr_addr > USER_VIRTUAL_LIMIT || map_size > USER_VIRTUAL_LIMIT - curr_addr) return NULL;

    for (uint64_t i = 0; i < num_pages; i++) {
        uint64_t phys = get_vmm_phys(ctx, curr_addr);
        if (phys != 0) {
            // Page is already physically backed (e.g. MAP_FIXED over existing):
            // just update its flags in-place.
            if (!map_vmm(ctx, curr_addr, phys, flags | VMM_PRESENT | VMM_USER)) {
                for (uint64_t j = 0; j < i; j++) unmap_vmm(ctx, (virt & ~0xFFFULL) + j * PAGE_SIZE);
                return NULL;
            }
        } else {
            // Not yet backed: reserve for demand paging.
            // The physical page is allocated lazily on first access (#PF).
            if (!reserve_vmm(ctx, curr_addr, flags | VMM_USER)) {
                for (uint64_t j = 0; j < i; j++) unmap_vmm(ctx, (virt & ~0xFFFULL) + j * PAGE_SIZE);
                return NULL;
            }
        }
        curr_addr += PAGE_SIZE;
    }
    return (void*)virt;
}

void* vmap_user_range(vmm_context_t* ctx, size_t size, uint64_t flags) {
    if (!ctx || size == 0 || size > SIZE_MAX - (PAGE_SIZE - 1)) return NULL;

    uint64_t num_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t map_size = num_pages * PAGE_SIZE;

    uint64_t flags_irq;
    spin_lock_irqsave(&vmm_lock, &flags_irq);

    uint64_t start_addr = vuser_cursor;
    if (start_addr >= USER_VIRTUAL_LIMIT || map_size > USER_VIRTUAL_LIMIT - start_addr) {
        spin_unlock_irqrestore(&vmm_lock, flags_irq);
        return NULL;
    }
    vuser_cursor += map_size;

    spin_unlock_irqrestore(&vmm_lock, flags_irq);

    return vmap_user_at(ctx, start_addr, map_size, flags);
}

void* vmap_user_range_32(vmm_context_t* ctx, size_t size, uint64_t flags) {
    if (!ctx || size == 0 || size > SIZE_MAX - (PAGE_SIZE - 1)) return NULL;
    uint64_t num_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t map_size = num_pages * PAGE_SIZE;
    uint64_t flags_irq;
    spin_lock_irqsave(&vmm_lock, &flags_irq);
    uint64_t start_addr = vuser32_cursor;
    uint64_t end_addr;
    for (;;) {
        end_addr = start_addr + map_size;
        if (end_addr < start_addr || end_addr > 0x80000000ULL) {
            spin_unlock_irqrestore(&vmm_lock, flags_irq);
            return NULL;
        }
        bool occupied = false;
        for (uint64_t page = start_addr; page < end_addr; page += PAGE_SIZE) {
            uint64_t pte = get_vmm_pte(ctx, page);
            if (pte & (VMM_PRESENT | VMM_DEMAND)) { occupied = true; break; }
        }
        if (!occupied) break;
        start_addr = end_addr;
    }
    if (end_addr > 0x80000000ULL) {
        spin_unlock_irqrestore(&vmm_lock, flags_irq);
        return NULL;
    }
    vuser32_cursor = end_addr;
    spin_unlock_irqrestore(&vmm_lock, flags_irq);
    return vmap_user_at(ctx, start_addr, map_size, flags);
}

void* vmalloc(size_t size) { return vmalloc_ex(&kernel_context, size, VMM_WRITABLE | VMM_NX); }

void* vmalloc_user(size_t size) { return vmalloc_user_ex(&kernel_context, size); }

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
    uint64_t count = header->page_count;

    for (uint64_t i = 0; i < count; i++) { unmap_vmm(&kernel_context, virt + (i * PAGE_SIZE)); }
}

void init_vmm(void) {
    // Check if we have the NX bit (mandatory for our OS)
    // NOTE: It is called XD on Intel CPUs but because AMD invented it first, lets stay loyal to AMD.
    if (!cpu_has_feature(CPU_FEATURE_NX)) panic("cpu doesn't support the nx bit");

    // Enable the NX/XD bit
    uint64_t efer = read_msr(MSR_EFER);
    write_msr(MSR_EFER, efer | MSR_EFER_NXE);

    // Make supervisor writes obey read-only PTEs as well.
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 1ULL << 16;
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
    enable_smap();
    enable_smep();

    uint64_t current_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(current_cr3));
    
    kernel_context.pml4 = (uint64_t*)phys_to_virt(current_cr3);

    // Pre-allocate all kernel half PDPTs (indices 256 to 511) so they are shared
    // across all cloned user contexts. This prevents synchronization issues where
    // a vmalloc in one context isn't visible in another because the PML4 entry was empty.
    for (int i = 256; i < 512; i++) {
        get_vmm_next_level(kernel_context.pml4, i, true, VMM_WRITABLE);
    }

    log("vmm: initialized vmm\n");
}
