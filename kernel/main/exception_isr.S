.intel_syntax noprefix

.section .text
// We dont put our ISRS here, our macros already handle that
.global isrrsv
.global isrhdr

.extern exception_panic

/*
Table of all CPU faults:
  0: Division Error | Does not push an error code
  1: Debug | Does not push an error code
  2: Non-Maskable Interrupt | Does not push an error code
  3: Breakpoint | Does not push an error code
  4: Overflow | Does not push an error code
  5: Bound Range Exceeded | Does not push an error code
  6: Invalid Opcode | Does not push an error code
  7: Device Not Available | Does not push an error code
  8: Double Fault | Always pushes 0 as error code
  9: Coprocessor Segment Overrun | Does not push an error code
  10: Invalid TSS | Pushes an error code
  11: Segment Not Present | Pushes an error code
  12: Stack Segment Fault | Pushes an error code
  13: General Protection Fault | Pushes an error code
  14: Page Fault | Pushes an error code
  15: Reserved | Does not push an error code
  16: x87 Floating Point Exception | Does not push an error code
  17: Alignment Check | Pushes an error code
  18: Machine Check | Does not push an error code
  19: SIMD Floating Point Exception | Does not push an error code
  20: Virtualization Exception | Does not push an error code
  21: Control Protection Exception  | Pushes an error code
  22-27: Reserved | Does not push an error code
  28: Hypervisor Injection Exception | Does not push an error code
  29: VMM Communication Exception | Pushes an error code
  30: Security Exception | Pushes an error code
  31: Reserved | Does not push an error code
*/

.macro ISR_NOERR vector
    .global isr\vector
    isr\vector:
        push 0              // dummy error code
        push \vector        // vector number
        jmp isrhdr
.endm

.macro ISR_ERR vector
    .global isr\vector
    isr\vector:
        push \vector        // vector number (error code already on stack)
        jmp isrhdr
.endm

ISR_NOERR 0
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_ERR   13
ISR_ERR   14
ISR_ERR   30

// Reserved ISRs (22-27, 31): dummy error code + sentinel vector 512
isrrsv:
    push 0      // dummy error code
    push 512    // sentinel: reserved exception
    jmp isrhdr

isrhdr:
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rbp
    push rdi
    push rsi
    push rdx
    push rcx
    push rbx
    push rax            // real RAX saved; safe to clobber it below

    mov rax, cr3
    push rax
    mov rax, cr2
    push rax
    mov rax, cr0
    push rax

    xor eax, eax        // zero-extend cleanly, unlike "mov ax, ds" alone
    mov ax, ds
    push rax
    xor eax, eax
    mov ax, es
    push rax

    // CS now sits at offset 184 (offsetof(exception_frame_t, cs))
    mov rax, [rsp + 184]
    test al, 3
    jz .no_swap_entry
    swapgs
.no_swap_entry:

    mov ax, 0x10
    mov ds, ax
    mov es, ax

    // exception_panic(exception_frame_t *frame)
    mov rdi, rsp
    mov rbx, rsp
    and rsp, -16
    call exception_panic
    mov rsp, rbx

    mov rax, [rsp + 184]
    test al, 3
    jz .no_swap_exit
    swapgs
.no_swap_exit:

    pop rax
    mov es, ax
    pop rax
    mov ds, ax

    pop rax
    pop rax
    pop rax

    pop rax
    pop rbx
    pop rcx
    pop rdx
    pop rsi
    pop rdi
    pop rbp
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15

    add rsp, 16         // discard vector + error_code
    iretq
