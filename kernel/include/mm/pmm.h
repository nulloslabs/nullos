#pragma once

#define PAGE_SIZE 4096
#define PMM_MAX_ORDER 52
#define PMM_DMA32_LIMIT_PAGES (1ULL << 20)
#define PMM_INVALID_PAGE UINT64_MAX
#define PMM_PAGE_INTERIOR (-3)
#define PMM_PAGE_RESERVED (-2)
#define PMM_PAGE_ALLOCATED (-1)

#ifndef __ASSEMBLY__
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <limine.h>

typedef enum {
    PMM_ZONE_DMA32,
    PMM_ZONE_NORMAL,
    PMM_ZONE_COUNT,
} pmm_zone_t;

typedef struct {
    uint64_t previous;
    uint64_t next;
} pmm_buddy_links_t;

void* pmalloc(void);
void* pmalloc_dma32(void);
void* prealloc(uint64_t count);
void* prealloc_dma32(uint64_t count);
void pfree(void *phys_addr);
void pfree_range(void *phys_addr, uint64_t size);
void pref(void *phys_addr);
uint64_t get_total_pmm_memory(void);
uint64_t get_free_pmm_memory(void);
uint64_t get_used_pmm_memory(void);
void init_pmm(void);
#endif
