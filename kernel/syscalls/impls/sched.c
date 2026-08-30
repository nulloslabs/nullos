#include <stdbool.h>
#include <signal.h>
#include <flock.h>
#include <time.h>
#include <wait.h>
#include <limits.h>
#include <errno.h>
#include <asm/unistd.h>
#include <asm/prctl.h>
#include <linux/sched.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/fb.h>
#include <sys/statx.h>
#include <sys/resource.h>
#include <sys/reboot.h>
#include <sys/epoll.h>
#include <sys/sysmacros.h>
#include <main/limine_req.h>
#include <main/elf.h>
#include <main/halt.h>
#include <main/domainname.h>
#include <main/utsname.h>
#include <main/msr.h>
#include <main/sched.h>
#include <main/signal.h>
#include <main/rng.h>
#include <io/fonts.h>
#include <io/devtmpfs.h>
#include <io/pts_devices.h>
#include <io/terminal.h>
#include <io/tty.h>
#include <io/time.h>
#include <io/sockets.h>
#include <io/unix_sockets.h>
#include <io/procfs.h>
#include <io/ext4.h>
#include <io/vfat.h>
#include <io/gpt.h>
#include <mm/mm.h>
#include <mm/pmm.h>
#include <syscalls/syscalls.h>
#include <syscalls/impls/helpers.h>
#include <syscalls/impls/sched.h>

void sys_rt_sigaction(syscall_frame_t *frame) {
    int signum = (int)frame->rdi;
    uint64_t act_ptr = frame->rsi;
    uint64_t oldact_ptr = frame->rdx;

    if (signum < 1 || signum > 31 || (act_ptr && (signum == SIGKILL || signum == SIGSTOP))) {
        frame->rax = (uint64_t)-EINVAL;
        return;
    }

    if (oldact_ptr) {
        if (copy_to_user((void *)oldact_ptr, &current_task_ptr->sigactions[signum * 4], 32) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    }
    if (act_ptr) {
        uint64_t new_sa[4];
        if (copy_from_user(new_sa, (const void *)act_ptr, 32) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
        if (new_sa[0] > (uint64_t)SIG_IGN && !user_address_range_ok(new_sa[0], 1)) { frame->rax = (uint64_t)-EINVAL; return; }
        if (new_sa[2] && !user_address_range_ok(new_sa[2], 1)) { frame->rax = (uint64_t)-EINVAL; return; }
        for (int k = 0; k < 4; k++) current_task_ptr->sigactions[signum * 4 + k] = new_sa[k];
    }
    frame->rax = 0;
}

void sys_rt_sigprocmask(syscall_frame_t *frame) {
    int how = (int)frame->rdi;
    const uint64_t *set = (const uint64_t *)frame->rsi;
    uint64_t *oldset = (uint64_t *)frame->rdx;
    size_t sigsetsize = (size_t)frame->r10;

    if (sigsetsize != 8) {
        frame->rax = (uint64_t)-EINVAL;
        return;
    }

    if (oldset) {
        if (copy_to_user(oldset, &current_task_ptr->blocked_signals, 8) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    }

    if (set) {
        uint64_t new_set;
        if (copy_from_user(&new_set, set, 8) < 0) { frame->rax = (uint64_t)-EFAULT; return; }

        new_set &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));

        if (how == SIG_BLOCK) {
            current_task_ptr->blocked_signals |= new_set;
        } else if (how == SIG_UNBLOCK) {
            current_task_ptr->blocked_signals &= ~new_set;
        } else if (how == SIG_SETMASK) {
            current_task_ptr->blocked_signals = new_set;
        } else {
            frame->rax = (uint64_t)-EINVAL;
            return;
        }
    }

    frame->rax = 0;
}

void sys_rt_sigreturn(syscall_frame_t *frame) {
    uint64_t user_rsp = frame->rsp;
    signal_stack_frame_t saved;
    if (user_rsp >= USER_ADDR_MAX - 16 || copy_from_user(&saved, (const void *)(user_rsp + 16), sizeof(saved)) < 0) {
        frame->rax = (uint64_t)-EFAULT;
        return;
    }
    syscall_frame_t saved_frame = saved.context;
    if (saved_frame.rip >= USER_ADDR_MAX || saved_frame.rsp >= USER_ADDR_MAX) {
        frame->rax = (uint64_t)-EFAULT;
        return;
    }
    saved_frame.rflags &= ~(SYSCALL_RFLAG_IOPL | SYSCALL_RFLAG_NT | SYSCALL_RFLAG_RF | SYSCALL_RFLAG_VM);
    saved_frame.rflags |= SYSCALL_RFLAG_FIXED;
    current_task_ptr->blocked_signals = saved.blocked_signals & ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
    *frame = saved_frame;
}

void sys_sched_yield(syscall_frame_t *frame) {
    spin_unlock(&sched_lock);
    yield_sched();
    spin_lock(&sched_lock);
    frame->rax = 0;
}

void sys_getpid(syscall_frame_t *frame) {
    frame->rax = (uint64_t)current_task_ptr->pid;
}

void sys_getpriority(syscall_frame_t *frame) {
    int which = (int)frame->rdi;
    id_t who = (id_t)frame->rsi;
    if (which != PRIO_PROCESS) { frame->rax = (uint64_t)-EINVAL; return; }
    task_t *task = find_priority_task(which, who);
    if (!task) { frame->rax = (uint64_t)-ESRCH; return; }
    frame->rax = (uint64_t)(20 - get_task_nice(task));
}

void sys_setpriority(syscall_frame_t *frame) {
    int which = (int)frame->rdi;
    id_t who = (id_t)frame->rsi;
    int nice = (int)frame->rdx;
    if (which != PRIO_PROCESS) { frame->rax = (uint64_t)-EINVAL; return; }
    task_t *task = find_priority_task(which, who);
    if (!task) { frame->rax = (uint64_t)-ESRCH; return; }
    if (current_task_ptr->euid != 0 && current_task_ptr->euid != task->euid && current_task_ptr->euid != task->uid) { frame->rax = (uint64_t)-EPERM; return; }
    if (current_task_ptr->euid != 0 && nice < get_task_nice(task)) { frame->rax = (uint64_t)-EACCES; return; }
    frame->rax = (uint64_t)set_task_nice(task, nice);
}

void sys_clone(syscall_frame_t *frame) {
    uint64_t clone_flags = frame->rdi;
    uint64_t new_stack    = frame->rsi;
    int *parent_tidptr    = (int *)frame->rdx;
    int *child_tidptr     = (int *)frame->r10;
    uint64_t tls          = frame->r8;

    if (!current_task_ptr || !current_task_ptr->ctx) { frame->rax = (uint64_t)-EFAULT; return; }

    // Validate optional tid pointers if provided.
    if (parent_tidptr && !user_write_range_ok(current_task_ptr->ctx, (uint64_t)parent_tidptr, sizeof(int))) { frame->rax = (uint64_t)-EFAULT; return; }
    if (child_tidptr  && !user_write_range_ok(current_task_ptr->ctx, (uint64_t)child_tidptr, sizeof(int))) { frame->rax = (uint64_t)-EFAULT; return; }
    // CLONE_SETTLS: tls must be a valid user address.
    if ((clone_flags & CLONE_SETTLS) && tls != 0 && !user_address_range_ok(tls, 1)) { frame->rax = (uint64_t)-EPERM; return; }

    // CLONE_SIGHAND without CLONE_VM is not allowed (Linux returns EINVAL).
    if ((clone_flags & CLONE_SIGHAND) && !(clone_flags & CLONE_VM)) { frame->rax = (uint64_t)-EINVAL; return; }
    // CLONE_THREAD requires CLONE_SIGHAND (hence CLONE_VM).
    if ((clone_flags & CLONE_THREAD)  && !(clone_flags & CLONE_SIGHAND)) { frame->rax = (uint64_t)-EINVAL; return; }

    // Resolve the address space for the child.
    vmm_context_t *child_ctx;
    if (clone_flags & CLONE_VM) {
        // Share the parent's address space (threads).
        child_ctx = current_task_ptr->ctx;
        if (!retain_vmm_context(child_ctx)) { frame->rax = (uint64_t)-ENOMEM; return; }
    } else {
        // Copy-on-write style copy of the parent's address space (fork/clone).
        child_ctx = clone_vmm_context(current_task_ptr->ctx);
        if (!child_ctx) { frame->rax = (uint64_t)-ENOMEM; return; }
    }

    pid_t child_pid = clone_task_flags(frame, child_ctx, clone_flags, new_stack, parent_tidptr, child_tidptr, tls);
    if (child_pid < 0) {
        destroy_vmm_context(child_ctx);
        frame->rax = (uint64_t)-EAGAIN;
        return;
    }

    frame->rax = (uint64_t)child_pid;

    if (clone_flags & CLONE_VFORK) {
        current_task_ptr->state       = TASK_STOPPED;
        current_task_ptr->waiting_for = child_pid;
        current_task_ptr->stopped_by_signal = 0;
        spin_unlock(&sched_lock);
        sti();
        yield_sched();
        spin_lock(&sched_lock);
        cli();
    }
}

void sys_fork(syscall_frame_t *frame) {
    if (!current_task_ptr || !current_task_ptr->ctx) { frame->rax = (uint64_t)-EFAULT; return; }

    vmm_context_t *child_ctx = clone_vmm_context(current_task_ptr->ctx);
    if (!child_ctx) { frame->rax = (uint64_t)-ENOMEM; return; }

    pid_t child_pid = clone_task(frame, child_ctx);
    if (child_pid < 0) { destroy_vmm_context(child_ctx); frame->rax = (uint64_t)-EAGAIN; return; }

    frame->rax = (uint64_t)child_pid;
}

void sys_vfork(syscall_frame_t *frame) {
    if (!current_task_ptr || !current_task_ptr->ctx) { frame->rax = (uint64_t)-EFAULT; return; }

    vmm_context_t *child_ctx = clone_vmm_context(current_task_ptr->ctx);
    if (!child_ctx) { frame->rax = (uint64_t)-ENOMEM; return; }

    pid_t child_pid = clone_task(frame, child_ctx);
    if (child_pid < 0) { destroy_vmm_context(child_ctx); frame->rax = (uint64_t)-EAGAIN; return; }

    current_task_ptr->state = TASK_STOPPED;
    current_task_ptr->waiting_for = child_pid;
    current_task_ptr->stopped_by_signal = 0;

    frame->rax = (uint64_t)child_pid;

    spin_unlock(&sched_lock);
    sti();
    yield_sched();
    spin_lock(&sched_lock);
    cli();
}

void sys_execve(syscall_frame_t *frame) {
    const char *user_path = (const char *)frame->rdi;
    char **user_argv = (char **)frame->rsi;
    char **user_envp = (char **)frame->rdx;
    if (!user_path) { frame->rax = (uint64_t)-EINVAL; return; }
    char path_buf[256];

    if (copy_string_from_user(path_buf, user_path, sizeof(path_buf)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }

    // Check for shebang
    initrd_file_t probe = read_initrd(path_buf);
    if (probe.data && probe.size >= 2 && ((char *)probe.data)[0] == '#' && ((char *)probe.data)[1] == '!') {
        // Parse interpreter from shebang line - bounded by probe.size (no NUL guarantee)
        char *p = (char *)probe.data + 2;
        char *end = (char *)probe.data + probe.size;
        while (p < end && (*p == ' ' || *p == '\t')) p++;

        char interp[256] = {0};
        int interp_len = 0;
        while (p < end && *p && *p != '\n' && *p != '\r' && *p != ' ' && *p != '\t' && interp_len < 255)
            interp[interp_len++] = *p++;
        interp[interp_len] = '\0';

        while (p < end && (*p == ' ' || *p == '\t')) p++;

        char interp_arg[256] = {0};
        int interp_arg_len = 0;
        while (p < end && *p && *p != '\n' && *p != '\r' && interp_arg_len < 255)
            interp_arg[interp_arg_len++] = *p++;
        interp_arg[interp_arg_len] = '\0';

        if (interp_len > 0) {
            char **orig_argv = NULL;
            int orig_argc = copy_from_user_strarray(&orig_argv, (const char **)user_argv, 63);
            if (orig_argc < 0) { frame->rax = (uint64_t)orig_argc; return; }

            bool has_interp_arg = interp_arg_len > 0;
            int script_index = has_interp_arg ? 2 : 1;
            int new_argc = orig_argc + 1 + has_interp_arg;
            char **new_argv = vmalloc((new_argc + 1) * sizeof(char *));
            if (!new_argv) {
                free_strarray(orig_argv, orig_argc);
                frame->rax = (uint64_t)-ENOMEM; return;
            }

            new_argv[0] = malloc(interp_len + 1);
            if (!new_argv[0]) {
                free_strarray(orig_argv, orig_argc);
                vfree(new_argv);
                frame->rax = (uint64_t)-ENOMEM; return;
            }
            memcpy(new_argv[0], interp, interp_len + 1);

            if (has_interp_arg) {
                new_argv[1] = malloc(interp_arg_len + 1);
                if (!new_argv[1]) {
                    free(new_argv[0]);
                    free_strarray(orig_argv, orig_argc);
                    vfree(new_argv);
                    frame->rax = (uint64_t)-ENOMEM; return;
                }
                memcpy(new_argv[1], interp_arg, interp_arg_len + 1);
            }

            new_argv[script_index] = malloc(strlen(path_buf) + 1);
            if (!new_argv[script_index]) {
                free(new_argv[0]);
                if (has_interp_arg) free(new_argv[1]);
                free_strarray(orig_argv, orig_argc);
                vfree(new_argv);
                frame->rax = (uint64_t)-ENOMEM; return;
            }
            memcpy(new_argv[script_index], path_buf, strlen(path_buf) + 1);

            for (int i = 1; i < orig_argc; i++) new_argv[script_index + i] = orig_argv[i];
            new_argv[new_argc] = NULL;
            // Copy envp
            char **envp_ptrs = NULL;
            int envc = copy_from_user_strarray(&envp_ptrs, (const char **)user_envp, 63);
            if (envc < 0) {
                free_strarray(orig_argv, orig_argc);
                vfree(new_argv);
                frame->rax = (uint64_t)envc; return;
            }
            int res = execve_elf(new_argv[0], new_argv, envp_ptrs, frame);
            if (res == 0) {
                current_task_ptr->fs_base = 0;
                current_task_ptr->gs_base = 0;
                write_msr(MSR_FS_BASE, 0);
                write_msr(MSR_KERNEL_GS_BASE, 0);
                for (int i = 1; i < 32; i++) {
                    uint64_t *sa = &current_task_ptr->sigactions[i * 4];
                    if (sa[0] != 0 && sa[0] != 1) {
                        sa[0] = 0; sa[1] = 0; sa[2] = 0; sa[3] = 0;
                    }
                }
                current_task_ptr->pending_signals = 0;
            }
            free(new_argv[0]);
            if (has_interp_arg) free(new_argv[1]);
            free(new_argv[script_index]);
            free_strarray(orig_argv, orig_argc);
            free_strarray(envp_ptrs, envc);
            vfree(new_argv);
            frame->rax = (uint64_t)res;
            return;
        }
    }

    // Normal (non-shebang) path
    char **argv_ptrs = NULL;
    char **envp_ptrs = NULL;
    int argc = copy_from_user_strarray(&argv_ptrs, (const char **)user_argv, 63);
    if (argc < 0) { frame->rax = (uint64_t)argc; return; }
    int envc = copy_from_user_strarray(&envp_ptrs, (const char **)user_envp, 63);
    if (envc < 0) {
        free_strarray(argv_ptrs, argc);
        frame->rax = (uint64_t)envc;
        return;
    }
    int res = execve_elf(path_buf, argv_ptrs, envp_ptrs, frame);
    if (res == 0) {
        current_task_ptr->fs_base = 0;
        current_task_ptr->gs_base = 0;
        write_msr(MSR_FS_BASE, 0);
        write_msr(MSR_KERNEL_GS_BASE, 0);
        for (int i = 1; i < 32; i++) {
            uint64_t *sa = &current_task_ptr->sigactions[i * 4];
            if (sa[0] != 0 && sa[0] != 1) {
                sa[0] = 0; sa[1] = 0; sa[2] = 0; sa[3] = 0;
            }
        }
        current_task_ptr->pending_signals = 0;
    }
    free_strarray(argv_ptrs, argc);
    free_strarray(envp_ptrs, envc);
    if (res != 0) frame->rax = (uint64_t)res;
}

void sys_exit(syscall_frame_t *frame) {
    int status = (int)frame->rdi;
    exit_task(status);
}

void sys_wait4(syscall_frame_t *frame) {
    pid_t pid = (pid_t)frame->rdi;
    int *wstatus = (int *)frame->rsi;
    int options = (int)frame->rdx;
    struct rusage *rusage = (struct rusage *)frame->r10;

    if (wstatus && !user_write_range_ok(current_task_ptr->ctx, (uint64_t)wstatus, sizeof(*wstatus))) {
        frame->rax = (uint64_t)-EFAULT;
        spin_unlock(&sched_lock);
        return;
    }
    if (rusage && !user_write_range_ok(current_task_ptr->ctx, (uint64_t)rusage, sizeof(*rusage))) {
        frame->rax = (uint64_t)-EFAULT;
        spin_unlock(&sched_lock);
        return;
    }

    while (1) {
        int found_child = 0;

        for (int i = 0; i < MAX_TASKS; i++) {
            // Only care about children of the current task
            if (!tasks[i]->state || tasks[i]->ppid != current_task_ptr->pid) continue;

            if (pid > 0 && tasks[i]->pid != pid) continue;
            if (pid == 0 && tasks[i]->pgid != current_task_ptr->pgid) continue;
            if (pid < -1 && tasks[i]->pgid != -pid) continue;

            found_child = 1;

            if (tasks[i]->state == TASK_ZOMBIE) {
                // Encode wstatus correctly (Linux wait status encoding)
                int status;
                if (tasks[i]->term_sig != 0) {
                    // Killed by signal: low 7 bits = signal number
                    status = tasks[i]->term_sig & 0x7f;
                } else {
                    // Normal exit: bits 8-15 = exit code, low byte = 0
                    status = (tasks[i]->exit_status & 0xff) << 8;
                }
                if (wstatus) {
                    if (write_vmm(current_task_ptr->ctx, (uint64_t)wstatus, &status, sizeof(int)) < 0) { release_task_slot(i); spin_unlock(&sched_lock); frame->rax = (uint64_t)-EFAULT; return; }
                }
                if (rusage) {
                    struct rusage ru = {0};
                    if (write_vmm(current_task_ptr->ctx, (uint64_t)rusage, &ru, sizeof(struct rusage)) < 0) { release_task_slot(i); spin_unlock(&sched_lock); frame->rax = (uint64_t)-EFAULT; return; }
                }
                pid_t ret = tasks[i]->pid;
                release_task_slot(i);
                frame->rax = (uint64_t)ret;
                
                spin_unlock(&sched_lock);
                return;
            }

            if ((options & WUNTRACED) && tasks[i]->state == TASK_STOPPED && tasks[i]->stopped_by_signal && !tasks[i]->stop_reported) {
                // Report stopped child (bits 8-15 = stop signal, low byte = 0x7f)
                int status = (SIGTSTP << 8) | 0x7f;
                if (wstatus) {
                    if (write_vmm(current_task_ptr->ctx, (uint64_t)wstatus, &status, sizeof(int)) < 0) { spin_unlock(&sched_lock); frame->rax = (uint64_t)-EFAULT; return; }
                }
                tasks[i]->stop_reported = 1;
                frame->rax = (uint64_t)tasks[i]->pid;
                
                spin_unlock(&sched_lock);
                return;
            }
        }

        // If we found no children at all, we can't wait
        if (!found_child) { 
            frame->rax = (uint64_t)-ECHILD; 
            spin_unlock(&sched_lock); // CRITICAL: Unlock before leaving!
            return; 
        }

        // WNOHANG: return 0 if no child has exited yet
        if (options & WNOHANG) { 
            frame->rax = 0; 
            spin_unlock(&sched_lock);
            return; 
        }

        // Return EINTR if any pending signal has a custom handler to run.
        if (signal_pending()) {
            uint64_t pending = current_task_ptr->pending_signals & ~current_task_ptr->blocked_signals;
            pending |= current_task_ptr->pending_signals & ((1ULL << SIGKILL) | (1ULL << SIGSTOP)); // unblockable
            
            int has_custom = 0;
            for (int s = 1; s <= 31; s++) {
                if (!(pending & (1ULL << s))) continue;
                uint64_t h = current_task_ptr->sigactions[s * 4];
                if (h != 0 && h != 1) { has_custom = 1; break; }
                if (s != SIGCHLD && s != SIGCONT && s != SIGTSTP && s != SIGSTOP && h == 0) { has_custom = 1; break; }
            }
            if (has_custom) {
                current_task_ptr->waiting_for = 0;
                frame->rax = (uint64_t)-EINTR;
                spin_unlock(&sched_lock);
                return;
            }
        }

        current_task_ptr->waiting_for = pid > 0 ? pid : -1;
        current_task_ptr->state = TASK_STOPPED;
        spin_unlock(&sched_lock);
        yield_sched();
        spin_lock(&sched_lock);
        current_task_ptr->waiting_for = 0;
    }
}

void sys_kill(syscall_frame_t *frame) {
    pid_t pid = (pid_t)frame->rdi;
    int sig = (int)frame->rsi;

    // Accept signal 0 (existence check) and standard POSIX signals 1-31
    if (sig < 0 || sig > 31) { frame->rax = (uint64_t)-EINVAL; return; }

    if (pid > 0) {
        // Send to specific process
        for (int i = 0; i < MAX_TASKS; i++) {
            if (tasks[i]->state != TASK_DEAD && tasks[i]->pid == pid) {
                if (current_task_ptr->euid != 0 && current_task_ptr->uid != tasks[i]->uid) {
                    frame->rax = (uint64_t)-EPERM; return;
                }
                if (pid == 1 && sig != 0 && current_task_ptr->euid != 0) { frame->rax = (uint64_t)-EPERM; return; }
                if (sig == 0) { frame->rax = 0; return; }
                send_task_signal(i, sig);
                frame->rax = 0;
                return;
            }
        }
        frame->rax = (uint64_t)-ESRCH;
        return;
    }

    if (pid == 0) {
        // Send to every process in caller's process group
        pid_t my_pgrp = current_task_ptr->pgid;
        int found = 0;
        for (int i = 0; i < MAX_TASKS; i++) {
            if (tasks[i]->state == TASK_DEAD) continue;
            if (tasks[i]->pgid != my_pgrp) continue;
            if (sig == 0) { found = 1; continue; }
            if (current_task_ptr->euid != 0 && current_task_ptr->uid != tasks[i]->uid) continue;
            send_task_signal(i, sig);
            found = 1;
        }
        frame->rax = found ? 0 : (uint64_t)-ESRCH;
        return;
    }

    if (pid == -1) {
        // Send to every process the caller may signal (all except PID 1)
        int found = 0;
        for (int i = 0; i < MAX_TASKS; i++) {
            if (tasks[i]->state == TASK_DEAD) continue;
            if (tasks[i]->pid == 1 || tasks[i]->pid == current_task_ptr->pid) continue;
            if (current_task_ptr->euid != 0 && current_task_ptr->uid != tasks[i]->uid) continue;
            if (sig != 0) send_task_signal(i, sig);
            found = 1;
        }
        frame->rax = found ? 0 : (uint64_t)-ESRCH;
        return;
    }

    // pid < -1: send to process group -pid
    pid_t target_pgrp = -pid;
    int found = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i]->state == TASK_DEAD) continue;
        if (tasks[i]->pgid != target_pgrp) continue;
        if (current_task_ptr->euid != 0 && current_task_ptr->uid != tasks[i]->uid) continue;
        if (sig != 0) send_task_signal(i, sig);
        found = 1;
    }
    frame->rax = found ? 0 : (uint64_t)-ESRCH;
}

void sys_getuid(syscall_frame_t *frame) {
    frame->rax = current_task_ptr->uid;
}

void sys_getgid(syscall_frame_t *frame) {
    frame->rax = current_task_ptr->gid;
}

void sys_setuid(syscall_frame_t *frame) {
    uid_t uid = (uid_t)frame->rdi;

    if (current_task_ptr && current_task_ptr->euid == 0) {
        current_task_ptr->uid = uid;
        current_task_ptr->euid = uid;
        current_task_ptr->suid = uid;
        current_task_ptr->fsuid = uid;
        frame->rax = 0;
        return;
    }

    if (uid == current_task_ptr->uid || uid == current_task_ptr->euid || uid == current_task_ptr->suid) {
        current_task_ptr->euid = uid;
        current_task_ptr->fsuid = uid;
        frame->rax = 0;
        return;
    }

    frame->rax = (uint64_t)-EPERM;
}

void sys_setgid(syscall_frame_t *frame) {
    gid_t gid = (gid_t)frame->rdi;

    if (current_task_ptr && current_task_ptr->euid == 0) {
        current_task_ptr->gid = gid;
        current_task_ptr->egid = gid;
        current_task_ptr->sgid = gid;
        current_task_ptr->fsgid = gid;
        frame->rax = 0;
        return;
    }

    if (gid == current_task_ptr->gid || gid == current_task_ptr->egid || gid == current_task_ptr->sgid) {
        current_task_ptr->egid = gid;
        current_task_ptr->fsgid = gid;
        frame->rax = 0;
        return;
    }

    frame->rax = (uint64_t)-EPERM;
}

void sys_geteuid(syscall_frame_t *frame) {
    frame->rax = current_task_ptr->euid;
}

void sys_getegid(syscall_frame_t *frame) {
    frame->rax = current_task_ptr->egid;
}

void sys_setpgid(syscall_frame_t *frame) {
    pid_t pid  = (pid_t)frame->rdi;
    pid_t pgid = (pid_t)frame->rsi;

    // pid == 0 means caller
    if (pid == 0) pid = current_task_ptr->pid;
    // pgid == 0 means use target's pid
    if (pgid == 0) pgid = pid;
    if (pgid < 0) { frame->rax = (uint64_t)-EINVAL; return; }

    // Find the target task
    task_t *target = NULL;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i]->state != TASK_DEAD && tasks[i]->pid == pid) {
            target = tasks[i];
            break;
        }
    }
    if (!target) { frame->rax = (uint64_t)-ESRCH; return; }

    // Target must be the caller or a child of the caller
    if (target->pid != current_task_ptr->pid && target->ppid != current_task_ptr->pid) {
        frame->rax = (uint64_t)-ESRCH; return;
    }

    // Can't change pgrp of a session leader
    if (target->pid == target->sid) {
        frame->rax = (uint64_t)-EPERM; return;
    }

    // Must stay within the same session
    if (target->sid != current_task_ptr->sid) {
        frame->rax = (uint64_t)-EPERM; return;
    }

    target->pgid = pgid;
    frame->rax = 0;
}

void sys_getppid(syscall_frame_t *frame) {
    frame->rax = current_task_ptr->ppid;
}

void sys_getpgrp(syscall_frame_t *frame) {
    frame->rax = current_task_ptr->pgid;
}

void sys_setsid(syscall_frame_t *frame) {
    if (current_task_ptr->pid == current_task_ptr->pgid) {
        frame->rax = (uint64_t)-EPERM;
        return;
    }
    // Create a new session: caller becomes session leader and pgrp leader
    current_task_ptr->sid  = current_task_ptr->pid;
    current_task_ptr->pgid = current_task_ptr->pid;
    // Disassociate from controlling terminal
    current_task_ptr->ctty_idx = -1;
    frame->rax = (uint64_t)current_task_ptr->pid;
}

void sys_seteuid(syscall_frame_t *frame) {
    uid_t euid = (uid_t)frame->rdi;

    if (current_task_ptr && current_task_ptr->euid == 0) {
        current_task_ptr->euid = euid;
        current_task_ptr->fsuid = euid;
        frame->rax = 0;
        return;
    }

    if (euid == current_task_ptr->uid || euid == current_task_ptr->euid) {
        current_task_ptr->euid = euid;
        current_task_ptr->fsuid = euid;
        frame->rax = 0;
        return;
    }

    frame->rax = (uint64_t)-EPERM;
}

void sys_setegid(syscall_frame_t *frame) {
    gid_t egid = (gid_t)frame->rdi;

    if (current_task_ptr && current_task_ptr->euid == 0) {
        current_task_ptr->egid = egid;
        current_task_ptr->fsgid = egid;
        frame->rax = 0;
        return;
    }

    if (egid == current_task_ptr->gid || egid == current_task_ptr->egid) {
        current_task_ptr->egid = egid;
        current_task_ptr->fsgid = egid;
        frame->rax = 0;
        return;
    }

    frame->rax = (uint64_t)-EPERM;
}

void sys_setresuid(syscall_frame_t *frame) {
    uid_t ruid = (uid_t)frame->rdi;
    uid_t euid = (uid_t)frame->rsi;
    uid_t suid = (uid_t)frame->rdx;
    uid_t no_change = (uid_t)-1;

    bool privileged = current_task_ptr && current_task_ptr->euid == 0;
    if (!privileged) {
        if (ruid != no_change && ruid != current_task_ptr->uid && ruid != current_task_ptr->euid && ruid != current_task_ptr->suid) {
            frame->rax = (uint64_t)-EPERM; return;
        }
        if (euid != no_change && euid != current_task_ptr->uid && euid != current_task_ptr->euid && euid != current_task_ptr->suid) {
            frame->rax = (uint64_t)-EPERM; return;
        }
        if (suid != no_change && suid != current_task_ptr->uid && suid != current_task_ptr->euid && suid != current_task_ptr->suid) {
            frame->rax = (uint64_t)-EPERM; return;
        }
    }

    if (ruid != no_change) current_task_ptr->uid = ruid;
    if (euid != no_change) {
        current_task_ptr->euid = euid;
        current_task_ptr->fsuid = euid;
    }
    if (suid != no_change) current_task_ptr->suid = suid;
    frame->rax = 0;
}

void sys_getresuid(syscall_frame_t *frame) {
    uid_t *ruid = (uid_t *)frame->rdi;
    uid_t *euid = (uid_t *)frame->rsi;
    uid_t *suid = (uid_t *)frame->rdx;

    if (!ruid || !euid || !suid) { frame->rax = (uint64_t)-EFAULT; return; }
    if (!user_write_range_ok(current_task_ptr->ctx, (uint64_t)ruid, sizeof(uid_t)) || !user_write_range_ok(current_task_ptr->ctx, (uint64_t)euid, sizeof(uid_t)) || !user_write_range_ok(current_task_ptr->ctx, (uint64_t)suid, sizeof(uid_t))) {
        frame->rax = (uint64_t)-EFAULT; return;
    }

    uid_t r = current_task_ptr->uid;
    uid_t e = current_task_ptr->euid;
    uid_t s = current_task_ptr->suid;
    if (write_vmm(current_task_ptr->ctx, (uint64_t)ruid, &r, sizeof(r)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    if (write_vmm(current_task_ptr->ctx, (uint64_t)euid, &e, sizeof(e)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    if (write_vmm(current_task_ptr->ctx, (uint64_t)suid, &s, sizeof(s)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    frame->rax = 0;
}

void sys_setresgid(syscall_frame_t *frame) {
    gid_t rgid = (gid_t)frame->rdi;
    gid_t egid = (gid_t)frame->rsi;
    gid_t sgid = (gid_t)frame->rdx;
    gid_t no_change = (gid_t)-1;

    bool privileged = current_task_ptr && current_task_ptr->euid == 0;
    if (!privileged) {
        if (rgid != no_change && rgid != current_task_ptr->gid && rgid != current_task_ptr->egid && rgid != current_task_ptr->sgid) {
            frame->rax = (uint64_t)-EPERM; return;
        }
        if (egid != no_change && egid != current_task_ptr->gid && egid != current_task_ptr->egid && egid != current_task_ptr->sgid) {
            frame->rax = (uint64_t)-EPERM; return;
        }
        if (sgid != no_change && sgid != current_task_ptr->gid && sgid != current_task_ptr->egid && sgid != current_task_ptr->sgid) {
            frame->rax = (uint64_t)-EPERM; return;
        }
    }

    if (rgid != no_change) current_task_ptr->gid = rgid;
    if (egid != no_change) {
        current_task_ptr->egid = egid;
        current_task_ptr->fsgid = egid;
    }
    if (sgid != no_change) current_task_ptr->sgid = sgid;
    frame->rax = 0;
}

void sys_getresgid(syscall_frame_t *frame) {
    gid_t *rgid = (gid_t *)frame->rdi;
    gid_t *egid = (gid_t *)frame->rsi;
    gid_t *sgid = (gid_t *)frame->rdx;

    if (!rgid || !egid || !sgid) { frame->rax = (uint64_t)-EFAULT; return; }
    if (!user_write_range_ok(current_task_ptr->ctx, (uint64_t)rgid, sizeof(gid_t)) || !user_write_range_ok(current_task_ptr->ctx, (uint64_t)egid, sizeof(gid_t)) || !user_write_range_ok(current_task_ptr->ctx, (uint64_t)sgid, sizeof(gid_t))) {
        frame->rax = (uint64_t)-EFAULT; return;
    }

    gid_t r = current_task_ptr->gid;
    gid_t e = current_task_ptr->egid;
    gid_t s = current_task_ptr->sgid;
    if (write_vmm(current_task_ptr->ctx, (uint64_t)rgid, &r, sizeof(r)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    if (write_vmm(current_task_ptr->ctx, (uint64_t)egid, &e, sizeof(e)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    if (write_vmm(current_task_ptr->ctx, (uint64_t)sgid, &s, sizeof(s)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    frame->rax = 0;
}

void sys_setfsuid(syscall_frame_t *frame) {
    uid_t fsuid = (uid_t)frame->rdi;
    uid_t previous = current_task_ptr->fsuid;

    if (current_task_ptr->euid == 0 || fsuid == current_task_ptr->uid || fsuid == current_task_ptr->euid || fsuid == current_task_ptr->suid || fsuid == current_task_ptr->fsuid)
        current_task_ptr->fsuid = fsuid;

    frame->rax = previous;
}

void sys_setfsgid(syscall_frame_t *frame) {
    gid_t fsgid = (gid_t)frame->rdi;
    gid_t previous = current_task_ptr->fsgid;

    if (current_task_ptr->euid == 0 || fsgid == current_task_ptr->gid || fsgid == current_task_ptr->egid || fsgid == current_task_ptr->sgid || fsgid == current_task_ptr->fsgid)
        current_task_ptr->fsgid = fsgid;

    frame->rax = previous;
}

void sys_getpgid(syscall_frame_t *frame) {
    pid_t pid = (pid_t)frame->rdi;
    if (pid == 0) {
        frame->rax = (uint64_t)current_task_ptr->pgid;
        return;
    }
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i]->state != TASK_DEAD && tasks[i]->pid == pid) {
            frame->rax = (uint64_t)tasks[i]->pgid;
            return;
        }
    }
    frame->rax = (uint64_t)-ESRCH;
}

void sys_getsid(syscall_frame_t *frame) {
    pid_t pid = (pid_t)frame->rdi;
    if (pid == 0) {
        frame->rax = (uint64_t)current_task_ptr->sid;
        return;
    }
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i]->state != TASK_DEAD && tasks[i]->pid == pid) {
            // Only allowed to query tasks in same session
            if (tasks[i]->sid != current_task_ptr->sid) {
                frame->rax = (uint64_t)-EPERM;
            } else {
                frame->rax = (uint64_t)tasks[i]->sid;
            }
            return;
        }
    }
    frame->rax = (uint64_t)-ESRCH;
}

void sys_rt_sigtimedwait(syscall_frame_t *frame) {
    uint64_t set_ptr   = frame->rdi;
    uint64_t info_ptr  = frame->rsi;
    struct timespec *timeout = (struct timespec *)frame->rdx;

    if (!set_ptr || !user_range_ok(current_task_ptr->ctx, (uint64_t)(void *)set_ptr, sizeof(uint64_t))) {
        frame->rax = (uint64_t)-EFAULT;
        return;
    }

    // sigset_t is 128 bytes, but only the low 64 bits matter for signals 1..63.
    uint64_t want = 0;
    if (read_vmm(current_task_ptr->ctx, &want, set_ptr, sizeof(uint64_t)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }

    // SIGKILL/SIGSTOP can't be caught this way.
    want &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
    if (want == 0) {
        frame->rax = (uint64_t)-EINVAL;
        return;
    }

    int64_t timeout_us = -1; // -1 = wait forever
    if (timeout) {
        if (!user_range_ok(current_task_ptr->ctx, (uint64_t)timeout, sizeof(struct timespec))) {
            frame->rax = (uint64_t)-EFAULT;
            return;
        }
        struct timespec ts;
        if (read_vmm(current_task_ptr->ctx, &ts, (uint64_t)timeout, sizeof(struct timespec)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
        if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L) {
            frame->rax = (uint64_t)-EINVAL;
            return;
        }
        if (ts.tv_sec > 9000000000LL) ts.tv_sec = 9000000000LL; // cap to avoid overflow
        timeout_us = (int64_t)ts.tv_sec * 1000000LL + (int64_t)(ts.tv_nsec / 1000);
    }

    uint64_t start_us = get_monotonic_time_us();

    for (;;) {
        // pending_signals is 1-indexed: bit i <=> signal i. Scan signals in @want.
        for (int sig = 1; sig < 64; sig++) {
            uint64_t bit = (1ULL << sig);
            if (!(want & (1ULL << (sig - 1)))) continue;
            if (!(current_task_ptr->pending_signals & bit)) continue;

            // Consume the first pending signal in @set, regardless of whether
            // a handler is installed. By the time we get here the signal is
            // blocked, so check_signals() has left it pending for us.
            current_task_ptr->pending_signals &= ~bit;

            if (info_ptr && user_write_range_ok(current_task_ptr->ctx, (uint64_t)(void *)info_ptr, sizeof(siginfo_t))) {
                siginfo_t si;
                memset(&si, 0, sizeof(si));
                si.si_signo  = sig;
                si.si_code   = SI_USER;
                si.si_pid    = current_task_ptr->pid;
                si.si_uid    = current_task_ptr->uid;
                (void)write_vmm(current_task_ptr->ctx, info_ptr, &si, sizeof(siginfo_t));
            }

            frame->rax = (uint64_t)sig;
            return;
        }

        // Timeout expired?
        if (timeout_us >= 0) {
            uint64_t elapsed = get_monotonic_time_us() - start_us;
            if ((int64_t)elapsed >= timeout_us) {
                frame->rax = (uint64_t)-EAGAIN;
                return;
            }
        }

        // Interrupted by an *unrelated* pending signal that has a handler?
        if (signal_pending()) {
            uint64_t pending = current_task_ptr->pending_signals & ~(current_task_ptr->blocked_signals << 1);
            pending |= current_task_ptr->pending_signals & ((1ULL << SIGKILL) | (1ULL << SIGSTOP));

            int interrupted = 0;
            for (int s = 1; s <= 31; s++) {
                if (!(pending & (1ULL << s))) continue;
                uint64_t h = current_task_ptr->sigactions[s * 4];
                if (h != 0 && h != 1) { interrupted = 1; break; }
                if (s != SIGCHLD && s != SIGCONT && s != SIGTSTP && s != SIGSTOP && h == 0) {
                    interrupted = 1; break;
                }
            }
            if (interrupted) {
                frame->rax = (uint64_t)-EINTR;
                return;
            }
        }

        // Yield to the scheduler until something changes.
        let_current_task_sleep(1000);
    }
}

void sys_prctl(syscall_frame_t *frame) {
    int option = (int)frame->rdi;
    unsigned long arg2 = (unsigned long)frame->rsi;
    unsigned long arg3 = (unsigned long)frame->rdx;
    unsigned long arg4 = (unsigned long)frame->r10;
    unsigned long arg5 = (unsigned long)frame->r8;

    (void)arg3; (void)arg4; (void)arg5;

    switch (option) {
        case PR_SET_NAME: {
            // arg2 = pointer to a 16-byte (incl. NUL) name buffer in userspace
            const char *user_name = (const char *)arg2;
            if (!user_name || !user_write_range_ok(current_task_ptr->ctx, (uint64_t)user_name, 16)) {
                frame->rax = (uint64_t)-EFAULT; return;
            }
            char buf[16];
            copy_from_user(buf, user_name, 16);
            buf[15] = '\0';
            strncpy(current_task_ptr->name, buf, sizeof(current_task_ptr->name) - 1);
            current_task_ptr->name[sizeof(current_task_ptr->name) - 1] = '\0';
            frame->rax = 0;
            return;
        }
        case PR_GET_NAME: {
            char *user_name = (char *)arg2;
            if (!user_name || !user_range_ok(current_task_ptr->ctx, (uint64_t)user_name, 16)) {
                frame->rax = (uint64_t)-EFAULT; return;
            }
            char buf[16];
            memset(buf, 0, sizeof(buf));
            strncpy(buf, current_task_ptr->name, sizeof(buf) - 1);
            buf[15] = '\0';
            if (copy_to_user(user_name, buf, 16) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            frame->rax = 0;
            return;
        }
        case PR_SET_PDEATHSIG:
        case PR_GET_PDEATHSIG:
        case PR_GET_DUMPABLE:
        case PR_SET_DUMPABLE:
        case PR_GET_KEEPCAPS:
        case PR_SET_KEEPCAPS:
        case PR_GET_TIMING:
        case PR_SET_TIMING:
        case PR_GET_ENDIAN:
        case PR_SET_ENDIAN:
        case PR_GET_TSC:
        case PR_SET_TSC:
        case PR_GET_SECUREBITS:
        case PR_SET_SECUREBITS:
        case PR_GET_TIMERSLACK:
        case PR_SET_TIMERSLACK:
        case PR_GET_UNALIGN:
        case PR_SET_UNALIGN:
        case PR_GET_FPEMU:
        case PR_SET_FPEMU:
        case PR_GET_FPEXC:
        case PR_SET_FPEXC:
        case PR_GET_SECCOMP:
        case PR_SET_SECCOMP:
        case PR_CAPBSET_READ:
        case PR_CAPBSET_DROP:
            // Accepted but no-op: these features aren't implemented, so
            // return success to avoid breaking userspace that probes them.
            frame->rax = 0;
            return;
        default:
            frame->rax = (uint64_t)-EINVAL;
            return;
    }
}

void sys_arch_prctl(syscall_frame_t *frame) {
    int code = (int)frame->rdi;
    unsigned long addr = (unsigned long)frame->rsi;

    switch (code) {
        case ARCH_SET_FS:
            // Validate user-space address for FS base (TLS pointer)
            if (addr != 0 && !user_range_ok(current_task_ptr->ctx, (uint64_t)addr, 1)) {
                frame->rax = (uint64_t)-EPERM; return;
            }
            current_task_ptr->fs_base = addr;
            write_msr(MSR_FS_BASE, addr);
            frame->rax = 0;
            return;
        case ARCH_GET_FS:
            if (!user_write_range_ok(current_task_ptr->ctx, addr, sizeof(uint64_t))) {
                frame->rax = (uint64_t)-EFAULT; return;
            }
            if (write_vmm(current_task_ptr->ctx, addr, &current_task_ptr->fs_base, sizeof(uint64_t)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            frame->rax = 0;
            return;
        case ARCH_SET_GS:
            if (addr != 0 && !user_range_ok(current_task_ptr->ctx, (uint64_t)addr, 1)) {
                frame->rax = (uint64_t)-EPERM; return;
            }
            current_task_ptr->gs_base = addr;
            write_msr(MSR_KERNEL_GS_BASE, addr);
            frame->rax = 0;
            return;
        case ARCH_GET_GS:
            if (!user_write_range_ok(current_task_ptr->ctx, addr, sizeof(uint64_t))) {
                frame->rax = (uint64_t)-EFAULT; return;
            }
            if (write_vmm(current_task_ptr->ctx, addr, &current_task_ptr->gs_base, sizeof(uint64_t)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            frame->rax = 0;
            return;
        default:
            frame->rax = (uint64_t)-EINVAL;
            return;
    }
}

void sys_gettid(syscall_frame_t *frame) {
    frame->rax = (uint64_t)current_task_ptr->pid;
}

void sys_tkill(syscall_frame_t *frame) {
    pid_t tid = (pid_t)frame->rdi;
    int sig = (int)frame->rsi;

    if (sig < 0 || sig > 31) { frame->rax = (uint64_t)-EINVAL; return; }

    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i]->state == TASK_DEAD) continue;
        if (tasks[i]->pid != tid) continue;
        if (current_task_ptr->euid != 0 && current_task_ptr->uid != tasks[i]->uid) {
            frame->rax = (uint64_t)-EPERM; return;
        }
        if (tid == 1 && sig != 0 && current_task_ptr->euid != 0) { frame->rax = (uint64_t)-EPERM; return; }
        if (sig == 0) { frame->rax = 0; return; }
        send_task_signal(i, sig);
        frame->rax = 0;
        return;
    }
    frame->rax = (uint64_t)-ESRCH;
}

void sys_sched_getaffinity(syscall_frame_t *frame) {
    pid_t pid    = (pid_t)frame->rdi;
    size_t size  = (size_t)frame->rsi;
    void *mask   = (void *)frame->rdx;

    (void)pid; // Currently unused

    int ncpus = cpu_count;
    if (ncpus <= 0) ncpus = 1;

    size_t needed = ((size_t)ncpus + 7) / 8;
    size_t needed_aligned = (needed + sizeof(unsigned long) - 1) & ~(sizeof(unsigned long) - 1);

    if (!mask) { frame->rax = (uint64_t)-EFAULT; return; }
    if (size < needed) { frame->rax = (uint64_t)-EINVAL; return; }
    if (!user_range_ok(current_task_ptr->ctx, (uint64_t)mask, size)) { frame->rax = (uint64_t)-EFAULT; return; }

    unsigned long buf[1024];
    size_t bufsize = (needed_aligned > sizeof(buf)) ? sizeof(buf) : needed_aligned;
    memset(buf, 0, bufsize);

    for (int c = 0; c < ncpus; c++) {
        buf[c / (sizeof(unsigned long) * 8)] |= 1UL << (c % (sizeof(unsigned long) * 8));
    }

    if (copy_to_user(mask, buf, bufsize) != 0) {
        frame->rax = (uint64_t)-EFAULT;
        return;
    }

    frame->rax = (uint64_t)needed;
}

void sys_set_tid_address(syscall_frame_t *frame) {
    int *tidptr = (int *)frame->rdi;
    current_task_ptr->clear_child_tid = tidptr;
    frame->rax = current_task_ptr->pid;
}

void sys_exit_group(syscall_frame_t *frame) {
    int status = (int)frame->rdi;
    vmm_context_t *group_ctx = current_task_ptr->ctx;

    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *task = tasks[i];
        if (task->state == TASK_DEAD || task->ctx != group_ctx || task == current_task_ptr) continue;
        task->exit_status = status;
        task->pending_signals |= 1ULL << SIGKILL;
        if (task->state == TASK_STOPPED) task->state = TASK_READY;
    }

    exit_task(status);
}

void sys_tgkill(syscall_frame_t *frame) {
    pid_t tgid = (pid_t)frame->rdi;
    pid_t tid = (pid_t)frame->rsi;
    int sig = (int)frame->rdx;

    if (sig < 0 || sig > 31) { frame->rax = (uint64_t)-EINVAL; return; }
    if (tid <= 0) { frame->rax = (uint64_t)-EINVAL; return; }

    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i]->state == TASK_DEAD) continue;
        if (tasks[i]->pid != tid) continue;
        if (tgid > 0 && tgid != tasks[i]->pid && tgid != current_task_ptr->pid) {
            frame->rax = (uint64_t)-ESRCH; return;
        }
        if (current_task_ptr->euid != 0 && current_task_ptr->uid != tasks[i]->uid) {
            frame->rax = (uint64_t)-EPERM; return;
        }
        if (tid == 1 && sig != 0 && current_task_ptr->euid != 0) { frame->rax = (uint64_t)-EPERM; return; }
        if (sig == 0) { frame->rax = 0; return; }
        send_task_signal(i, sig);
        frame->rax = 0;
        return;
    }
    frame->rax = (uint64_t)-ESRCH;
}

void sys_set_robust_list(syscall_frame_t *frame) {
    void *head = (void *)frame->rdi;
    size_t len = (size_t)frame->rsi;

    // Linux documents the only supported struct size as 24 bytes
    // (sizeof(struct robust_list_head) on x86-64).
    // Like Linux, don't validate head here - validation happens at use time (exit)
    if (len != 24) { frame->rax = (uint64_t)-EINVAL; return; }

    current_task_ptr->robust_list_head = head;
    frame->rax = 0;
}

void sys_get_robust_list(syscall_frame_t *frame) {
    pid_t pid = (pid_t)frame->rdi;
    void **head_ptr = (void **)frame->rsi;
    size_t *len_ptr = (size_t *)frame->r10;

    if (pid != 0 && current_task_ptr && pid != current_task_ptr->pid) { frame->rax = (uint64_t)-ESRCH; return; }
    if (!head_ptr || !len_ptr || !user_write_range_ok(current_task_ptr->ctx, (uint64_t)head_ptr, sizeof(void *)) || !user_write_range_ok(current_task_ptr->ctx, (uint64_t)len_ptr, sizeof(size_t))) {
        frame->rax = (uint64_t)-EFAULT; return;
    }

    void *head = current_task_ptr->robust_list_head;
    if (write_vmm(current_task_ptr->ctx, (uint64_t)head_ptr, &head, sizeof(void *)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    size_t len = head ? 24 : 0;
    if (write_vmm(current_task_ptr->ctx, (uint64_t)len_ptr, &len, sizeof(size_t)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    frame->rax = 0;
}

void sys_rseq(syscall_frame_t *frame) {
    struct rseq *rseq = (void *)frame->rdi;
    uint32_t rseq_len = (uint32_t)frame->rsi;
    int flags = (int)frame->rdx;
    uint32_t sig = (uint32_t)frame->r10;

    if (flags & ~RSEQ_FLAG_UNREGISTER) { frame->rax = (uint64_t)-EINVAL; return; }

    // NULL with unregister is a no-op success.
    if ((flags & RSEQ_FLAG_UNREGISTER) || !rseq) {
        current_task_ptr->rseq = NULL;
        current_task_ptr->rseq_len = 0;
        current_task_ptr->rseq_sig = 0;
        frame->rax = 0;
        return;
    }

    if (!user_write_range_ok(current_task_ptr->ctx, (uint64_t)rseq, rseq_len)) { frame->rax = (uint64_t)-EFAULT; return; }

    current_task_ptr->rseq = rseq;
    current_task_ptr->rseq_len = rseq_len;
    current_task_ptr->rseq_sig = sig;
    frame->rax = 0;
}
