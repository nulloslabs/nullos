#include <freestanding/stddef.h>
#include <freestanding/stdint.h>
#include <main/string.h>
#include <mm/mm.h>
#include <main/panic.h>
#include <main/limine_req.h>
#include <io/terminal.h>

// A simple memory management system with validation.

struct memory_header *free_list_start = NULL;
uint64_t hhdm_offset = 0;

// Magic number to validate heap allocations
#define HEAP_MAGIC 0xDEADBEEF

// Structure to track valid heap allocations
typedef struct {
    uint32_t magic;
    struct memory_header *header;
} heap_validator_t;

// Validate that a pointer is a valid heap allocation
static bool is_valid_heap_ptr(void* ptr) {
    if (!ptr) return false;
    
    struct memory_header *header = (struct memory_header*)ptr - 1;
    
    // Sanity checks on header
    if (header->is_free > 1) return false;  // is_free should be 0 or 1
    if (header->size == 0 || header->size > 0x10000000) return false;  // Max 256MB
    
    // Additional: check that the header looks reasonable
    // (simple heuristic: header should be within reasonable memory range)
    uint64_t addr = (uint64_t)header;
    if (addr < 0xffffc00000000000ULL) return false;  // Must be in kernel space
    
    return true;
}

void* malloc(size_t size) {
    if (size == 0) {
        size = 1;  // Allocate minimum 1 byte
    }
    
    // Prevent excessive allocations
    if (size > 0x10000000) return NULL;  // Max 256MB per allocation

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

    // SECURITY: Validate that ptr is actually a valid heap allocation
    if (!is_valid_heap_ptr(ptr)) {
        panic("realloc: invalid pointer");
        return NULL;
    }

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

    // SECURITY: Validate that ptr is a valid heap allocation before freeing
    if (!is_valid_heap_ptr(ptr)) {
        panic("free: invalid pointer - possible double free or corruption");
        return;
    }

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
        panic("didn't get hhdm response");

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
            printf("mm: initialized mm\n");
            return;
        }
    }

    panic("no usable memory found for mm");
}
