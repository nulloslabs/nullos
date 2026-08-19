#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <main/assert.h>
#include <main/log.h>
#include <main/string.h>
#include <main/panic.h>
#include <main/limine_req.h>
#include <main/spinlocks.h>
#include <mm/mm.h>
#include <mm/pmm.h>
#include <mm/vmm.h>

#define HEAP_ALIGNMENT       16ULL
#define HEAP_INITIAL_GROW    (64 * 1024ULL)
#define HEAP_MIN_GROW        (64 * 1024ULL)
#define HEAP_MIN_SPLIT       16ULL
#define HEAP_BLOCK_MAGIC     0x4e554c4c48454150ULL
#define KERNEL_HEAP_BASE     0xffffb00000000000ULL
#define KERNEL_HEAP_LIMIT    0xffffc00000000000ULL

#define ALIGN_UP(value, alignment) \
    (((value) + ((alignment) - 1)) & ~((alignment) - 1))

struct memory_header *free_list_start = NULL;
uint64_t hhdm_offset = 0;

static spinlock_t mm_lock = SPINLOCK_INIT;
static uint64_t kernel_heap_cursor = KERNEL_HEAP_BASE;
static bool heap_ready = false;

static bool add_overflow_size(size_t a, size_t b, size_t *out) {
    if (a > (size_t)-1 - b) return true;
    *out = a + b;
    return false;
}

static struct memory_header *find_header_locked(void *ptr) {
    if (!ptr) return NULL;
    struct memory_header *wanted = (struct memory_header *)ptr - 1;
    for (struct memory_header *block = free_list_start; block; block = block->next) {
        assert(block->magic == HEAP_BLOCK_MAGIC);
        if (block == wanted) return block;
    }
    return NULL;
}

static void coalesce_free_list_locked(void) {
    struct memory_header *block = free_list_start;
    while (block && block->next) {
        assert(block->magic == HEAP_BLOCK_MAGIC);
        uint8_t *end = (uint8_t *)(block + 1) + block->size;
        if (block->is_free && block->next->is_free && end == (uint8_t *)block->next) {
            block->size += sizeof(struct memory_header) + block->next->size;
            block->next = block->next->next;
            continue;
        }
        block = block->next;
    }
}

static void insert_region_locked(void *region, size_t region_size) {
    struct memory_header *block = (struct memory_header *)region;
    block->size = region_size - sizeof(*block);
    block->magic = HEAP_BLOCK_MAGIC;
    block->is_free = 1;
    block->next = NULL;

    if (!free_list_start || block < free_list_start) {
        block->next = free_list_start;
        free_list_start = block;
    } else {
        struct memory_header *prev = free_list_start;
        while (prev->next && prev->next < block) prev = prev->next;
        block->next = prev->next;
        prev->next = block;
    }
    coalesce_free_list_locked();
}

static void split_block_locked(struct memory_header *block, size_t wanted) {
    assert(block != NULL);
    assert(block->magic == HEAP_BLOCK_MAGIC);
    assert(block->size >= wanted);

    // The caller retains ownership state of the first part.  malloc() splits
    // a free block, while realloc() can split an allocated block; in either
    // case the newly-created tail is free.
    size_t remainder = block->size - wanted;
    if (remainder < sizeof(struct memory_header) + HEAP_MIN_SPLIT) return;

    struct memory_header *tail = (struct memory_header *)
        ((uint8_t *)(block + 1) + wanted);
    tail->size = remainder - sizeof(*tail);
    tail->magic = HEAP_BLOCK_MAGIC;
    tail->is_free = 1;
    tail->next = block->next;

    block->size = wanted;
    block->next = tail;
}

static bool grow_kernel_heap(size_t wanted) {
    size_t total;
    if (add_overflow_size(wanted, sizeof(struct memory_header), &total)) return false;
    if (total > (size_t)-1 - (PAGE_SIZE - 1)) return false;

    size_t grow_size = ALIGN_UP(total, PAGE_SIZE);
    if (grow_size < HEAP_MIN_GROW) grow_size = HEAP_MIN_GROW;

    uint64_t irq;
    spin_lock_irqsave(&mm_lock, &irq);
    uint64_t start = kernel_heap_cursor;
    if (start >= KERNEL_HEAP_LIMIT || grow_size > KERNEL_HEAP_LIMIT - start) {
        spin_unlock_irqrestore(&mm_lock, irq);
        return false;
    }
    kernel_heap_cursor += grow_size;
    spin_unlock_irqrestore(&mm_lock, irq);

    size_t mapped = 0;
    while (mapped < grow_size) {
        void *phys = pmalloc();
        if (!phys) break;
        if (!map_vmm(&kernel_context, start + mapped, (uint64_t)phys, VMM_WRITABLE | VMM_NX)) {
            pfree(phys);
            break;
        }
        mapped += PAGE_SIZE;
    }

    if (mapped != grow_size) {
        while (mapped > 0) {
            mapped -= PAGE_SIZE;
            unmap_vmm(&kernel_context, start + mapped);
        }
        return false;
    }

    // pmalloc() already zeroes every page. Publish the fully mapped region
    // only after all mappings succeed, so allocation never sees half a span.
    spin_lock_irqsave(&mm_lock, &irq);
    insert_region_locked((void *)start, grow_size);
    spin_unlock_irqrestore(&mm_lock, irq);
    return true;
}

void *malloc(size_t size) {
    if (!heap_ready) return NULL;
    if (size == 0) size = 1;
    if (size > (size_t)-1 - (HEAP_ALIGNMENT - 1)) return NULL;
    size = ALIGN_UP(size, HEAP_ALIGNMENT);

    for (;;) {
        uint64_t irq;
        spin_lock_irqsave(&mm_lock, &irq);

        // Best fit limits fragmentation while retaining a simple address-
        // ordered list, which makes adjacent-block coalescing inexpensive.
        struct memory_header *best = NULL;
        for (struct memory_header *block = free_list_start; block; block = block->next) {
            if (!block->is_free || block->size < size) continue;
            if (!best || block->size < best->size) best = block;
            if (block->size == size) break;
        }

        if (best) {
            split_block_locked(best, size);
            best->is_free = 0;
            spin_unlock_irqrestore(&mm_lock, irq);
            return best + 1;
        }

        spin_unlock_irqrestore(&mm_lock, irq);
        if (!grow_kernel_heap(size)) return NULL;
    }
}

void free(void *ptr) {
    if (!ptr) return;

    uint64_t irq;
    spin_lock_irqsave(&mm_lock, &irq);
    struct memory_header *block = find_header_locked(ptr);
    if (block) {
        if (!block->is_free) {
            block->is_free = 1;
            coalesce_free_list_locked();
        }
        spin_unlock_irqrestore(&mm_lock, irq);
        return;
    }
    spin_unlock_irqrestore(&mm_lock, irq);

    // vmalloc allocations occupy the distinct range above KERNEL_HEAP_LIMIT.
    if ((uintptr_t)ptr >= KERNEL_HEAP_LIMIT) vfree(ptr);
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) {
        free(ptr);
        return NULL;
    }
    if (size > (size_t)-1 - (HEAP_ALIGNMENT - 1)) return NULL;
    size = ALIGN_UP(size, HEAP_ALIGNMENT);

    uint64_t irq;
    spin_lock_irqsave(&mm_lock, &irq);
    struct memory_header *block = find_header_locked(ptr);
    if (!block) {
        spin_unlock_irqrestore(&mm_lock, irq);
        return (uintptr_t)ptr >= KERNEL_HEAP_LIMIT ? vrealloc(ptr, size) : NULL;
    }

    if (block->size >= size) {
        split_block_locked(block, size);
        coalesce_free_list_locked();
        spin_unlock_irqrestore(&mm_lock, irq);
        return ptr;
    }

    // Grow in place when the immediately following block is free.
    struct memory_header *next = block->next;
    if (next && next->is_free && (uint8_t *)(block + 1) + block->size == (uint8_t *)next && block->size + sizeof(*next) + next->size >= size) {
        block->size += sizeof(*next) + next->size;
        block->next = next->next;
        split_block_locked(block, size);
        spin_unlock_irqrestore(&mm_lock, irq);
        return ptr;
    }

    size_t old_size = block->size;
    spin_unlock_irqrestore(&mm_lock, irq);

    void *replacement = malloc(size);
    if (!replacement) return NULL;
    memcpy(replacement, ptr, old_size);
    free(ptr);
    return replacement;
}

void init_mm(void) {
    if (!hhdm_req.response) panic("didn't get hhdm response");
    if (!kernel_context.pml4) panic("vmm must be initialized before mm");

    hhdm_offset = hhdm_req.response->offset;
    heap_ready = true;
    if (!grow_kernel_heap(HEAP_INITIAL_GROW - sizeof(struct memory_header))) {
        heap_ready = false;
        panic("unable to allocate initial kernel heap pages");
    }

    log("mm: initialized mm\n");
}
