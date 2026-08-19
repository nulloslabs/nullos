#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <mm/pmm.h>
#include <mm/vma.h>

#define KERNEL_HEAP_BASE  0xffffb00000000000ULL
#define KERNEL_HEAP_LIMIT 0xffffc00000000000ULL
#define USER_MMAP_BASE  0x0000100000000000ULL
#define USER_MMAP32_BASE 0x0000000040000000ULL
#define USER_VIRTUAL_LIMIT 0x0000800000000000ULL
#define USER_STACK_BASE 0x0000700000000000ULL
#define MAX_USER_MMAP_PAGES (262144ULL) // 1 GiB of mmap-managed virtual memory

// Page Table Entry Flags
#define VMM_PRESENT  (1ULL << 0)
#define VMM_WRITABLE (1ULL << 1)
#define VMM_USER     (1ULL << 2)
#define VMM_PWT      (1ULL << 3)
#define VMM_PCD      (1ULL << 4)
#define VMM_SHARED   (1ULL << 9)
#define VMM_EXTERNAL (1ULL << 10)
#define VMM_DEMAND   (1ULL << 11) // demand-paged: reserved but not yet backed
#define VMM_NX       (1ULL << 63)

typedef struct {
    uint64_t* pml4; // Virtual address of the PML4 table
    uint32_t refcount;
    uint64_t mmap_pages;
    vma_table_t vmas;
} vmm_context_t;

typedef struct {
    uint64_t page_count;
    uint8_t padding[56];
} vmalloc_header_t;

extern vmm_context_t kernel_context;

void* phys_to_virt(uint64_t phys);
uint64_t virt_to_phys(void* virt);
void set_vmm_user(vmm_context_t* ctx, uint64_t virt);
bool map_vmm(vmm_context_t* ctx, uint64_t virt, uint64_t phys, uint64_t flags);
void unmap_vmm(vmm_context_t* ctx, uint64_t virt);
uint64_t get_vmm_phys(vmm_context_t* ctx, uint64_t virt);
bool vmm_user_range_valid(vmm_context_t *ctx, uint64_t addr, size_t size, bool write);

// Context-aware memory access
void read_vmm(vmm_context_t* ctx, void* dest, uint64_t virt_src, size_t size);
void write_vmm(vmm_context_t* ctx, uint64_t virt_dest, const void* src, size_t size);
void memset_vmm(vmm_context_t* ctx, uint64_t virt_dest, int val, size_t size);

void switch_vmm_context(vmm_context_t* ctx);
vmm_context_t* create_vmm_context(void);
bool retain_vmm_context(vmm_context_t* ctx);
void destroy_vmm_context(vmm_context_t* ctx);
vmm_context_t* clone_vmm_context(vmm_context_t* parent);
void* vmalloc_ex(vmm_context_t* ctx, size_t size, uint64_t flags);
void* vmalloc_user_ex(vmm_context_t* ctx, size_t size);

void* vmap_mmio(uint64_t phys, size_t num_pages);
void vunmap_mmio(void* addr, size_t num_pages);

void* vmap_user_at(vmm_context_t* ctx, uint64_t virt, size_t size, uint64_t flags);
void* vmap_user_range(vmm_context_t* ctx, size_t size, uint64_t flags);
void* vmap_user_range_32(vmm_context_t* ctx, size_t size, uint64_t flags);
bool  reserve_vmm(vmm_context_t* ctx, uint64_t virt, uint64_t flags);
uint64_t get_vmm_pte(vmm_context_t* ctx, uint64_t virt);

void* vmalloc(size_t size);
void* vmalloc_user(size_t size);
void* vrealloc(void* ptr, size_t size);
void vfree(void* ptr);
void init_vmm(void);
