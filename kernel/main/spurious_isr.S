.intel_syntax noprefix
.global isr_spurious
.extern current_apic_mode
.extern lapic_base
.extern eoi_pic

// Catch-all ISR for unhandled interrupt vectors.
// Just sends EOI and returns — prevents #GP from missing IDT entries.
isr_spurious:
    push rax
    push rbx
    push rcx
    push rdx

    // Send EOI to APIC or PIC
    mov eax, dword ptr [rip + current_apic_mode]
    test eax, eax
    jz .sp_eoi_pic

    cmp eax, 2
    je .sp_eoi_x2apic

    // xAPIC: MMIO write
    mov rbx, [rip + lapic_base]
    test rbx, rbx
    jz .sp_eoi_done
    mov dword ptr [rbx + 0xB0], 0
    jmp .sp_eoi_done

.sp_eoi_x2apic:
    xor edx, edx
    xor eax, eax
    mov ecx, 0x80B
    wrmsr
    jmp .sp_eoi_done

.sp_eoi_pic:
    call eoi_pic

.sp_eoi_done:
    pop rdx
    pop rcx
    pop rbx
    pop rax
    iretq
