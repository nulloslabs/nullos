#include <stdint.h>
#include <stddef.h>
#include <sys/syscall.h>

typedef struct {
    uint64_t type;
    union {
        uint64_t val;
    } un;
} __attribute__((packed)) auxv_t;

#define AT_NULL   0
#define AT_PHDR   3
#define AT_PHENT  4
#define AT_PHNUM  5
#define AT_ENTRY  9
#define AT_BASE   7

static void puts(const char *s) {
    size_t len = 0;
    const char *p = s;
    while (*p++) len++;
    asm volatile(
        "syscall"
        :
        : "a"(1), "D"(1), "S"(s), "d"(len)
        : "rcx", "r11", "memory"
    );
}

static void print_num(uint64_t n) {
    char buf[21];
    int i = 20;
    buf[20] = '\n';
    if (n == 0) buf[--i] = '0';
    else {
        while (n && i > 0) {
            buf[--i] = '0' + n % 10;
            n /= 10;
        }
    }
    print(&buf[i]);
}

static void print_ptr(uint64_t p) {
    char buf[18];
    const char *hex = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        buf[15-i] = hex[(p >> (i*4)) & 0xF];
    }
    buf[16] = '\n';
    buf[17] = 0;
    print(&buf[0]);
}

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
