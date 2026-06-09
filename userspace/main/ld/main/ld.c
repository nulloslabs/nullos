#include <stdint.h>
#include <stddef.h>
#include <sys/syscall.h>

#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4

#define MAP_PRIVATE 0x02
#define MAP_FIXED   0x10
#define MAP_ANONYMOUS 0x20

#define O_RDONLY 0
#define PAGE_SIZE 4096

#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_PHDR 6

#define DT_NULL 0
#define DT_NEEDED 1
#define DT_HASH 4
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_JMPREL 23
#define DT_PLTRELSZ 2

#define R_X86_64_64 1
#define R_X86_64_GLOB_DAT 6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE 8

#define ELF64_R_SYM(i) ((i) >> 32)
#define ELF64_R_TYPE(i) ((i) & 0xffffffffL)

#define AT_NULL   0
#define AT_PHDR   3
#define AT_PHENT  4
#define AT_PHNUM  5
#define AT_PAGESZ 6
#define AT_BASE   7
#define AT_ENTRY  9

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
} __attribute__((packed)) auxv_t;

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

typedef struct loaded_so {
    char name[128];
    uint64_t base;
    elf64_dyn_t *dynamic;
    elf64_sym_t *symtab;
    uint64_t symcount;
    const char *strtab;
    elf64_rela_t *rela;
    uint64_t relasz;
    elf64_rela_t *jmprel;
    uint64_t pltrelsz;
    struct loaded_so *next;
} loaded_so_t;

static loaded_so_t *loaded_libraries = NULL;
static loaded_so_t so_pool[64];
static int so_pool_idx = 0;

static const char *lib_search_paths[] = {
    "/usr/local/lib",
    "/usr/lib/",
    "/lib/",
    "",
    NULL
};

static uint64_t next_load_base = 0x4000000000ULL;

static int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

static void strcpy(char *dest, const char *src) {
    while (*src) *dest++ = *src++;
    *dest = '\0';
}

static void strcat(char *dest, const char *src) {
    while (*dest) dest++;
    while (*src) *dest++ = *src++;
    *dest = '\0';
}

static inline int64_t open(const char *path, uint32_t flags, uint32_t mode) {
    int64_t ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(SYS_open), "D"(path), "S"(flags), "d"(mode) : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t read(int fd, void *buf, size_t count) {
    int64_t ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(SYS_read), "D"(fd), "S"(buf), "d"(count) : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t write(int fd, const void *buf, size_t count) {
    int64_t ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(SYS_write), "D"(fd), "S"(buf), "d"(count) : "rcx", "r11", "memory");
    return ret;
}

static inline void *mmap(void *addr, size_t length, int prot, int flags, int fd, uint64_t offset) {
    void *ret;
    register uint64_t r9 __asm__("r9") = offset;
    register int r8 __asm__("r8") = fd;
    register int r10 __asm__("r10") = flags;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(SYS_mmap), "D"(addr), "S"(length), "d"(prot), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t close(int fd) {
    int64_t ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(SYS_close), "D"(fd) : "rcx", "r11", "memory");
    return ret;
}

static inline void exit(int status) {
    __asm__ volatile("syscall" : : "a"(SYS_exit), "D"(status) : "rcx", "r11", "memory");
}


static loaded_so_t *alloc_so(void) {
    if (so_pool_idx >= 64) return NULL;
    return &so_pool[so_pool_idx++];
}

static uint64_t resolve_symbol(const char *name) {
    for (loaded_so_t *so = loaded_libraries; so; so = so->next) {
        if (!so->symtab || !so->strtab) continue;
        uint64_t count = so->symcount;
        if (count == 0) count = 10000; // Fallback if no DT_HASH

        for (uint64_t i = 0; i < count; i++) {
            elf64_sym_t *sym = &so->symtab[i];
            if (so->symcount == 0) {
                if (sym->name == 0 && sym->value == 0 && sym->info == 0 && i > 0) {
                    if (so->symtab[i+1].name == 0 && so->symtab[i+1].value == 0) break;
                }
            }
            if (sym->name && sym->shndx != 0) {
                const char *sym_name = so->strtab + sym->name;
                if (strcmp(sym_name, name) == 0) {
                    return so->base + sym->value;
                }
            }
        }
    }
    return 0;
}

static void process_relocations(loaded_so_t *so) {
    void do_relocs(elf64_rela_t *table, uint64_t size) {
        if (!table || !size) return;
        uint64_t count = size / sizeof(elf64_rela_t);
        for (uint64_t i = 0; i < count; i++) {
            elf64_rela_t *r = &table[i];
            uint64_t *target = (uint64_t *)(so->base + r->offset);
            uint32_t type = ELF64_R_TYPE(r->info);
            uint32_t sidx = ELF64_R_SYM(r->info);

            if (type == R_X86_64_RELATIVE) {
                *target = so->base + r->addend;
            } else if (type == R_X86_64_JUMP_SLOT || type == R_X86_64_GLOB_DAT || type == R_X86_64_64) {
                elf64_sym_t *s = &so->symtab[sidx];
                const char *sname = so->strtab + s->name;
                uint64_t val = resolve_symbol(sname);
                if (val) {
                    if (type == R_X86_64_64) val += r->addend;
                    *target = val;
                }
            }
        }
    }
    do_relocs(so->rela, so->relasz);
    do_relocs(so->jmprel, so->pltrelsz);
}

static void parse_dynamic(loaded_so_t *so) {
    if (!so->dynamic) return;
    for (int i = 0; so->dynamic[i].tag != DT_NULL; i++) {
        elf64_dyn_t *d = &so->dynamic[i];
        switch (d->tag) {
            case DT_STRTAB: so->strtab = (const char *)(so->base + d->un.ptr); break;
            case DT_SYMTAB: so->symtab = (elf64_sym_t *)(so->base + d->un.ptr); break;
            case DT_HASH: {
                uint32_t *hash = (uint32_t *)(so->base + d->un.ptr);
                so->symcount = hash[1];
                break;
            }
            case DT_RELA: so->rela = (elf64_rela_t *)(so->base + d->un.ptr); break;
            case DT_RELASZ: so->relasz = d->un.val; break;
            case DT_JMPREL: so->jmprel = (elf64_rela_t *)(so->base + d->un.ptr); break;
            case DT_PLTRELSZ: so->pltrelsz = d->un.val; break;
        }
    }
}

static int load_shared_library(const char *soname) {
    for (loaded_so_t *so = loaded_libraries; so; so = so->next) {
        if (strcmp(so->name, soname) == 0) return 0;
    }

    int fd = -1;
    if (soname[0] != '/' && soname[0] != '.') {
        for (int i = 0; lib_search_paths[i] != NULL; i++) {
            char path[256];
            strcpy(path, lib_search_paths[i]);
            strcat(path, soname);
            fd = open(path, O_RDONLY, 0);
            if (fd >= 0) break;
        }
    } else {
        fd = open(soname, O_RDONLY, 0);
    }

    if (fd < 0) return -1;

    elf64_ehdr_t ehdr;
    if (read(fd, &ehdr, sizeof(ehdr)) != sizeof(ehdr) || ehdr.magic != 0x464C457F) {
        close(fd);
        return -1;
    }

    void *first_page = mmap(NULL, PAGE_SIZE, PROT_READ, MAP_PRIVATE, fd, 0);
    if ((int64_t)first_page < 0) { close(fd); return -1; }
    
    elf64_ehdr_t *map_ehdr = (elf64_ehdr_t *)first_page;
    elf64_phdr_t *map_phdrs = (elf64_phdr_t *)((uint8_t*)first_page + map_ehdr->phoff);

    uint64_t min_vaddr = (uint64_t)-1;
    uint64_t max_vaddr = 0;
    for (int i = 0; i < map_ehdr->phnum; i++) {
        if (map_phdrs[i].type == PT_LOAD) {
            uint64_t start = map_phdrs[i].vaddr & ~(PAGE_SIZE - 1);
            uint64_t end = (map_phdrs[i].vaddr + map_phdrs[i].memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
            if (start < min_vaddr) min_vaddr = start;
            if (end > max_vaddr) max_vaddr = end;
        }
    }

    uint64_t load_size = max_vaddr - min_vaddr;
    uint64_t base_addr = next_load_base;
    next_load_base += (load_size + 0xFFFFFF) & ~0xFFFFFFULL;

    loaded_so_t *so = alloc_so();
    if (!so) { close(fd); return -1; }
    strcpy(so->name, soname);
    so->base = base_addr;

    for (int i = 0; i < map_ehdr->phnum; i++) {
        if (map_phdrs[i].type == PT_LOAD) {
            uint64_t seg_start = base_addr + map_phdrs[i].vaddr;
            uint64_t page_start = seg_start & ~(PAGE_SIZE - 1);
            uint64_t page_end = (seg_start + map_phdrs[i].memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
            uint64_t map_len = page_end - page_start;
            uint64_t offset = map_phdrs[i].offset & ~(PAGE_SIZE - 1);
            
            int prot = 0;
            if (map_phdrs[i].flags & 1) prot |= PROT_EXEC;
            if (map_phdrs[i].flags & 2) prot |= PROT_WRITE;
            if (map_phdrs[i].flags & 4) prot |= PROT_READ;

            mmap((void*)page_start, map_len, prot, MAP_PRIVATE | MAP_FIXED, fd, offset);
        } else if (map_phdrs[i].type == PT_DYNAMIC) {
            so->dynamic = (elf64_dyn_t *)(base_addr + map_phdrs[i].vaddr);
        }
    }

    if (!loaded_libraries) {
        loaded_libraries = so;
    } else {
        loaded_so_t *last = loaded_libraries;
        while (last->next) last = last->next;
        last->next = so;
    }

    parse_dynamic(so);

    if (so->strtab && so->dynamic) {
        for (int i = 0; so->dynamic[i].tag != DT_NULL; i++) {
            if (so->dynamic[i].tag == DT_NEEDED) {
                const char *needed = so->strtab + so->dynamic[i].un.val;
                load_shared_library(needed);
            }
        }
    }

    close(fd);
    return 0;
}

__attribute__((visibility("hidden"))) uint64_t _ld_start(uint64_t *stack) {
    uint64_t argc = stack[0];
    char **argv = (char **)&stack[1];
    char **envp = &argv[argc + 1];
    char **p = envp;
    while (*p) p++;
    auxv_t *auxv = (auxv_t *)(p + 1);

    uint64_t entry = 0, base = 0, main_phnum = 0;
    elf64_phdr_t *main_phdr = NULL;

    for (auxv_t *a = auxv; a->type != AT_NULL; a++) {
        if (a->type == AT_ENTRY) entry = a->un.val;
        else if (a->type == AT_BASE) base = a->un.val;
        else if (a->type == AT_PHDR) main_phdr = (elf64_phdr_t *)a->un.val;
        else if (a->type == AT_PHNUM) main_phnum = a->un.val;
    }

    if (base) {
        elf64_ehdr_t *ld_ehdr = (elf64_ehdr_t *)base;
        elf64_phdr_t *ld_phdr = (elf64_phdr_t *)(base + ld_ehdr->phoff);
        elf64_dyn_t *ld_dyn = NULL;
        for (int i = 0; i < ld_ehdr->phnum; i++) {
            if (ld_phdr[i].type == PT_DYNAMIC) {
                ld_dyn = (elf64_dyn_t *)(base + ld_phdr[i].vaddr);
                break;
            }
        }
        if (ld_dyn) {
            elf64_rela_t *rela = NULL;
            uint64_t relasz = 0;
            for (int i = 0; ld_dyn[i].tag != DT_NULL; i++) {
                if (ld_dyn[i].tag == DT_RELA) rela = (elf64_rela_t *)(base + ld_dyn[i].un.ptr);
                else if (ld_dyn[i].tag == DT_RELASZ) relasz = ld_dyn[i].un.val;
            }
            if (rela && relasz) {
                for (uint64_t i = 0; i < relasz / sizeof(elf64_rela_t); i++) {
                    if (ELF64_R_TYPE(rela[i].info) == R_X86_64_RELATIVE) {
                        uint64_t *target = (uint64_t *)(base + rela[i].offset);
                        *target = base + rela[i].addend;
                    }
                }
            }
        }
    }

    loaded_so_t *main_so = alloc_so();
    if (main_so) {
        strcpy(main_so->name, "main");
        uint64_t main_base = 0;
        if (main_phdr && main_phnum > 0) {
            main_base = (uint64_t)main_phdr - 64; 
            for (int i = 0; i < main_phnum; i++) {
                if (main_phdr[i].type == PT_PHDR) {
                    main_base = (uint64_t)main_phdr - main_phdr[i].vaddr;
                    break;
                }
            }
        }
        main_so->base = main_base;
        
        for (int i = 0; i < main_phnum; i++) {
            if (main_phdr[i].type == PT_DYNAMIC) {
                main_so->dynamic = (elf64_dyn_t *)(main_base + main_phdr[i].vaddr);
                break;
            }
        }
        loaded_libraries = main_so;
        parse_dynamic(main_so);
        if (main_so->strtab && main_so->dynamic) {
            for (int i = 0; main_so->dynamic[i].tag != DT_NULL; i++) {
                if (main_so->dynamic[i].tag == DT_NEEDED) {
                    const char *needed = main_so->strtab + main_so->dynamic[i].un.val;
                    load_shared_library(needed);
                }
            }
        }
    }

    for (loaded_so_t *so = loaded_libraries; so; so = so->next) process_relocations(so);

    return entry;
}

__asm__ (
    ".section .text\n"
    ".global _start\n"
    ".hidden _start\n"
    "_start:\n"
    "mov %rsp, %rdi\n"
    "call _ld_start\n"
    "jmp *%rax\n"
);
