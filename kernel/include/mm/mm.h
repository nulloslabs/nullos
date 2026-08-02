#pragma once

#include <stddef.h>
#include <stdint.h>

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
