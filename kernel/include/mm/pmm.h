#pragma once

#include <stdint.h>
#include <stddef.h>
#include <limine.h>

#define PAGE_SIZE 4096

void* pmalloc(void);
void* pmalloc_dma32(void);
void* prealloc(uint64_t count);
void pfree(void *phys_addr);
void pfree_range(void *phys_addr, uint64_t size);
void pref(void *phys_addr);
uint64_t get_total_pmm_memory(void);
uint64_t get_free_pmm_memory(void);
uint64_t get_used_pmm_memory(void);
void init_pmm(void);
