#include <freestanding/stddef.h>
#include <freestanding/stdint.h>
#include <main/string.h>
#include <mm/mm.h>
#include <mm/vmm.h>
#include <main/panic.h>
#include <main/limine_req.h>
#include <io/terminal.h>
#include <main/spinlock.h>

struct memory_header *free_list_start = NULL;
uint64_t hhdm_offset = 0;
static spinlock_t mm_lock = SPINLOCK_INIT;

static int in_pool(void* ptr) {
    return ptr >= (void*)free_list_start &&
           ptr <  (void*)((uint8_t*)free_list_start + 16 * 1024 * 1024);
}

void* malloc(size_t size) {
    if (size == 0) size = 1;
    size = (size + 7) & ~7;

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
    return vmalloc(size);
}

void* realloc(void* ptr, size_t size) {
    if (!ptr) return malloc(size);

    if (!in_pool(ptr))
        return vrealloc(ptr, size);

    uint64_t flags;
    spin_lock_irqsave(&mm_lock, &flags);

    struct memory_header *header = (struct memory_header*)ptr - 1;
    if (header->size >= size) {
        spin_unlock_irqrestore(&mm_lock, flags);
        return ptr;
    }

    size_t old_size = header->size;
    spin_unlock_irqrestore(&mm_lock, flags);

    void *new_ptr = malloc(size);
    if (new_ptr) {
        memcpy(new_ptr, ptr, old_size);
        free(ptr);
    }
    return new_ptr;
}

void free(void* ptr) {
    if (!ptr) return;

    if (!in_pool(ptr)) {
        vfree(ptr);
        return;
    }

    uint64_t flags;
    spin_lock_irqsave(&mm_lock, &flags);

    struct memory_header *header = (struct memory_header*)ptr - 1;
    header->is_free = 1;

    struct memory_header *curr = free_list_start;
    while (curr && curr->next) {
        if (curr->is_free && curr->next->is_free) {
            curr->size += sizeof(struct memory_header) + curr->next->size;
            curr->next  = curr->next->next;
            continue;
        }
        curr = curr->next;
    }
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
            free_list_start->size    = (16 * 1024 * 1024) - sizeof(struct memory_header);
            free_list_start->is_free = 1;
            free_list_start->next    = NULL;
            entry->base   += 16 * 1024 * 1024;
            entry->length -= 16 * 1024 * 1024;
            printf("mm: initialized mm\n");
            return;
        }
    }

    panic("no usable memory found for mm");
}
