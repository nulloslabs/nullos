#pragma once

#include <stdint.h>
#include <stddef.h>
#include <mm/vmm.h>
#include <main/sched.h>

#define ELF_USER_ADDR_MAX 0x0000800000000000ULL
#define ELF_MAX_PHNUM 128

#define ELF_MAGIC 0x464C457F
#define ELF_CLASS64 2
#define ELF_DATA2LSB 1
#define EM_X86_64 62

// Executable types
#define ET_NONE   0
#define ET_REL    1
#define ET_EXEC   2
#define ET_DYN    3
#define ET_CORE   4
#define ET_LOOS   0xFE00
#define ET_HIOS   0xFEFF
#define ET_LOPROC 0xFF00
#define ET_HIPROC 0xFFFF

// Program header types
#define PT_NULL         0
#define PT_LOAD         1
#define PT_DYNAMIC      2
#define PT_INTERP       3
#define PT_NOTE         4
#define PT_SHLIB        5
#define PT_PHDR         6
#define PT_TLS          7
#define PT_LOOS         0x60000000
#define PT_GNU_EH_FRAME 0x6474E550
#define PT_GNU_STACK    0x6474E551
#define PT_GNU_RELRO    0x6474E552
#define PT_GNU_PROPERTY 0x6474E553
#define PT_HIOS         0x6FFFFFFF
#define PT_LOPROC       0x70000000
#define PT_HIPROC       0x7FFFFFFF

// Program header flags
#define PF_X 1
#define PF_W 2
#define PF_R 4

// Section header types
#define SHT_NULL          0
#define SHT_PROGBITS      1
#define SHT_SYMTAB        2
#define SHT_STRTAB        3
#define SHT_RELA          4
#define SHT_HASH          5
#define SHT_DYNAMIC       6
#define SHT_NOTE          7
#define SHT_NOBITS        8
#define SHT_REL           9
#define SHT_SHLIB         10
#define SHT_DYNSYM        11
#define SHT_INIT_ARRAY    14
#define SHT_FINI_ARRAY    15
#define SHT_PREINIT_ARRAY 16
#define SHT_GROUP         17
#define SHT_SYMTAB_SHNDX  18
#define SHT_LOOS          0x60000000
#define SHT_GNU_HASH      0x6FFFFFF6
#define SHT_GNU_VERDEF    0x6FFFFFFD
#define SHT_GNU_VERNEED   0x6FFFFFFE
#define SHT_GNU_VERSYM    0x6FFFFFFF
#define SHT_HIOS          0x6FFFFFFF
#define SHT_LOPROC        0x70000000
#define SHT_HIPROC        0x7FFFFFFF
#define SHT_LOUSER        0x80000000
#define SHT_HIUSER        0xFFFFFFFF

// Section header flags
#define SHF_WRITE            0x1
#define SHF_ALLOC            0x2
#define SHF_EXECINSTR        0x4
#define SHF_MERGE            0x10
#define SHF_STRINGS          0x20
#define SHF_INFO_LINK        0x40
#define SHF_LINK_ORDER       0x80
#define SHF_OS_NONCONFORMING 0x100
#define SHF_GROUP            0x200
#define SHF_TLS              0x400
#define SHF_COMPRESSED       0x800
#define SHF_MASKOS           0x0FF00000
#define SHF_MASKPROC         0xF0000000

// Reserved section indexes
#define SHN_UNDEF     0x0000
#define SHN_LORESERVE 0xFF00
#define SHN_LOPROC    0xFF00
#define SHN_HIPROC    0xFF1F
#define SHN_LOOS      0xFF20
#define SHN_HIOS      0xFF3F
#define SHN_ABS       0xFFF1
#define SHN_COMMON    0xFFF2
#define SHN_XINDEX    0xFFFF
#define SHN_HIRESERVE 0xFFFF

// Symbol bindings
#define STB_LOCAL  0
#define STB_GLOBAL 1
#define STB_WEAK   2
#define STB_LOOS   10
#define STB_GNU_UNIQUE 10
#define STB_HIOS   12
#define STB_LOPROC 13
#define STB_HIPROC 15

// Symbol types
#define STT_NOTYPE  0
#define STT_OBJECT  1
#define STT_FUNC    2
#define STT_SECTION 3
#define STT_FILE    4
#define STT_COMMON  5
#define STT_TLS     6
#define STT_LOOS    10
#define STT_GNU_IFUNC 10
#define STT_HIOS    12
#define STT_LOPROC  13
#define STT_HIPROC  15

// Symbol visibility
#define STV_DEFAULT   0
#define STV_INTERNAL  1
#define STV_HIDDEN    2
#define STV_PROTECTED 3

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

// auxv types
#define AT_NULL   0
#define AT_PHDR   3
#define AT_PHENT  4
#define AT_PHNUM  5
#define AT_PAGESZ 6
#define AT_BASE   7
#define AT_FLAGS  8
#define AT_ENTRY  9
#define AT_RANDOM 25

#define ELF64_ST_BIND(info) ((info) >> 4)
#define ELF64_ST_TYPE(info) ((info) & 0x0F)
#define ELF64_ST_INFO(bind, type) (((bind) << 4) | ((type) & 0x0F))
#define ELF64_ST_VISIBILITY(other) ((other) & 0x03)

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

typedef struct {
    uint64_t type;
    union {
        uint64_t val;
    } un;
} __attribute__((packed)) elf64_auxv_t;

int execute_elf(const char *path, char **argv, char **envp);
int execve_elf(const char *path, char **argv, char **envp, void* raw_frame);
