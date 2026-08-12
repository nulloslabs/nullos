#include <main/log.h>
#include <main/panic.h>
#include <main/string.h>
#include <main/limine_req.h>
#include <main/spinlocks.h>
#include <main/sched.h>
#include <mm/pmm.h>
#include <mm/oom.h>
static uint8_t* bitmap = NULL;
static uint8_t* ref_counts = NULL;
static uint64_t max_pages = 0;
static uint64_t last_index = 0; // For optimization
static uint64_t total_pages = 0;
static uint64_t free_pages = 0;
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
            free_pages--;
            last_index = idx;
            uint64_t phys = idx * PAGE_SIZE;
            spin_unlock_irqrestore(&pmm_lock, flags);
            memset((void*)(phys + hhdm_req.response->offset), 0, PAGE_SIZE);
            return (void*)phys; // Returns PHYSICAL address
        }
    }
    spin_unlock_irqrestore(&pmm_lock, flags);
    if (!is_sched_ready()) panic("out of memory");
    // sched_lock is held by syscall_entry across the whole syscall;
    // kill_oom() needs to acquire it internally, so release it first.
    spin_unlock(&sched_lock);
    kill_oom();
    spin_lock(&sched_lock);
    return NULL; // OOM
}

void* pmalloc_dma32(void) {
    uint64_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);

    uint64_t dma32_pages = max_pages;
    if (dma32_pages > (1ULL << 20)) dma32_pages = 1ULL << 20;
    for (uint64_t idx = 0; idx < dma32_pages; idx++) {
        if (!(bitmap[idx / 8] & (1 << (idx % 8)))) {
            bitmap[idx / 8] |= (1 << (idx % 8));
            ref_counts[idx] = 1;
            free_pages--;
            last_index = idx + 1;
            uint64_t phys = idx * PAGE_SIZE;
            spin_unlock_irqrestore(&pmm_lock, flags);
            memset((void*)(phys + hhdm_req.response->offset), 0, PAGE_SIZE);
            return (void*)phys;
        }
    }

    spin_unlock_irqrestore(&pmm_lock, flags);
    return NULL;
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
            free_pages -= count;
            last_index = idx + count;
            spin_unlock_irqrestore(&pmm_lock, flags);
            return (void*)(idx * PAGE_SIZE);
        }
    }
    spin_unlock_irqrestore(&pmm_lock, flags);
    if (!is_sched_ready()) panic("out of memory");
    spin_unlock(&sched_lock);
    kill_oom();
    spin_lock(&sched_lock);
    return NULL;
}

void* prealloc_dma32(uint64_t count) {
    if (count == 0 || count > (1ULL << 20)) return NULL;
    uint64_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);
    uint64_t dma32_pages = max_pages;
    if (dma32_pages > (1ULL << 20)) dma32_pages = 1ULL << 20;
    for (uint64_t idx = 0; idx + count <= dma32_pages; idx++) {
        bool available = true;
        for (uint64_t page = 0; page < count; page++) {
            if (bitmap[(idx + page) / 8] & (1 << ((idx + page) % 8))) { available = false; break; }
        }
        if (!available) continue;
        for (uint64_t page = 0; page < count; page++) {
            bitmap[(idx + page) / 8] |= 1 << ((idx + page) % 8);
            ref_counts[idx + page] = 1;
        }
        free_pages -= count;
        last_index = idx + count;
        uint64_t phys = idx * PAGE_SIZE;
        spin_unlock_irqrestore(&pmm_lock, flags);
        memset((void *)(phys + hhdm_req.response->offset), 0, count * PAGE_SIZE);
        return (void *)phys;
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
            free_pages++;
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
            free_pages++;
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

uint64_t get_total_pmm_memory(void) {
    uint64_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);
    uint64_t memory = total_pages * PAGE_SIZE;
    spin_unlock_irqrestore(&pmm_lock, flags);
    return memory;
}

uint64_t get_free_pmm_memory(void) {
    uint64_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);
    uint64_t memory = free_pages * PAGE_SIZE;
    spin_unlock_irqrestore(&pmm_lock, flags);
    return memory;
}

uint64_t get_used_pmm_memory(void) {
    uint64_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);
    uint64_t memory = (total_pages - free_pages) * PAGE_SIZE;
    spin_unlock_irqrestore(&pmm_lock, flags);
    return memory;
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
    total_pages = 0;
    free_pages = 0;
    uint64_t bitmap_size = (max_pages + 7) / 8;
    uint64_t refcount_size = max_pages;
    uint64_t metadata_size = bitmap_size + refcount_size;
    uint64_t metadata_length = (metadata_size + PAGE_SIZE - 1) & ~((uint64_t)PAGE_SIZE - 1);

    // find usable hole for bitmap
    for (size_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry* entry = memmap->entries[i];
        uint64_t usable_start = (entry->base + PAGE_SIZE - 1) & ~((uint64_t)PAGE_SIZE - 1);
        uint64_t usable_end = (entry->base + entry->length) & ~((uint64_t)PAGE_SIZE - 1);
        if (entry->type == LIMINE_MEMMAP_USABLE && usable_end >= usable_start && usable_end - usable_start >= metadata_length) {
            // Place bitmap in virtual address space via HHDM
            bitmap = (uint8_t*)(usable_start + hhdm_offset);
            ref_counts = bitmap + bitmap_size;
            
            // initially mark everything reserved
            memset(bitmap, 0xFF, bitmap_size);
            memset(ref_counts, 1, refcount_size);
            
            // Reserve every page touched by the allocator metadata.
            entry->base = usable_start + metadata_length;
            entry->length = usable_end - entry->base;
            break;
        }
    }

    if (!bitmap) panic("unable to reserve physical memory metadata");

    // mark usable regions as free
    for (size_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry* entry = memmap->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE) {
            uint64_t usable_start = (entry->base + PAGE_SIZE - 1) & ~((uint64_t)PAGE_SIZE - 1);
            uint64_t usable_end = (entry->base + entry->length) & ~((uint64_t)PAGE_SIZE - 1);
            for (uint64_t address = usable_start; address < usable_end; address += PAGE_SIZE) {
                uint64_t page_idx = address / PAGE_SIZE;
                if (page_idx == 0) continue;
                if (page_idx < max_pages && (bitmap[page_idx / 8] & (1 << (page_idx % 8)))) {
                    bitmap[page_idx / 8] &= ~(1 << (page_idx % 8));
                    ref_counts[page_idx] = 0;
                    total_pages++;
                    free_pages++;
                }
            }
        }
    }
    log("pmm: initialized pmm\n");
}
