#pragma once

#include <freestanding/stdint.h>
#include <freestanding/stddef.h>
#include <limine/limine.h>

#define PAGE_SIZE 4096

void* pmalloc(void);
void* prealloc(uint64_t count);
void pfree(void *phys_addr);
void pfree_range(void *phys_addr, uint64_t size);
void pref(void *phys_addr);
void init_pmm(void);