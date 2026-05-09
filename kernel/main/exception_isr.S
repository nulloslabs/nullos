.intel_syntax noprefix

.section .text
// We dont put our ISRS here, our macros already handle that
.global isrrsv
.global isrhdr

.extern exception_panic

/*
Table of all CPU faults:
  0: Divide by 0 | Does not push an error code
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
    push rdi
    push rsi
    push rdx
    push rcx
    push r8
    push r9
    push r10
    push r11

    // Save segments
    mov ax, ds
    push rax
    mov ax, es
    push rax

    // Swap GS if we came from user space.
    // Frame layout from RSP:
    //   +0..+79  = saved regs (r11, r10, r9, r8, rcx, rdx, rsi, rdi, es, ds)
    //   +80      = vector
    //   +88      = error code
    //   +96      = RIP
    //   +104     = CS
    //   +112     = RFLAGS
    //   +120     = RSP (user)
    mov rax, [rsp + 80 + 24]    // CS
    test al, 3
    jz .no_swap_entry
    swapgs
.no_swap_entry:

    // Load kernel data segments
    mov ax, 0x10
    mov ds, ax
    mov es, ax

    // exception_panic(vector, error_code, rip, rsp, cs)
    mov rdi, [rsp + 80]         // vector number
    mov rsi, [rsp + 80 + 8]     // error code
    mov rdx, [rsp + 80 + 16]    // rip
    mov rcx, [rsp + 80 + 40]    // rsp
    mov r8,  [rsp + 80 + 24]    // cs

    call exception_panic

    // Swap GS back if returning to user space
    mov rax, [rsp + 80 + 24]    // CS
    test al, 3
    jz .no_swap_exit
    swapgs
.no_swap_exit:

    // Restore segments
    pop rax
    mov es, ax
    pop rax
    mov ds, ax

    pop r11
    pop r10
    pop r9
    pop r8
    pop rcx
    pop rdx
    pop rsi
    pop rdi

    // Clean up vector and error code
    add rsp, 16
    iretq
