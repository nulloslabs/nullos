#include <stdint.h>
#include <stddef.h>
#include <sys/syscall.h>

#define AT_NULL   0
#define AT_PHDR   3
#define AT_PHENT  4
#define AT_PHNUM  5
#define AT_BASE   7
#define AT_ENTRY  9

typedef struct {
    uint64_t type;
    union {
        uint64_t val;
    } un;
} __attribute__((packed)) auxv_t;

uint64_t _ld_start(uint64_t *stack) {
    uint64_t argc = stack[0];
    char **argv = (char **)&stack[1];
    char **envp = &argv[argc + 1];

    char **p = envp;
    while (*p) p++;
    auxv_t *auxv = (auxv_t *)(p + 1);

    uint64_t entry = 0;
    for (; auxv->type != AT_NULL; auxv++) {
        if (auxv->type == AT_ENTRY) {
            entry = auxv->un.val;
        }
    }

    if (entry) {
        return entry;
    }

    asm volatile(
        "syscall"
        :
        : "a"(60), "D"(0)
    );
    return 0;
}

__asm__(
    ".section .text\n"
    ".global _start\n"
    "_start:\n"
    "mov %rsp, %rdi\n"
    "call _ld_start\n"
    "jmp *%rax\n"
);
