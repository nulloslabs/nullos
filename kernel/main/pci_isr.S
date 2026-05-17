.intel_syntax noprefix
.global isr43
.extern pci_dispatch
.extern current_apic_mode
.extern lapic_base

.macro MSI_STUB vec
.global isr\vec
isr\vec:
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
    // 17 quadwords pushed = 136 bytes. CS of interrupt frame is at rsp+144.

    mov rax, [rsp + 144]
    test al, 3
    jz 1f
    swapgs
1:
    mov ax, 0x10
    mov ds, ax
    mov es, ax

    mov edi, \vec
    call pci_dispatch

    // LAPIC EOI (MSI is always LAPIC-delivered).
    mov eax, dword ptr [rip + current_apic_mode]
    cmp eax, 2
    je 2f
    mov rbx, [rip + lapic_base]
    test rbx, rbx
    jz 3f
    mov dword ptr [rbx + 0xB0], 0
    jmp 3f
2:
    xor edx, edx
    xor eax, eax
    mov ecx, 0x80B
    wrmsr
3:

    mov rax, [rsp + 144]
    test al, 3
    jz 4f
    swapgs
4:
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
.endm

isr43:
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
    jz 5f
    swapgs
5:
    mov ax, 0x10
    mov ds, ax
    mov es, ax

    mov edi, 43
    call pci_dispatch

    // EOI for legacy: PIC if no APIC, LAPIC MMIO for xAPIC, MSR for x2APIC.
    mov eax, dword ptr [rip + current_apic_mode]
    test eax, eax
    jz 6f
    cmp eax, 2
    je 7f
    // xAPIC
    mov rbx, [rip + lapic_base]
    test rbx, rbx
    jz 8f
    mov dword ptr [rbx + 0xB0], 0
    jmp 8f
7:
    // x2APIC
    xor edx, edx
    xor eax, eax
    mov ecx, 0x80B
    wrmsr
    jmp 8f
6:
    // Legacy PIC
    mov al, 0x20
    out 0x20, al
8:

    mov rax, [rsp + 144]
    test al, 3
    jz 9f
    swapgs
9:
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

// =====================================================================
// MSI vectors 48..95 — one stub per device.
// =====================================================================
MSI_STUB 48
MSI_STUB 49
MSI_STUB 50
MSI_STUB 51
MSI_STUB 52
MSI_STUB 53
MSI_STUB 54
MSI_STUB 55
MSI_STUB 56
MSI_STUB 57
MSI_STUB 58
MSI_STUB 59
MSI_STUB 60
MSI_STUB 61
MSI_STUB 62
MSI_STUB 63
MSI_STUB 64
MSI_STUB 65
MSI_STUB 66
MSI_STUB 67
MSI_STUB 68
MSI_STUB 69
MSI_STUB 70
MSI_STUB 71
MSI_STUB 72
MSI_STUB 73
MSI_STUB 74
MSI_STUB 75
MSI_STUB 76
MSI_STUB 77
MSI_STUB 78
MSI_STUB 79
MSI_STUB 80
MSI_STUB 81
MSI_STUB 82
MSI_STUB 83
MSI_STUB 84
MSI_STUB 85
MSI_STUB 86
MSI_STUB 87
MSI_STUB 88
MSI_STUB 89
MSI_STUB 90
MSI_STUB 91
MSI_STUB 92
MSI_STUB 93
MSI_STUB 94
MSI_STUB 95

