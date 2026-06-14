.intel_syntax noprefix
.global syscall_entry
.extern sched_lock
.extern current_task_ptr

syscall_entry:
    // switch to kernel stack
    swapgs
    mov r12, rsp
    mov rsp, [rip + current_task_ptr]
    mov rsp, [rsp + 40]
    add rsp, 32768

    // build syscall_frame_t
    push r15
    push r14
    push r13
    push r12                       // user RSP
    push r11                       // RFLAGS
    push r10                       // Arg 4
    push r9                        // Arg 6
    push r8                        // Arg 5
    push rbp
    push rdi                       // Arg 1
    push rsi                       // Arg 2
    push rdx                       // Arg 3
    push rcx                       // user RIP
    push rbx
    push rax                       // syscall number

    mov rdi, rsp           // frame pointer
    
    // align RSP to 16 bytes
    mov rbx, rsp
    and rsp, -16
    
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
    
    mov rsp, rbx           // restore RSP for pops

    // restore registers
    pop rax
    pop rbx
    pop rcx
    // Mitigate AMD sysret vulnerability: check if return RIP in RCX is canonical user space.
    // User canonical addresses must have bits 47-63 as all 0s.
    // Use rdx (about to be popped next anyway) as temporary scratch space.
    mov rdx, rcx
    shr rdx, 47
    jz .Lrcx_ok
    xor rcx, rcx           // Force non-canonical/kernel addresses to 0 to safely fault in ring 3
.Lrcx_ok:
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

    mov rsp, r12
    swapgs                 // Switch kernel GS back to user GS
    sysretq
