.intel_syntax noprefix
.global syscall_entry
.extern sched_lock
.extern dispatch_syscall

#include <mm/smap.h>
#include <main/sched.h>

syscall_entry:
    SANITIZE_KERNEL_FLAGS
    // switch to kernel stack
    swapgs
    mov qword ptr gs:[TASK_SYSCALL_USER_RSP_OFFSET], rsp
    mov rsp, qword ptr gs:[TASK_KSTACK_OFFSET]
    add rsp, KSTACK_SIZE

    // build syscall_frame_t
    push r11                       // user RFLAGS
    push rcx                       // user RIP
    push r15
    push r14
    push r13
    push r12
    push r11                       // RFLAGS
    push r10                       // Arg 4
    push r9                        // Arg 6
    push r8                        // Arg 5
    push rbp
    push qword ptr gs:[TASK_SYSCALL_USER_RSP_OFFSET] // user RSP
    push rdi                       // Arg 1
    push rsi                       // Arg 2
    push rdx                       // Arg 3
    push rcx                       // user RIP
    push rbx
    push rax                       // syscall number

    mov rdi, rsp           // frame pointer
    
    // Acquire sched_lock with interrupts ENABLED during spinloop
    sti
.Lspin:
    mov al, 1
    xchg byte ptr [rip + sched_lock], al
    test al, al
    jz .Llocked
    pause
    jmp .Lspin
    
.Llocked:
    // Now we have sched_lock, KEEP interrupts enabled during syscall
    // This allows the timer to preempt and schedule newly created tasks
    call dispatch_syscall
    
    // Release sched_lock before returning to userspace
    cli
    mov byte ptr [rip + sched_lock], 0
    
    // Validate the user return address and sanitize user RFLAGS.
    mov rdx, [rsp + 128]
    movabs rax, 0x0000800000000000
    cmp rdx, rax
    jb .Lrip_ok
    xor edx, edx
.Lrip_ok:
    mov [rsp + 128], rdx
    mov rdx, [rsp + 136]
    movabs rax, 0xfffffffffffc8fff
    and rdx, rax
    or rdx, 2
    mov [rsp + 136], rdx

    // iretq lets rt_sigreturn restore RCX and R11.
    sub rsp, 40
    mov qword ptr [rsp + 32], 0x1b
    mov rax, [rsp + 88]            // saved user RSP (40 + offsetof(rsp))
    mov [rsp + 24], rax
    mov rax, [rsp + 176]           // saved RFLAGS (40 + offsetof(rflags))
    mov [rsp + 16], rax
    mov qword ptr [rsp + 8], 0x23
    mov rax, [rsp + 168]           // saved RIP (40 + offsetof(rip))
    mov [rsp], rax

    // Restore registers.
    mov r15, [rsp + 160]
    mov r14, [rsp + 152]
    mov r13, [rsp + 144]
    mov r12, [rsp + 136]
    mov r11, [rsp + 128]
    mov r10, [rsp + 120]
    mov r9,  [rsp + 112]
    mov r8,  [rsp + 104]
    mov rbp, [rsp + 96]
    mov rdi, [rsp + 80]
    mov rsi, [rsp + 72]
    mov rdx, [rsp + 64]
    mov rcx, [rsp + 56]
    mov rbx, [rsp + 48]
    mov rax, [rsp + 40]

    swapgs
    iretq

.section .note.GNU-stack,"",@progbits
