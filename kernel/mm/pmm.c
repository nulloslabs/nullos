#include <main/log.h>
#include <main/panic.h>
#include <main/rng.h>
#include <main/string.h>
#include <main/limine_req.h>
#include <main/spinlocks.h>
#include <main/sched.h>
#include <mm/pmm.h>
#include <mm/oom.h>

static uint32_t *ref_counts = NULL;
static int8_t *page_orders = NULL;
static uint64_t free_heads[PMM_ZONE_COUNT][PMM_MAX_ORDER + 1];
static uint64_t max_pages = 0;
static uint64_t total_pages = 0;
static uint64_t free_pages = 0;
static spinlock_t pmm_lock = SPINLOCK_INIT;

static pmm_zone_t page_zone(uint64_t page) { return page < PMM_DMA32_LIMIT_PAGES ? PMM_ZONE_DMA32 : PMM_ZONE_NORMAL; }

static pmm_buddy_links_t *buddy_links(uint64_t page) { return (void *)(page * PAGE_SIZE + hhdm_req.response->offset); }

static uint8_t pages_order(uint64_t count) {
    uint8_t order = 0;
    uint64_t pages = 1;
    while (pages < count && order < PMM_MAX_ORDER) { pages <<= 1; order++; }
    return order;
}

static void insert_free_block(uint64_t page, uint8_t order, pmm_zone_t zone) {
    pmm_buddy_links_t *links = buddy_links(page);
    links->previous = PMM_INVALID_PAGE;
    links->next = free_heads[zone][order];
    if (links->next != PMM_INVALID_PAGE) buddy_links(links->next)->previous = page;
    free_heads[zone][order] = page;
    page_orders[page] = order;
}

static void remove_free_block(uint64_t page, uint8_t order, pmm_zone_t zone) {
    pmm_buddy_links_t *links = buddy_links(page);
    if (links->previous == PMM_INVALID_PAGE) free_heads[zone][order] = links->next;
    else buddy_links(links->previous)->next = links->next;
    if (links->next != PMM_INVALID_PAGE) buddy_links(links->next)->previous = links->previous;
    page_orders[page] = PMM_PAGE_INTERIOR;
}

static void free_block(uint64_t page, uint8_t order) {
    pmm_zone_t zone = page_zone(page);
    while (order < PMM_MAX_ORDER) {
        uint64_t buddy = page ^ (1ULL << order);
        if (buddy >= max_pages || page_zone(buddy) != zone || page_orders[buddy] != order) break;
        remove_free_block(buddy, order, zone);
        if (buddy < page) page = buddy;
        order++;
    }
    insert_free_block(page, order, zone);
}

static void free_page_range(uint64_t page, uint64_t count) {
    while (count) {
        uint8_t order = 0;
        while (order < PMM_MAX_ORDER) {
            uint64_t next_size = 1ULL << (order + 1);
            if ((page & (next_size - 1)) || next_size > count || page_zone(page) != page_zone(page + next_size - 1)) break;
            order++;
        }
        free_block(page, order);
        uint64_t block_pages = 1ULL << order;
        page += block_pages;
        count -= block_pages;
    }
}

static uint64_t allocate_from_zone(pmm_zone_t zone, uint64_t count) {
    uint8_t wanted_order = pages_order(count);
    uint8_t order = wanted_order;
    while (order <= PMM_MAX_ORDER && free_heads[zone][order] == PMM_INVALID_PAGE) order++;
    if (order > PMM_MAX_ORDER) return PMM_INVALID_PAGE;

    /* Randomize allocation: take a random free block instead of always the
     * list head, so physical page assignment is not predictable. */
    uint64_t page = free_heads[zone][order];
    if (is_rng_seeded()) {
        uint64_t steps;
        get_random_bytes(&steps, sizeof(steps));
        for (uint64_t i = steps % PMM_RANDOMIZE_STEPS; i > 0; i--) {
            uint64_t next = buddy_links(page)->next;
            if (next == PMM_INVALID_PAGE) break;
            page = next;
        }
    }
    remove_free_block(page, order, zone);
    while (order > wanted_order) {
        order--;
        insert_free_block(page + (1ULL << order), order, zone);
    }

    uint64_t block_pages = 1ULL << wanted_order;
    if (count < block_pages) free_page_range(page + count, block_pages - count);
    for (uint64_t i = 0; i < count; i++) {
        page_orders[page + i] = PMM_PAGE_ALLOCATED;
        ref_counts[page + i] = 1;
    }
    free_pages -= count;
    return page;
}

static void *allocate_pages(uint64_t count, bool dma32, bool oom) {
    if (!count || count > max_pages || pages_order(count) > PMM_MAX_ORDER) return NULL;
    uint64_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);
    uint64_t page = dma32 ? allocate_from_zone(PMM_ZONE_DMA32, count) : allocate_from_zone(PMM_ZONE_NORMAL, count);
    if (!dma32 && page == PMM_INVALID_PAGE) page = allocate_from_zone(PMM_ZONE_DMA32, count);
    spin_unlock_irqrestore(&pmm_lock, flags);
    if (page == PMM_INVALID_PAGE) {
        if (!oom) return NULL;
        if (!is_sched_ready()) panic("out of memory");
        spin_unlock(&sched_lock);
        kill_oom();
        spin_lock(&sched_lock);
        return NULL;
    }
    uint64_t phys = page * PAGE_SIZE;
    memset((void *)(phys + hhdm_req.response->offset), 0, count * PAGE_SIZE);
    return (void *)phys;
}

void* pmalloc(void) { return allocate_pages(1, false, true); }

void* pmalloc_dma32(void) { return allocate_pages(1, true, false); }

void* prealloc(uint64_t count) { return allocate_pages(count, false, true); }

void* prealloc_dma32(uint64_t count) { return allocate_pages(count, true, false); }

void pfree(void *phys_addr) {
    if (!phys_addr) return;
    uint64_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);
    uint64_t page_idx = (uint64_t)phys_addr / PAGE_SIZE;
    if (page_idx < max_pages && ref_counts[page_idx] > 0) {
        ref_counts[page_idx]--;
        if (ref_counts[page_idx] == 0) {
            bool was_reserved = page_orders[page_idx] == PMM_PAGE_RESERVED;
            page_orders[page_idx] = PMM_PAGE_ALLOCATED;
            free_block(page_idx, 0);
            if (was_reserved) total_pages++;
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
    for (uint64_t page_idx = start_page / PAGE_SIZE; page_idx < end_page / PAGE_SIZE; page_idx++) {
        if (page_idx < max_pages && ref_counts[page_idx] > 0) {
            bool was_reserved = page_orders[page_idx] == PMM_PAGE_RESERVED;
            ref_counts[page_idx] = 0;
            page_orders[page_idx] = PMM_PAGE_ALLOCATED;
            free_block(page_idx, 0);
            if (was_reserved) total_pages++;
            free_pages++;
        }
    }
    spin_unlock_irqrestore(&pmm_lock, flags);
}

bool pref(void *phys_addr) {
    bool retained = false;
    uint64_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);
    uint64_t page_idx = (uint64_t)phys_addr / PAGE_SIZE;
    if (page_idx < max_pages && ref_counts[page_idx] > 0 && ref_counts[page_idx] < UINT32_MAX) {
        ref_counts[page_idx]++;
        retained = true;
    }
    spin_unlock_irqrestore(&pmm_lock, flags);
    return retained;
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

    // Size allocator metadata from usable RAM, not high MMIO mappings that
    // can never be returned by the PMM.
    uint64_t highest_addr = 0;
    for (size_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry* entry = memmap->entries[i];
        if (entry->type != LIMINE_MEMMAP_USABLE) continue;
        uint64_t top = entry->base + entry->length;
        if (top > highest_addr) highest_addr = top;
    }

    max_pages = highest_addr / PAGE_SIZE;
    total_pages = 0;
    free_pages = 0;
    uint64_t refcount_size = max_pages * sizeof(*ref_counts);
    uint64_t order_size = max_pages * sizeof(*page_orders);
    uint64_t metadata_size = refcount_size + order_size;
    uint64_t metadata_length = (metadata_size + PAGE_SIZE - 1) & ~((uint64_t)PAGE_SIZE - 1);

    for (int zone = 0; zone < PMM_ZONE_COUNT; zone++) {
        for (int order = 0; order <= PMM_MAX_ORDER; order++) free_heads[zone][order] = PMM_INVALID_PAGE;
    }

    // Find a usable region for the per-page buddy metadata.
    for (size_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry* entry = memmap->entries[i];
        uint64_t usable_start = (entry->base + PAGE_SIZE - 1) & ~((uint64_t)PAGE_SIZE - 1);
        uint64_t usable_end = (entry->base + entry->length) & ~((uint64_t)PAGE_SIZE - 1);
        if (entry->type == LIMINE_MEMMAP_USABLE && usable_end >= usable_start && usable_end - usable_start >= metadata_length) {
            ref_counts = (uint32_t *)(usable_start + hhdm_offset);
            page_orders = (int8_t *)ref_counts + refcount_size;
            
            // initially mark everything reserved
            for (uint64_t page = 0; page < max_pages; page++) ref_counts[page] = 1;
            memset(page_orders, PMM_PAGE_RESERVED, order_size);
            
            // Reserve every page touched by the allocator metadata.
            entry->base = usable_start + metadata_length;
            entry->length = usable_end - entry->base;
            break;
        }
    }

    if (!ref_counts) panic("unable to reserve physical memory metadata");

    // mark usable regions as free
    for (size_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry* entry = memmap->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE) {
            uint64_t usable_start = (entry->base + PAGE_SIZE - 1) & ~((uint64_t)PAGE_SIZE - 1);
            uint64_t usable_end = (entry->base + entry->length) & ~((uint64_t)PAGE_SIZE - 1);
            uint64_t first_page = usable_start / PAGE_SIZE;
            uint64_t end_page = usable_end / PAGE_SIZE;
            if (first_page == 0) first_page = 1;
            if (end_page > max_pages) end_page = max_pages;
            if (first_page >= end_page) continue;
            uint64_t count = end_page - first_page;
            memset(ref_counts + first_page, 0, count * sizeof(*ref_counts));
            free_page_range(first_page, count);
            total_pages += count;
            free_pages += count;
        }
    }
    log("pmm: initialized pmm\n");
}
