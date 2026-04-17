#pragma once

#include <freestanding/stdint.h>
#include <freestanding/stddef.h>
#include <limine/limine.h>

#define PAGE_SIZE 4096

void init_pmm(void);
void* pmalloc(void);
void pfree(void *phys_addr);