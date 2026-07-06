#pragma once

#include <freestanding/stdint.h>
#include <freestanding/stddef.h>
#include <mm/vmm.h>

#define VMA_MAX      64
#define VMA_NAME_MAX 64

#define VMA_PROT_READ  1
#define VMA_PROT_WRITE 2
#define VMA_PROT_EXEC  4

#define VMA_FLAG_ANON   0x0001
#define VMA_FLAG_STACK  0x0002
#define VMA_FLAG_HEAP   0x0004
#define VMA_FLAG_SHARED 0x0008

typedef struct {
    bool     used;
    uint64_t start;
    uint64_t end;
    int      prot;
    int      flags;
    uint64_t offset;
    char     name[VMA_NAME_MAX];
} vma_t;

typedef struct {
    vma_t entries[VMA_MAX];
} vma_table_t;

void add_vma(vma_table_t *tbl, uint64_t start, uint64_t end, int prot, int flags, uint64_t offset, const char *name);
void remove_vma(vma_table_t *tbl, uint64_t start, uint64_t end);
void protect_vma(vma_table_t *tbl, uint64_t start, uint64_t end, int prot);
void set_vma_heap(vma_table_t *tbl, uint64_t brk_start, uint64_t brk);
bool get_vma(const vma_table_t *tbl, int n, vma_t *out);
int count_vmas(const vma_table_t *tbl);
void init_vma_table(vma_table_t *tbl);
