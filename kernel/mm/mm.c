#include <freestanding/stddef.h>
#include <freestanding/stdint.h>
#include <main/string.h>
#include <mm/mm.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <main/panic.h>
#include <main/limine_req.h>
#include <io/terminal.h>
#include <main/spinlocks.h>

#define BOOTSTRAP_HEAP_SIZE (16 * 1024 * 1024ULL)
#define KERNEL_HEAP_BASE    0xffffb00000000000ULL
#define KERNEL_HEAP_LIMIT   0xffffc00000000000ULL
#define KERNEL_HEAP_GROW    (1024 * 1024ULL)
#define ALIGN_UP(x, a)      (((x) + ((a) - 1)) & ~((a) - 1))

struct memory_header *free_list_start = NULL;
uint64_t hhdm_offset = 0;
static spinlock_t mm_lock = SPINLOCK_INIT;
static uint64_t kernel_heap_cursor = KERNEL_HEAP_BASE;

static struct memory_header *find_header_locked(void *ptr) {
    if (!ptr) return NULL;
    struct memory_header *header = (struct memory_header *)ptr - 1;
    for (struct memory_header *curr = free_list_start; curr; curr = curr->next) {
        if (curr == header) return curr;
    }
    return NULL;
}

static void coalesce_free_list_locked(void) {
    struct memory_header *curr = free_list_start;
    while (curr && curr->next) {
        uint8_t *curr_end = (uint8_t *)curr + sizeof(struct memory_header) + curr->size;
        if (curr->is_free && curr->next->is_free && curr_end == (uint8_t *)curr->next) {
            curr->size += sizeof(struct memory_header) + curr->next->size;
            curr->next = curr->next->next;
            continue;
        }
        curr = curr->next;
    }
}

static void insert_free_region_locked(void *region, size_t size) {
    struct memory_header *block = (struct memory_header *)region;
    block->size = size - sizeof(struct memory_header);
    block->is_free = 1;
    block->next = NULL;

    if (!free_list_start || block < free_list_start) {
        block->next = free_list_start;
        free_list_start = block;
        coalesce_free_list_locked();
        return;
    }

    struct memory_header *curr = free_list_start;
    while (curr->next && curr->next < block) curr = curr->next;
    block->next = curr->next;
    curr->next = block;
    coalesce_free_list_locked();
}

static int grow_kernel_heap(size_t needed) {
    if (!kernel_context.pml4) return 0;

    size_t grow_size = ALIGN_UP(needed + sizeof(struct memory_header), PAGE_SIZE);
    if (grow_size < KERNEL_HEAP_GROW) grow_size = KERNEL_HEAP_GROW;

    uint64_t flags;
    spin_lock_irqsave(&mm_lock, &flags);
    uint64_t start = kernel_heap_cursor;
    if (start > KERNEL_HEAP_LIMIT || grow_size > KERNEL_HEAP_LIMIT - start) {
        spin_unlock_irqrestore(&mm_lock, flags);
        return 0;
    }
    kernel_heap_cursor += grow_size;
    spin_unlock_irqrestore(&mm_lock, flags);

    size_t mapped = 0;
    for (; mapped < grow_size; mapped += PAGE_SIZE) {
        void *phys = pmalloc();
        if (!phys || !map_vmm(&kernel_context, start + mapped, (uint64_t)phys, VMM_WRITABLE | VMM_NX)) {
            if (phys) pfree(phys);
            for (size_t rollback = 0; rollback < mapped; rollback += PAGE_SIZE) {
                unmap_vmm(&kernel_context, start + rollback);
            }
            return 0;
        }
        memset((void *)(start + mapped), 0, PAGE_SIZE);
    }

    spin_lock_irqsave(&mm_lock, &flags);
    insert_free_region_locked((void *)start, grow_size);
    spin_unlock_irqrestore(&mm_lock, flags);
    return 1;
}

void* malloc(size_t size) {
    if (size == 0) size = 1;
    size = (size + 7) & ~7;

    for (;;) {
        uint64_t flags;
        spin_lock_irqsave(&mm_lock, &flags);

        struct memory_header *curr = free_list_start;
        while (curr) {
            if (curr->is_free && curr->size >= size) {
                if (curr->size >= (size + sizeof(struct memory_header) + 16)) {
                    struct memory_header *new_block =
                        (struct memory_header*)((uint8_t*)curr + sizeof(struct memory_header) + size);
                    new_block->size    = curr->size - size - sizeof(struct memory_header);
                    new_block->is_free = 1;
                    new_block->next    = curr->next;

                    curr->size = size;
                    curr->next = new_block;
                }
                curr->is_free = 0;
                spin_unlock_irqrestore(&mm_lock, flags);
                return (void*)(curr + 1);
            }
            curr = curr->next;
        }

        spin_unlock_irqrestore(&mm_lock, flags);
        if (!grow_kernel_heap(size)) return NULL;
    }
}

void* realloc(void* ptr, size_t size) {
    if (!ptr) return malloc(size);

    uint64_t flags;
    spin_lock_irqsave(&mm_lock, &flags);
    struct memory_header *header = find_header_locked(ptr);
    if (!header) {
        spin_unlock_irqrestore(&mm_lock, flags);
        return vrealloc(ptr, size);
    }

    if (header->size >= size) { spin_unlock_irqrestore(&mm_lock, flags); return ptr; }

    size_t old_size = header->size;
    spin_unlock_irqrestore(&mm_lock, flags);

    void *new_ptr = malloc(size);
    if (new_ptr) { memcpy(new_ptr, ptr, old_size); free(ptr); }
    return new_ptr;
}

void free(void* ptr) {
    if (!ptr) return;

    uint64_t flags;
    spin_lock_irqsave(&mm_lock, &flags);

    struct memory_header *header = find_header_locked(ptr);
    if (!header) {
        spin_unlock_irqrestore(&mm_lock, flags);
        vfree(ptr);
        return;
    }

    header->is_free = 1;
    coalesce_free_list_locked();
    spin_unlock_irqrestore(&mm_lock, flags);
}

void init_mm(void) {
    if (hhdm_req.response == NULL) panic("didn't get hhdm response");

    hhdm_offset = hhdm_req.response->offset;
    struct limine_memmap_response *memmap = mm_req.response;

    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE &&
            entry->length >= (16 * 1024 * 1024)) {
            free_list_start = (struct memory_header*)(entry->base + hhdm_offset);
            free_list_start->size    = BOOTSTRAP_HEAP_SIZE - sizeof(struct memory_header);
            free_list_start->is_free = 1;
            free_list_start->next    = NULL;
            entry->base   += BOOTSTRAP_HEAP_SIZE;
            entry->length -= BOOTSTRAP_HEAP_SIZE;
            printf("mm: initialized mm\n");
            return;
        }
    }

    panic("no usable memory found for mm");
}
