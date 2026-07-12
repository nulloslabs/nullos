#include <mm/pmm.h>
#include <main/string.h>
#include <main/limine_req.h>
#include <main/spinlocks.h>
#include <main/log.h>

static uint8_t* bitmap = NULL;
static uint8_t* ref_counts = NULL;
static uint64_t max_pages = 0;
static uint64_t last_index = 0; // For optimization
static spinlock_t pmm_lock = SPINLOCK_INIT;

void* pmalloc(void) {
    uint64_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);

    // Simple Next-Fit search for a free bit
    for (uint64_t i = 0; i < max_pages; i++) {
        uint64_t idx = (last_index + i) % max_pages;
        if (!(bitmap[idx / 8] & (1 << (idx % 8)))) {
            bitmap[idx / 8] |= (1 << (idx % 8)); // Mark used
            ref_counts[idx] = 1;
            last_index = idx;
            uint64_t phys = idx * PAGE_SIZE;
            spin_unlock_irqrestore(&pmm_lock, flags);

            // Zero the page via the HHDM so callers never observe stale data
            // left over from a previous owner.  This is critical for user
            // pages: this kernel lacks a destroy_vmm_context() path, so pages
            // reaching the PMM via sys_munmap/exec can still hold old lock
            // words, and pthread mutexes interpret a leftover 2 (lll
            // "contended" state) as "another thread owns this", deadlocking
            // single-threaded programs.
            memset((void*)(phys + hhdm_req.response->offset), 0, PAGE_SIZE);

            return (void*)phys; // Returns PHYSICAL address
        }
    }
    spin_unlock_irqrestore(&pmm_lock, flags);
    return NULL; // OOM
}

void* prealloc(uint64_t count) {
    if (count == 0) return NULL;
    uint64_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);

    // Search for `count` contiguous free pages starting at last_index
    for (uint64_t i = 0; i < max_pages; i++) {
        uint64_t idx = (last_index + i) % max_pages;
        // Can't wrap around: need idx..idx+count-1 all in range
        if (idx + count > max_pages) continue;
        int ok = 1;
        for (uint64_t j = 0; j < count; j++) {
            if (bitmap[(idx + j) / 8] & (1 << ((idx + j) % 8))) { ok = 0; break; }
        }
        if (ok) {
            for (uint64_t j = 0; j < count; j++) {
                bitmap[(idx + j) / 8] |= (1 << ((idx + j) % 8));
                ref_counts[idx + j] = 1;
            }
            last_index = idx + count;
            spin_unlock_irqrestore(&pmm_lock, flags);
            return (void*)(idx * PAGE_SIZE);
        }
    }
    spin_unlock_irqrestore(&pmm_lock, flags);
    return NULL;
}

void pfree(void *phys_addr) {
    uint64_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);
    uint64_t page_idx = (uint64_t)phys_addr / PAGE_SIZE;
    if (page_idx < max_pages && ref_counts[page_idx] > 0) {
        ref_counts[page_idx]--;
        if (ref_counts[page_idx] == 0) {
            bitmap[page_idx / 8] &= ~(1 << (page_idx % 8));
        }
    }
    spin_unlock_irqrestore(&pmm_lock, flags);
}

void pfree_range(void *phys_addr, uint64_t size) {
    if (!phys_addr || size == 0) return;
    uint64_t start = (uint64_t)phys_addr;
    // Align the start UP to a page boundary so we never free a partially-
    // shared page (e.g. a page holding tail data we don't own).
    uint64_t start_page = (start + PAGE_SIZE - 1) & ~((uint64_t)PAGE_SIZE - 1);
    uint64_t end_page   = (start + size) & ~((uint64_t)PAGE_SIZE - 1);

    uint64_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);
    for (uint64_t pa = start_page; pa < end_page; pa += PAGE_SIZE) {
        uint64_t page_idx = pa / PAGE_SIZE;
        if (page_idx < max_pages && ref_counts[page_idx] > 0) {
            ref_counts[page_idx] = 0;
            bitmap[page_idx / 8] &= ~(1 << (page_idx % 8));
        }
    }
    spin_unlock_irqrestore(&pmm_lock, flags);
}

void pref(void *phys_addr) {
    uint64_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);
    uint64_t page_idx = (uint64_t)phys_addr / PAGE_SIZE;
    if (page_idx < max_pages && ref_counts[page_idx] > 0 && ref_counts[page_idx] < 255) {
        ref_counts[page_idx]++;
    }
    spin_unlock_irqrestore(&pmm_lock, flags);
}

void init_pmm(void) {
    struct limine_memmap_response* memmap = mm_req.response;
    uint64_t hhdm_offset = hhdm_req.response->offset;

    // find top of memory
    uint64_t highest_addr = 0;
    for (size_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry* entry = memmap->entries[i];
        uint64_t top = entry->base + entry->length;
        if (top > highest_addr) highest_addr = top;
    }

    max_pages = highest_addr / PAGE_SIZE;
    uint64_t bitmap_size = (max_pages + 7) / 8;
    uint64_t refcount_size = max_pages;

    // find usable hole for bitmap
    for (size_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry* entry = memmap->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE && entry->length >= bitmap_size + refcount_size) {
            // Place bitmap in virtual address space via HHDM
            bitmap = (uint8_t*)(entry->base + hhdm_offset);
            ref_counts = (uint8_t*)(entry->base + hhdm_offset + bitmap_size);
            
            // initially mark everything reserved
            memset(bitmap, 0xFF, bitmap_size);
            memset(ref_counts, 1, refcount_size);
            
            // shrink entry to protect bitmap memory
            entry->base += bitmap_size + refcount_size;
            entry->length -= bitmap_size + refcount_size;
            break;
        }
    }

    // mark usable regions as free
    for (size_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry* entry = memmap->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE) {
            for (uint64_t j = 0; j < entry->length; j += PAGE_SIZE) {
                uint64_t page_idx = (entry->base + j) / PAGE_SIZE;
                // Clear the bit
                bitmap[page_idx / 8] &= ~(1 << (page_idx % 8));
                ref_counts[page_idx] = 0;
            }
        }
    }
    log("initialized pmm");
}
