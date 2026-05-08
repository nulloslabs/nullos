#include <freestanding/stddef.h>
#include <freestanding/stdint.h>
#include <main/string.h>
#include <mm/mm.h>
#include <main/panic.h>
#include <main/limine_req.h>
#include <io/terminal.h>

// A simple memory management system.

struct memory_header *free_list_start = NULL;
uint64_t hhdm_offset = 0;

void* malloc(size_t size) {
    if (size == 0) {
        size = 1;  // Allocate minimum 1 byte
    }

    // 1. Alignment (8 or 16 byte alignment is crucial for modern CPUs)
    size = (size + 7) & ~7; 

    struct memory_header *curr = free_list_start;
    while (curr) {
        if (curr->is_free && curr->size >= size) {
            // Can we split this block? 
            // We need enough space for the requested size + a new header + at least some data
            if (curr->size >= (size + sizeof(struct memory_header) + 16)) {
                struct memory_header *new_block = (struct memory_header*)((uint8_t*)curr + sizeof(struct memory_header) + size);
                new_block->size = curr->size - size - sizeof(struct memory_header);
                new_block->is_free = 1;
                new_block->next = curr->next;

                curr->size = size;
                curr->next = new_block;
            }

            curr->is_free = 0;
            return (void*)(curr + 1);
        }
        curr = curr->next;
    }
    return NULL;
}

void* realloc(void* ptr, size_t size) {
    if (!ptr) return malloc(size);

    struct memory_header *header = (struct memory_header*)ptr - 1;
    if (header->size >= size) return ptr; // Already big enough!

    void *new_ptr = malloc(size);
    if (new_ptr) {
        memcpy(new_ptr, ptr, header->size);
        free(ptr);
    }
    return new_ptr;
}

void free(void* ptr) {
    if (!ptr) return;

    struct memory_header *header = (struct memory_header*)ptr - 1;
    header->is_free = 1;

    struct memory_header *curr = free_list_start;
    while (curr && curr->next) {
        if (curr->is_free && curr->next->is_free) {
            curr->size += sizeof(struct memory_header) + curr->next->size;
            curr->next = curr->next->next;
            // Don't move to next yet, check if the NEW next is also free
            continue; 
        }
        curr = curr->next;
    }
}

void init_mm(void) {
    if (hhdm_req.response == NULL)
        panic("Didn't get HHDM response.");

    hhdm_offset = hhdm_req.response->offset;
    struct limine_memmap_response *memmap = mm_req.response;

    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE && entry->length >= (16 * 1024 * 1024)) {
            free_list_start = (struct memory_header*)(entry->base + hhdm_offset);
            free_list_start->size = (16 * 1024 * 1024) - sizeof(struct memory_header);
            free_list_start->is_free = 1;
            free_list_start->next = NULL;
            entry->base += 16 * 1024 * 1024;
            entry->length -= 16 * 1024 * 1024;
            printf("MM: Initialized MM.\n");
            return;
        }
    }

    panic("No usable memory found for MM.");
}
