.intel_syntax noprefix
.extern handle_pci_interrupt
.extern current_apic_mode
.extern lapic_base
.global isr43

isr43:
    // save registers
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

    // save segments
    mov ax, ds
    push rax
    mov ax, es
    push rax

    // setup kernel environment
    mov rax, [rsp + 144]
    test al, 3
    jz .no_swap_entry
    swapgs
.no_swap_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax

    // call handler
    call handle_pci_interrupt

    // send EOI
    mov eax, dword ptr [rip + current_apic_mode]
    test eax, eax
    jz .eoi_pic

    cmp eax, 2
    je .try_x2aeoi_pic

    mov rbx, [rip + lapic_base]
    test rbx, rbx
    jz .eoi_done
    mov dword ptr [rbx + 0xB0], 0
    jmp .eoi_done

.try_x2aeoi_pic:
    xor edx, edx
    xor eax, eax
    mov ecx, 0x80B
    wrmsr
    jmp .eoi_done

.eoi_pic:
    mov al, 0x20
    out 0x20, al

.eoi_done:
    // cleanup
    mov rax, [rsp + 144]
    test al, 3
    jz .no_swap_exit
    swapgs
.no_swap_exit:

    // restore segments
    pop rax
    mov es, ax
    pop rax
    mov ds, ax

    // restore registers
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