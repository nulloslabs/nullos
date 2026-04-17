.intel_syntax noprefix

.section .text
.global isr0
.global isr4
.global isr5
.global isr6
.global isr7
.global isr8
.global isr13
.global isr14
.global isr30
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

isr0:
    push 0       // dummy error code
    push 0       /* this might seem weird but trust me bro
                                                                  - coolguy09 2026 */
    jmp isrhdr

isr4:
    push 0       // dummy error code
    push 4       // vector number
    jmp isrhdr

isr5:
    push 0       // dummy error code
    push 5       // vector number
    jmp isrhdr

isr6:
    push 0       // dummy error code
    push 6       // vector number
    jmp isrhdr

isr7:
    push 0       // dummy error code
    push 7       // vector number
    jmp isrhdr

isr8:
    push 8       // vector number
    jmp isrhdr

isr13:
    push 13       // vector number
    jmp isrhdr

isr14:
    push 14       // vector number
    jmp isrhdr

isr30:
    push 30       // vector number
    jmp isrhdr

// Exception for reserved (22-27, 31) ISRs
isrrsv:
    push 0         // dummy error code
    push 512    // we know that nothing else is gonna use vector 22-27 and 31, so just pass 512 (reserved exception)
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

    // Swap GS if we came from user space
    // Frame: 8 (r11) + 8 (r10) + 8 (r9) + 8 (r8) + 8 (rcx) + 8 (rdx) + 8 (rsi) + 8 (rdi) 
    //        + 8 (es) + 8 (ds) = 80 bytes
    // Then 8 (vector) + 8 (error) + 8 (RIP) + 8 (CS)
    mov rax, [rsp + 80 + 24] // Offset to CS
    test al, 3
    jz .no_swap_entry
    swapgs
.no_swap_entry:

    // Load kernel data segments
    mov ax, 0x10
    mov ds, ax
    mov es, ax

    // Arguments for exception_panic(vector, error_code, rip, rsp, cs)
    // RSP + 80 + 0  = vector
    // RSP + 80 + 8  = error code
    // RSP + 80 + 16 = rip
    // RSP + 80 + 24 = cs
    // RSP + 80 + 40 = rsp
    mov rdi, [rsp + 80]      // vector number
    mov rsi, [rsp + 80 + 8]  // error code
    mov rdx, [rsp + 80 + 16] // rip
    mov rcx, [rsp + 80 + 40] // rsp
    mov r8,  [rsp + 80 + 24] // cs (5th argument)

    call exception_panic

    // Swap GS back if we are returning to user space
    mov rax, [rsp + 80 + 24] // Offset to CS
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
    
    // Cleanup vector and error code
    add rsp, 16
    iretq