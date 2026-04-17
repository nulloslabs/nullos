#include <mm/pmm.h>
#include <main/string.h>
#include <main/limine_req.h>
#include <io/terminal.h>
#include <main/spinlock.h>

static uint8_t* bitmap = NULL;
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
            last_index = idx;
            spin_unlock_irqrestore(&pmm_lock, flags);
            return (void*)(idx * PAGE_SIZE); // Returns PHYSICAL address
        }
    }
    spin_unlock_irqrestore(&pmm_lock, flags);
    return NULL; // OOM
}

void pfree(void *phys_addr) {
    uint64_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);
    uint64_t page_idx = (uint64_t)phys_addr / PAGE_SIZE;
    bitmap[page_idx / 8] &= ~(1 << (page_idx % 8));
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
    uint64_t bitmap_size = max_pages / 8;

    // find usable hole for bitmap
    for (size_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry* entry = memmap->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE && entry->length >= bitmap_size) {
            // Place bitmap in virtual address space via HHDM
            bitmap = (uint8_t*)(entry->base + hhdm_offset);
            
            // initially mark everything reserved
            memset(bitmap, 0xFF, bitmap_size);
            
            // shrink entry to protect bitmap memory
            entry->base += bitmap_size;
            entry->length -= bitmap_size;
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
            }
        }
    }
    printf("PMM: Initialized PMM.\n");
}