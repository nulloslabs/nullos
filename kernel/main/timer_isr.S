.intel_syntax noprefix
.global isr32
.extern schedule
.extern get_current_task_ptr
.extern current_apic_mode
.extern lapic_base
.extern poll_usb_hcds
.extern check_signals_from_interrupt
.extern record_timer_interrupt
// The spinlock for the scheduler is just a uint32_t
.extern sched_lock

#include <main/smap.h>

isr32:
    SANITIZE_KERNEL_FLAGS
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

    mov rbx, rsp
    and rsp, -16
    call record_timer_interrupt
    mov rsp, rbx

    // scheduler
    // Only poll USB HCDs + schedule a task switch if sched_lock is not held.
    // Blocking read paths (read_pts, read_pty_master, read_dev_tty, ...)
    // do "int $32" to yield while holding sched_lock, so this check
    // prevents those software-interrupt yields from driving USB polling
    // (which would also incorrectly heat up the typematic repeat timer
    // and make USB keyboard auto-repeat run way too fast).
    mov al, 1
    xchg byte ptr [rip + sched_lock], al
    test al, al
    jnz .skip_switch

    mov rbx, rsp
    and rsp, -16
    call poll_usb_hcds
    mov rsp, rbx

    mov r12, rsp
    and rsp, -16
    call get_current_task_ptr
    mov rsp, r12
    mov [rax + 32], rsp
    mov r12, rsp
    and rsp, -16
    call schedule
    mov rsp, r12
    mov r12, rsp
    and rsp, -16
    call get_current_task_ptr
    mov rsp, r12
    mov rsp, [rax + 32]
    mov byte ptr [rip + sched_lock], 0

.skip_switch:
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
    // Deliver pending signals before returning to userspace.
    mov rdi, rsp
    mov rbx, rsp
    and rsp, -16
    call check_signals_from_interrupt
    mov rsp, rbx

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

.section .note.GNU-stack,"",@progbits
