#pragma once

#include <freestanding/stdint.h>
#include <freestanding/stddef.h>
#include <mm/vmm.h>
#include <main/scheduler.h>

#define ELF_MAGIC 0x464C457F
#define ELF_CLASS64 2
#define ELF_DATA2LSB 1
#define EM_X86_64 62

#define ET_EXEC 2
#define ET_DYN 3

// Program header types
#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_INTERP 3

// Dynamic tags
#define DT_NULL 0
#define DT_NEEDED 1
#define DT_HASH 4
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_RELAENT 9
#define DT_STRSZ 10
#define DT_SYMENT 11
#define DT_PLTRELSZ 2
#define DT_JMPREL 23

// Relocation types
#define R_X86_64_NONE 0
#define R_X86_64_64 1
#define R_X86_64_PC32 2
#define R_X86_64_GOT32 3
#define R_X86_64_PLT32 4
#define R_X86_64_COPY 5
#define R_X86_64_GLOB_DAT 6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE 8

#define ELF64_R_SYM(i) ((i) >> 32)
#define ELF64_R_TYPE(i) ((i) & 0xffffffffL)

typedef struct {
    uint32_t magic;
    uint8_t class;
    uint8_t data;
    uint8_t version;
    uint8_t osabi;
    uint8_t abiversion;
    uint8_t pad[7];
    uint16_t type;
    uint16_t machine;
    uint32_t e_version;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
} __attribute__((packed)) elf64_ehdr_t;

typedef struct {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
} __attribute__((packed)) elf64_phdr_t;

typedef struct {
    uint32_t name;
    uint32_t type;
    uint64_t flags;
    uint64_t addr;
    uint64_t offset;
    uint64_t size;
    uint32_t link;
    uint32_t info;
    uint64_t addralign;
    uint64_t entsize;
} __attribute__((packed)) elf64_shdr_t;

typedef struct {
    int64_t tag;
    union {
        uint64_t val;
        uint64_t ptr;
    } un;
} __attribute__((packed)) elf64_dyn_t;

typedef struct {
    uint32_t name;
    uint8_t info;
    uint8_t other;
    uint16_t shndx;
    uint64_t value;
    uint64_t size;
} __attribute__((packed)) elf64_sym_t;

typedef struct {
    uint64_t offset;
    uint64_t info;
    int64_t addend;
} __attribute__((packed)) elf64_rela_t;

// Loaded Shared Object metadata
typedef struct loaded_so {
    char name[128];
    uint64_t base;
    elf64_dyn_t *dynamic;
    elf64_sym_t *symtab;
    char *strtab;
    struct loaded_so *next;
} loaded_so_t;

pid_t execute_elf(const char *path, char **argv, char **envp);
int execve_elf(const char *path, char **argv, char **envp, void* raw_frame);
