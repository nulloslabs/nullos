#pragma once

#include <freestanding/stdint.h>
#include <freestanding/stddef.h>
#include <freestanding/stdbool.h>

#define PAGE_SIZE 4096

// Page Table Entry Flags
#define VMM_PRESENT  (1ULL << 0)
#define VMM_WRITABLE (1ULL << 1)
#define VMM_USER     (1ULL << 2)
#define VMM_PWT      (1ULL << 3)
#define VMM_PCD      (1ULL << 4)

typedef struct {
    uint64_t* pml4; // Virtual address of the PML4 table
} vmm_context_t;

typedef struct {
    uint64_t page_count;
} vmalloc_header_t;

extern vmm_context_t kernel_context;

void init_vmm(void);
vmm_context_t* create_vmm_context(void);
bool map_vmm(vmm_context_t* ctx, uint64_t virt, uint64_t phys, uint64_t flags);
void set_vmm_user(vmm_context_t* ctx, uint64_t virt);
void unmap_vmm(vmm_context_t* ctx, uint64_t virt);
uint64_t get_vmm_phys(vmm_context_t* ctx, uint64_t virt);
void switch_vmm_context(vmm_context_t* ctx);
void* phys_to_virt(uint64_t phys);
uint64_t virt_to_phys(void* virt);
void* vmap_mmio(uint64_t phys, size_t num_pages);
void* vmalloc(size_t size);
void* vmalloc_user(size_t size);
void* vmalloc_ex(vmm_context_t* ctx, size_t size, uint64_t flags);
void* vmalloc_user_ex(vmm_context_t* ctx, size_t size);
void* vrealloc(void* ptr, size_t size);
void vfree(void* ptr);

vmm_context_t* clone_vmm_context(vmm_context_t* parent);

// Context-aware memory access
void read_vmm(vmm_context_t* ctx, void* dest, uint64_t virt_src, size_t size);
void write_vmm(vmm_context_t* ctx, uint64_t virt_dest, const void* src, size_t size);
void memset_vmm(vmm_context_t* ctx, uint64_t virt_dest, int val, size_t size);