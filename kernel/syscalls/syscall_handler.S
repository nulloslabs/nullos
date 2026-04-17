.intel_syntax noprefix
.global syscall_entry
.extern sched_lock
.extern current_task_ptr

syscall_entry:
    // entering from Ring 3
    // `syscall` sets RIP=LSTAR, CS=STAR(47:32), SS=STAR(47:32)+8, RFLAGS=RFLAGS & ~SFMASK
    // RSP STILL POINTS TO USER STACK

    // switch to kernel stack
    mov r12, rsp
    mov rsp, [rip + current_task_ptr]
    mov rsp, [rsp + 16]
    add rsp, 4096

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
    
    mov dword ptr [rip + sched_lock], 1
    sti
    call syscall_dispatch
    cli
    mov dword ptr [rip + sched_lock], 0
    
    mov rsp, rbx           // restore RSP for pops

    // restore registers
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

    mov rsp, r12

    sysretq