#pragma once

#include <stdint.h>
#include <stddef.h>

#define HEAP_ALIGNMENT       16ULL
#define HEAP_INITIAL_GROW    (64 * 1024ULL)
#define HEAP_MIN_GROW        (64 * 1024ULL)
#define HEAP_MIN_SPLIT       16ULL
#define HEAP_BLOCK_MAGIC     0x4e554c4c48454150ULL
#define KERNEL_HEAP_BASE     0xffffb00000000000ULL
#define KERNEL_HEAP_LIMIT    0xffffc00000000000ULL

#define ALIGN_UP(value, alignment) (((value) + ((alignment) - 1)) & ~((alignment) - 1))

struct memory_header {
    size_t size;
    uint64_t magic;
    int is_free;
    struct memory_header *next;
};

// Start of the memory chain
extern struct memory_header *free_list_start;
extern uint64_t hhdm_offset;

void* malloc(size_t size);
void* realloc(void* ptr, size_t size);
void free(void* ptr);
void init_mm(void);
