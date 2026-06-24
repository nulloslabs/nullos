.intel_syntax noprefix
.global syscall_entry
.extern sched_lock
.extern current_task_ptr

syscall_entry:
    // switch to kernel stack
    swapgs
    mov [rip + saved_user_rsp], rsp
    mov rsp, [rip + current_task_ptr]
    mov rsp, [rsp + 48]
    add rsp, 32768

    // build syscall_frame_t
    push r15
    push r14
    push r13
    push r12
    push r11                       // RFLAGS
    push r10                       // Arg 4
    push r9                        // Arg 6
    push r8                        // Arg 5
    push rbp
    push qword ptr [rip + saved_user_rsp] // user RSP
    push rdi                       // Arg 1
    push rsi                       // Arg 2
    push rdx                       // Arg 3
    push rcx                       // user RIP
    push rbx
    push rax                       // syscall number

    mov rdi, rsp           // frame pointer
    
.Llock_loop:
    lock bts dword ptr [rip + sched_lock], 0
    jnc .Llock_done
    pause
    jmp .Llock_loop
.Llock_done:
    
    sti
    call syscall_dispatch
    cli
    
    lock btr dword ptr [rip + sched_lock], 0
    
    // restore registers
    pop rax
    pop rbx
    pop rcx
    mov rdx, rcx
    shr rdx, 48
    jz .Lrcx_ok
    xor rcx, rcx           // Force non-canonical/kernel addresses to 0 to safely fault in ring 3
.Lrcx_ok:
    pop rdx
    pop rsi
    pop rdi
    add rsp, 8            // skip user RSP
    pop rbp
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15

    sub rsp, 80           // go back to user RSP location
    mov rsp, [rsp]        // restore user RSP from syscall_frame_t.rsp
    swapgs                // Switch kernel GS back to user GS
    sysretq

.section .bss
.align 8
saved_user_rsp:
    .quad 0
