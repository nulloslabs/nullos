.intel_syntax noprefix
.global gdt_flush
.global tss_flush

gdt_flush:
    lgdt [rdi]            // Load GDT pointer

    // Update data segment registers
    mov ax, 0x10          // 0x10 is the Kernel Data Segment
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    // Far return to reload CS to 0x08 (Kernel Code Segment)
    pop rdi               // Pop the return address into rdi
    push 0x08             // Push new CS selector
    push rdi              // Push return address back
    retfq                 // Far return

tss_flush:
    mov ax, 0x28          // 0x28 is the TSS descriptor in the GDT
    ltr ax                // Load Task Register
    ret
