#pragma once

#include <freestanding/stddef.h>

struct memory_header {
    size_t size;
    int is_free;
    struct memory_header *next;
};

// Start of our memory chain
extern struct memory_header *free_list_start;
extern uint64_t hhdm_offset;

void init_mm(void* start_addr, size_t total_size);
void init_heap(void);
void* malloc(size_t size);
void* realloc(void* ptr, size_t size);
void free(void* ptr);