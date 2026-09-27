.intel_syntax noprefix
.global acpi_isr
.extern uacpi_irq_dispatch
.extern eoi_apic

#include <mm/smap.h>

acpi_isr:
    SANITIZE_KERNEL_FLAGS
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov ax, ds
    push rax
    mov ax, es
    push rax

    mov rax, [rsp + 144]
    test al, 3
    jz .no_swap_entry
    swapgs
.no_swap_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax

    mov rbx, rsp
    and rsp, -16
    call uacpi_irq_dispatch
    call eoi_apic
    mov rsp, rbx

    mov rax, [rsp + 144]
    test al, 3
    jz .no_swap_exit
    swapgs
.no_swap_exit:
    pop rax
    mov es, ax
    pop rax
    mov ds, ax
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    iretq

.section .note.GNU-stack,"",@progbits
