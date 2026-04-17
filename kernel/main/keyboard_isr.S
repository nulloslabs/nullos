.intel_syntax noprefix
.global isr33

.extern key_buffer
.extern key_head
.extern eoi_apic
.extern handle_ps2_scancode

isr33:
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
    jz .no_swap_entry_kbd
    swapgs
.no_swap_entry_kbd:

    mov ax, 0x10
    mov ds, ax
    mov es, ax

    in al, 0x60
    movzx rdi, al

    push rsp
    push [rsp]
    and rsp, -16
    call handle_ps2_scancode
    call eoi_pic
    call eoi_apic
    mov rsp, [rsp + 8]

    mov rax, [rsp + 144]
    test al, 3
    jz .no_swap_exit_kbd
    swapgs
.no_swap_exit_kbd:

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