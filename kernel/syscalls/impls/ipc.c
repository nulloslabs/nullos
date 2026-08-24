#include <stdbool.h>
#include <signal.h>
#include <flock.h>
#include <time.h>
#include <wait.h>
#include <limits.h>
#include <errno.h>
#include <asm/unistd.h>
#include <linux/rseq.h>
#include <linux/types.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/fb.h>
#include <sys/statx.h>
#include <sys/resource.h>
#include <sys/futex.h>
#include <sys/random.h>
#include <sys/uio.h>
#include <main/log.h>
#include <main/spinlocks.h>
#include <main/halt.h>
#include <main/domainname.h>
#include <main/utsname.h>
#include <main/msr.h>
#include <main/sched.h>
#include <main/string.h>
#include <io/fb.h>
#include <io/devices.h>
#include <io/devpts.h>
#include <io/initrd.h>
#include <io/keyboard.h>
#include <io/pty.h>
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
#include <syscalls/impls/ipc.h>

void sys_pipe(syscall_frame_t *frame) {
    int *pipefd = (int *)frame->rdi;
    unix_handle_t *read_end = NULL;
    unix_handle_t *write_end = NULL;
    int fds[2];
    int r;

    if (!pipefd) { frame->rax = (uint64_t)-EINVAL; return; }
    if (!user_write_range_ok(current_task_ptr->ctx, (uint64_t)pipefd, sizeof(int) * 2)) { frame->rax = (uint64_t)-EFAULT; return; }
    r = create_unix_pipe(&read_end, &write_end);
    if (r < 0) { frame->rax = (uint64_t)r; return; }

    fds[0] = alloc_fd_handle(&current_task_ptr->fd_table, "pipe:r", FD_PIPE, O_RDONLY, read_end);
    if (fds[0] < 0) {
        release_unix_handle(read_end);
        release_unix_handle(write_end);
        frame->rax = (uint64_t)fds[0];
        return;
    }
    fds[1] = alloc_fd_handle(&current_task_ptr->fd_table, "pipe:w", FD_PIPE, O_WRONLY, write_end);
    if (fds[1] < 0) {
        free_fd(&current_task_ptr->fd_table, fds[0]);
        release_unix_handle(write_end);
        frame->rax = (uint64_t)fds[1];
        return;
    }

    if (write_vmm(current_task_ptr->ctx, (uint64_t)pipefd, fds, sizeof(fds)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    frame->rax = 0;
}

void sys_socket(syscall_frame_t *frame) {
    int domain = (int)frame->rdi;
    int type = (int)frame->rsi;
    int protocol = (int)frame->rdx;
    int socket_flags = type & (SOCK_NONBLOCK | SOCK_CLOEXEC);
    int base_type = type & SOCK_TYPE_MASK;
    socket_t *sock = NULL;

    if (type & ~(SOCK_TYPE_MASK | SOCK_NONBLOCK | SOCK_CLOEXEC)) {
        frame->rax = (uint64_t)-EINVAL;
        return;
    }
    if ((domain == AF_PACKET || (domain == AF_INET && base_type == SOCK_RAW)) && (!current_task_ptr || current_task_ptr->euid != 0)) {
        frame->rax = (uint64_t)-EPERM;
        return;
    }

    int r = create_socket(domain, base_type, protocol, &sock);
    if (r < 0) { frame->rax = (uint64_t)r; return; }
    sock->flags = socket_flags & SOCK_NONBLOCK;

    int fd = alloc_fd_handle(&current_task_ptr->fd_table, "socket", FD_SOCKET, O_RDWR | (socket_flags & SOCK_NONBLOCK), sock);
    if (fd < 0) {
        release_socket(sock);
        frame->rax = (uint64_t)fd;
        return;
    }
    frame->rax = (uint64_t)fd;
}

void sys_connect(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    const void *addr = (const void *)frame->rsi;
    uint32_t addrlen = (uint32_t)frame->rdx;
    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }
    if (entry->type != FD_SOCKET) { frame->rax = (uint64_t)-ENOTSOCK; return; }
    if (addrlen > 128) { frame->rax = (uint64_t)-EINVAL; return; }
    if (addrlen > 0 && !user_range_ok(current_task_ptr->ctx, (uint64_t)addr, addrlen)) { frame->rax = (uint64_t)-EFAULT; return; }
    uint8_t kaddr[128];
    memset(kaddr, 0, sizeof(kaddr));
    uint32_t copy_len = (addrlen < sizeof(kaddr)) ? addrlen : sizeof(kaddr);
    if (read_vmm(current_task_ptr->ctx, kaddr, (uint64_t)addr, copy_len) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    int access = prepare_unix_socket_path(kaddr, &copy_len, false);
    if (access < 0) { frame->rax = (uint64_t)access; return; }
    socket_t *sock = (socket_t *)entry->handle;
    if (!sock || !sock->ops || !sock->ops->connect) { frame->rax = (uint64_t)-EINVAL; return; }
    frame->rax = (uint64_t)sock->ops->connect(sock, kaddr, copy_len);
}

void sys_accept(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    fd_entry_t *entry = get_current_fd(fd);
    socket_t *accepted = NULL;
    int r;
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }
    if (entry->type != FD_SOCKET) { frame->rax = (uint64_t)-ENOTSOCK; return; }
    socket_t *sock = (socket_t *)entry->handle;
    if (!sock || !sock->ops || !sock->ops->accept) { frame->rax = (uint64_t)-EINVAL; return; }

    r = sock->ops->accept(sock, &accepted);
    if (r < 0) { frame->rax = (uint64_t)r; return; }
    int newfd = alloc_fd_handle(&current_task_ptr->fd_table, "socket:accepted", FD_SOCKET, O_RDWR, accepted);
    if (newfd < 0) {
        release_socket(accepted);
        frame->rax = (uint64_t)newfd;
        return;
    }
    frame->rax = (uint64_t)newfd;
}

void sys_sendto(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    const void *buf = (const void *)frame->rsi;
    size_t len = (size_t)frame->rdx;
    int flags = (int)frame->r10;
    const void *dest_addr = (const void *)frame->r8;
    socklen_t addrlen = (socklen_t)frame->r9;

    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }
    if (entry->type != FD_SOCKET) { frame->rax = (uint64_t)-ENOTSOCK; return; }
    if (len > MAX_IO_COUNT) { frame->rax = (uint64_t)-EINVAL; return; }
    if (addrlen > 128) { frame->rax = (uint64_t)-EINVAL; return; }
    if (len > 0 && !user_range_ok(current_task_ptr->ctx, (uint64_t)buf, len)) { frame->rax = (uint64_t)-EFAULT; return; }

    uint8_t kaddr[128];
    if (dest_addr && addrlen > 0) {
        if (!user_range_ok(current_task_ptr->ctx, (uint64_t)dest_addr, addrlen)) { frame->rax = (uint64_t)-EFAULT; return; }
        uint32_t copy_len = (addrlen < sizeof(kaddr)) ? addrlen : sizeof(kaddr);
        if (read_vmm(current_task_ptr->ctx, kaddr, (uint64_t)dest_addr, copy_len) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    }

    uint8_t *kbuf = malloc(len);
    if (!kbuf) { frame->rax = (uint64_t)-ENOMEM; return; }
    if (read_vmm(current_task_ptr->ctx, kbuf, (uint64_t)buf, len) < 0) { free(kbuf); frame->rax = (uint64_t)-EFAULT; return; }

    socket_t *sock = (socket_t *)entry->handle;
    int64_t w = -EBADF;
    if (sock && sock->ops && sock->ops->sendto) {
        w = sock->ops->sendto(sock, kbuf, len, flags, (dest_addr && addrlen > 0) ? kaddr : NULL, dest_addr ? addrlen : 0);
    }
    free(kbuf);
    frame->rax = (uint64_t)w;
}

void sys_recvfrom(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    void *buf = (void *)frame->rsi;
    size_t len = (size_t)frame->rdx;
    int flags = (int)frame->r10;
    void *src_addr = (void *)frame->r8;
    socklen_t *addrlen_ptr = (socklen_t *)frame->r9;

    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }
    if (entry->type != FD_SOCKET) { frame->rax = (uint64_t)-ENOTSOCK; return; }
    if (len > MAX_IO_COUNT) { frame->rax = (uint64_t)-EINVAL; return; }
    if (len > 0 && !user_write_range_ok(current_task_ptr->ctx, (uint64_t)buf, len)) { frame->rax = (uint64_t)-EFAULT; return; }

    uint8_t kaddr[128] = {0};
    socklen_t user_addrlen = 0;
    socklen_t kaddrlen = 0;
    if (src_addr || addrlen_ptr) {
        if (!src_addr || !addrlen_ptr || copy_from_user(&user_addrlen, addrlen_ptr, sizeof(user_addrlen)) < 0) {
            frame->rax = (uint64_t)-EFAULT;
            return;
        }
        kaddrlen = user_addrlen < sizeof(kaddr) ? user_addrlen : sizeof(kaddr);
        if (!user_write_range_ok(current_task_ptr->ctx, (uint64_t)src_addr, kaddrlen) || !user_write_range_ok(current_task_ptr->ctx, (uint64_t)addrlen_ptr, sizeof(user_addrlen))) {
            frame->rax = (uint64_t)-EFAULT;
            return;
        }
    }
    uint8_t *kbuf = malloc(len);
    if (!kbuf) { frame->rax = (uint64_t)-ENOMEM; return; }

    socket_t *sock = (socket_t *)entry->handle;
    int64_t got = -EBADF;
    if (sock && sock->ops && sock->ops->recvfrom) {
        got = sock->ops->recvfrom(sock, kbuf, len, flags, kaddr, &kaddrlen);
    }
    if (got > (int64_t)len) {
        got = -EIO;
    }
    if (got >= 0) {
        if (write_vmm(current_task_ptr->ctx, (uint64_t)buf, kbuf, (size_t)got) < 0) { free(kbuf); frame->rax = (uint64_t)-EFAULT; return; }
        if (src_addr && addrlen_ptr) {
            socklen_t copy_len = kaddrlen < user_addrlen ? kaddrlen : user_addrlen;
            if (copy_len > sizeof(kaddr)) copy_len = sizeof(kaddr);
            if (copy_len) (void)write_vmm(current_task_ptr->ctx, (uint64_t)src_addr, kaddr, copy_len);
            (void)write_vmm(current_task_ptr->ctx, (uint64_t)addrlen_ptr, &kaddrlen, sizeof(socklen_t));
        }
    }
    free(kbuf);
    frame->rax = (uint64_t)got;
}

void sys_sendmsg(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    const struct msghdr *user_msg = (const struct msghdr *)frame->rsi;
    int flags = (int)frame->rdx;
    struct msghdr msg;
    struct iovec *iov = NULL;
    uint8_t *buf = NULL;
    uint8_t name[128];
    size_t total = 0;
    int64_t result = -EBADF;

    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }
    if (entry->type != FD_SOCKET) { frame->rax = (uint64_t)-ENOTSOCK; return; }
    if (copy_from_user(&msg, user_msg, sizeof(msg)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    if (msg.msg_iovlen > MAX_IOV || msg.msg_namelen > sizeof(name)) { frame->rax = (uint64_t)-EINVAL; return; }
    if (msg.msg_iovlen) {
        size_t iov_size = msg.msg_iovlen * sizeof(*iov);
        iov = malloc(iov_size);
        if (!iov) { frame->rax = (uint64_t)-ENOMEM; return; }
        if (copy_from_user(iov, msg.msg_iov, iov_size) < 0) { result = -EFAULT; goto out; }
        for (size_t i = 0; i < msg.msg_iovlen; i++) {
            if (iov[i].iov_len > MAX_IO_COUNT - total || (iov[i].iov_len && !user_range_ok(current_task_ptr->ctx, (uint64_t)iov[i].iov_base, iov[i].iov_len))) {
                result = -EFAULT;
                goto out;
            }
            total += iov[i].iov_len;
        }
    }
    if (msg.msg_name && msg.msg_namelen && copy_from_user(name, msg.msg_name, msg.msg_namelen) < 0) {
        result = -EFAULT;
        goto out;
    }

    buf = malloc(total ? total : 1);
    if (!buf) { result = -ENOMEM; goto out; }
    size_t offset = 0;
    for (size_t i = 0; i < msg.msg_iovlen; i++) {
        if (iov[i].iov_len && copy_from_user(buf + offset, iov[i].iov_base, iov[i].iov_len) < 0) {
            result = -EFAULT;
            goto out;
        }
        offset += iov[i].iov_len;
    }

    socket_t *sock = (socket_t *)entry->handle;
    if (sock && sock->ops && sock->ops->sendto) {
        result = sock->ops->sendto(sock, buf, total, flags, msg.msg_name ? name : NULL, msg.msg_namelen);
    }

out:
    if (buf) free(buf);
    if (iov) free(iov);
    frame->rax = (uint64_t)result;
}

void sys_recvmsg(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    struct msghdr *user_msg = (struct msghdr *)frame->rsi;
    int flags = (int)frame->rdx;
    struct msghdr msg;
    struct iovec *iov = NULL;
    uint8_t *buf = NULL;
    uint8_t name[128] = {0};
    socklen_t name_len;
    size_t total = 0;
    int64_t result = -EBADF;

    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }
    if (entry->type != FD_SOCKET) { frame->rax = (uint64_t)-ENOTSOCK; return; }
    if (copy_from_user(&msg, user_msg, sizeof(msg)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    if (msg.msg_iovlen > MAX_IOV || msg.msg_namelen > sizeof(name)) { frame->rax = (uint64_t)-EINVAL; return; }
    if (msg.msg_name && msg.msg_namelen && !user_write_range_ok(current_task_ptr->ctx, (uint64_t)msg.msg_name, msg.msg_namelen)) {
        frame->rax = (uint64_t)-EFAULT;
        return;
    }

    if (msg.msg_iovlen) {
        size_t iov_size = msg.msg_iovlen * sizeof(*iov);
        iov = malloc(iov_size);
        if (!iov) { frame->rax = (uint64_t)-ENOMEM; return; }
        if (copy_from_user(iov, msg.msg_iov, iov_size) < 0) { result = -EFAULT; goto out; }
        for (size_t i = 0; i < msg.msg_iovlen; i++) {
            if (iov[i].iov_len > MAX_IO_COUNT - total || (iov[i].iov_len && !user_write_range_ok(current_task_ptr->ctx, (uint64_t)iov[i].iov_base, iov[i].iov_len))) {
                result = -EFAULT;
                goto out;
            }
            total += iov[i].iov_len;
        }
    }

    buf = malloc(total ? total : 1);
    if (!buf) { result = -ENOMEM; goto out; }
    name_len = msg.msg_name ? msg.msg_namelen : 0;
    socket_t *sock = (socket_t *)entry->handle;
    if (sock && sock->ops && sock->ops->recvfrom) {
        result = sock->ops->recvfrom(sock, buf, total, flags, msg.msg_name ? name : NULL, msg.msg_name ? &name_len : NULL);
    }
    if (result < 0) goto out;
    if ((uint64_t)result > total) { result = -EIO; goto out; }

    size_t remaining = (size_t)result;
    size_t offset = 0;
    for (size_t i = 0; i < msg.msg_iovlen && remaining; i++) {
        size_t copy_len = iov[i].iov_len < remaining ? iov[i].iov_len : remaining;
        if (copy_to_user(iov[i].iov_base, buf + offset, copy_len) < 0) { result = -EFAULT; goto out; }
        offset += copy_len;
        remaining -= copy_len;
    }
    if (msg.msg_name && name_len) {
        socklen_t copy_len = name_len < msg.msg_namelen ? name_len : msg.msg_namelen;
        if (copy_len > sizeof(name)) copy_len = sizeof(name);
        if (copy_len && copy_to_user(msg.msg_name, name, copy_len) < 0) { result = -EFAULT; goto out; }
    }
    msg.msg_namelen = name_len;
    msg.msg_controllen = 0;
    msg.msg_flags = 0;
    if (copy_to_user(user_msg, &msg, sizeof(msg)) < 0) result = -EFAULT;

out:
    if (buf) free(buf);
    if (iov) free(iov);
    frame->rax = (uint64_t)result;
}

void sys_shutdown(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    int how = (int)frame->rsi;
    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }
    if (entry->type != FD_SOCKET) { frame->rax = (uint64_t)-ENOTSOCK; return; }
    socket_t *sock = (socket_t *)entry->handle;
    if (!sock || !sock->ops || !sock->ops->shutdown) { frame->rax = (uint64_t)-EINVAL; return; }
    frame->rax = (uint64_t)sock->ops->shutdown(sock, how);
}

void sys_bind(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    const void *addr = (const void *)frame->rsi;
    uint32_t addrlen = (uint32_t)frame->rdx;
    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }
    if (entry->type != FD_SOCKET) { frame->rax = (uint64_t)-ENOTSOCK; return; }
    if (addrlen > 128) { frame->rax = (uint64_t)-EINVAL; return; }
    if (addrlen > 0 && !user_range_ok(current_task_ptr->ctx, (uint64_t)addr, addrlen)) { frame->rax = (uint64_t)-EFAULT; return; }
    uint8_t kaddr[128];
    memset(kaddr, 0, sizeof(kaddr));
    uint32_t copy_len = (addrlen < sizeof(kaddr)) ? addrlen : sizeof(kaddr);
    if (read_vmm(current_task_ptr->ctx, kaddr, (uint64_t)addr, copy_len) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    int access = prepare_unix_socket_path(kaddr, &copy_len, true);
    if (access < 0) { frame->rax = (uint64_t)access; return; }
    socket_t *sock = (socket_t *)entry->handle;
    if (!sock || !sock->ops || !sock->ops->bind) { frame->rax = (uint64_t)-EINVAL; return; }
    frame->rax = (uint64_t)sock->ops->bind(sock, kaddr, copy_len);
}

void sys_listen(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    int backlog = (int)frame->rsi;
    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }
    if (entry->type != FD_SOCKET) { frame->rax = (uint64_t)-ENOTSOCK; return; }
    socket_t *sock = (socket_t *)entry->handle;
    if (!sock || !sock->ops || !sock->ops->listen) { frame->rax = (uint64_t)-EINVAL; return; }
    frame->rax = (uint64_t)sock->ops->listen(sock, backlog);
}

void sys_getsockname(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    void *user_addr = (void *)frame->rsi;
    socklen_t *user_addrlen = (socklen_t *)frame->rdx;
    uint8_t addr[128];
    socklen_t addrlen;

    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }
    if (entry->type != FD_SOCKET) { frame->rax = (uint64_t)-ENOTSOCK; return; }
    if (!user_addr || !user_addrlen || copy_from_user(&addrlen, user_addrlen, sizeof(addrlen)) < 0) {
        frame->rax = (uint64_t)-EFAULT;
        return;
    }
    if (addrlen > sizeof(addr)) addrlen = sizeof(addr);
    if (!user_write_range_ok(current_task_ptr->ctx, (uint64_t)user_addr, addrlen) || !user_write_range_ok(current_task_ptr->ctx, (uint64_t)user_addrlen, sizeof(addrlen))) {
        frame->rax = (uint64_t)-EFAULT;
        return;
    }

    socket_t *sock = (socket_t *)entry->handle;
    if (!sock || !sock->ops || !sock->ops->getsockname) { frame->rax = (uint64_t)-EOPNOTSUPP; return; }
    int result = sock->ops->getsockname(sock, addr, &addrlen);
    if (result < 0) { frame->rax = (uint64_t)result; return; }
    if (addrlen > sizeof(addr)) { frame->rax = (uint64_t)-EIO; return; }

    if (copy_to_user(user_addr, addr, addrlen) < 0 || copy_to_user(user_addrlen, &addrlen, sizeof(addrlen)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    frame->rax = 0;
}

void sys_getpeername(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    void *user_addr = (void *)frame->rsi;
    socklen_t *user_addrlen = (socklen_t *)frame->rdx;
    uint8_t addr[128];
    socklen_t addrlen;

    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }
    if (entry->type != FD_SOCKET) { frame->rax = (uint64_t)-ENOTSOCK; return; }
    if (!user_addr || !user_addrlen || copy_from_user(&addrlen, user_addrlen, sizeof(addrlen)) < 0) {
        frame->rax = (uint64_t)-EFAULT;
        return;
    }
    if (addrlen > sizeof(addr)) addrlen = sizeof(addr);
    if (!user_write_range_ok(current_task_ptr->ctx, (uint64_t)user_addr, addrlen) || !user_write_range_ok(current_task_ptr->ctx, (uint64_t)user_addrlen, sizeof(addrlen))) {
        frame->rax = (uint64_t)-EFAULT;
        return;
    }

    socket_t *sock = (socket_t *)entry->handle;
    if (!sock || !sock->ops || !sock->ops->getpeername) { frame->rax = (uint64_t)-EOPNOTSUPP; return; }
    int result = sock->ops->getpeername(sock, addr, &addrlen);
    if (result < 0) { frame->rax = (uint64_t)result; return; }
    if (addrlen > sizeof(addr)) { frame->rax = (uint64_t)-EIO; return; }

    if (copy_to_user(user_addr, addr, addrlen) < 0 || copy_to_user(user_addrlen, &addrlen, sizeof(addrlen)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    frame->rax = 0;
}

void sys_socketpair(syscall_frame_t *frame) {
    int domain = (int)frame->rdi;
    int type = (int)frame->rsi;
    int protocol = (int)frame->rdx;
    int *sv = (int *)frame->r10;
    socket_t *a = NULL;
    socket_t *b = NULL;
    int fds[2];
    int r;

    if (!sv) { frame->rax = (uint64_t)-EINVAL; return; }
    if (!user_write_range_ok(current_task_ptr->ctx, (uint64_t)sv, sizeof(int) * 2)) { frame->rax = (uint64_t)-EFAULT; return; }
    r = create_socketpair(domain, type, protocol, &a, &b);
    if (r < 0) { frame->rax = (uint64_t)r; return; }

    fds[0] = alloc_fd_handle(&current_task_ptr->fd_table, "socketpair", FD_SOCKET, O_RDWR, a);
    if (fds[0] < 0) {
        release_socket(a);
        release_socket(b);
        frame->rax = (uint64_t)fds[0];
        return;
    }
    fds[1] = alloc_fd_handle(&current_task_ptr->fd_table, "socketpair", FD_SOCKET, O_RDWR, b);
    if (fds[1] < 0) {
        free_fd(&current_task_ptr->fd_table, fds[0]);
        release_socket(b);
        frame->rax = (uint64_t)fds[1];
        return;
    }

    if (write_vmm(current_task_ptr->ctx, (uint64_t)sv, fds, sizeof(fds)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    frame->rax = 0;
}

void sys_setsockopt(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    int level = (int)frame->rsi;
    int optname = (int)frame->rdx;
    fd_entry_t *entry = get_current_fd(fd);
    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }
    if (entry->type != FD_SOCKET) { frame->rax = (uint64_t)-ENOTSOCK; return; }
    if (level != SOL_SOCKET) { frame->rax = (uint64_t)-ENOPROTOOPT; return; }
    switch (optname) {
        case SO_REUSEADDR:
        case SO_KEEPALIVE:
        case SO_BROADCAST:
        case SO_LINGER:
            frame->rax = 0;
            return;
        case SO_BINDTODEVICE: {
            const char *name = (const char *)frame->r10;
            socklen_t length = (socklen_t)frame->r8;
            char interface[IFNAMSIZ];
            if (!name || length == 0 || length > IFNAMSIZ) { frame->rax = (uint64_t)-EINVAL; return; }
            memset(interface, 0, sizeof(interface));
            if (copy_from_user(interface, name, length) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
            interface[IFNAMSIZ - 1] = '\0';
            if (strcmp(interface, "eth0") != 0) { frame->rax = (uint64_t)-ENODEV; return; }
            frame->rax = 0;
            return;
        }
        default:
            frame->rax = (uint64_t)-ENOPROTOOPT;
            return;
    }
}

void sys_getsockopt(syscall_frame_t *frame) {
    int fd = (int)frame->rdi;
    int level = (int)frame->rsi;
    int optname = (int)frame->rdx;
    int *optval = (int *)frame->r10;
    uint32_t *optlen = (uint32_t *)frame->r8;
    fd_entry_t *entry = get_current_fd(fd);
    int val;

    if (!entry) { frame->rax = (uint64_t)-EBADF; return; }
    if (entry->type != FD_SOCKET) { frame->rax = (uint64_t)-ENOTSOCK; return; }
    if (level != SOL_SOCKET) { frame->rax = (uint64_t)-EINVAL; return; }
    if (!user_write_range_ok(current_task_ptr->ctx, (uint64_t)optval, sizeof(int)) || !user_write_range_ok(current_task_ptr->ctx, (uint64_t)optlen, sizeof(uint32_t))) {
        frame->rax = (uint64_t)-EFAULT; return;
    }
    if (optname == SO_ERROR) val = get_unix_socket_error((unix_handle_t *)entry->handle);
    else if (optname == SO_TYPE) val = get_unix_socket_type((unix_handle_t *)entry->handle);
    else { frame->rax = (uint64_t)-ENOPROTOOPT; return; }
    if (write_vmm(current_task_ptr->ctx, (uint64_t)optval, &val, sizeof(int)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    uint32_t len = sizeof(int);
    if (write_vmm(current_task_ptr->ctx, (uint64_t)optlen, &len, sizeof(uint32_t)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
    frame->rax = 0;
}

void sys_futex(syscall_frame_t *frame) {
    uint32_t *uaddr = (uint32_t *)frame->rdi;
    int op = (int)frame->rsi;
    uint32_t val = (uint32_t)frame->rdx;
    struct timespec *timeout_ptr = (struct timespec *)frame->r10;
    uint32_t *uaddr2 = (uint32_t *)frame->r8;
    uint32_t val3 = (uint32_t)frame->r9;

    int cmd = op & FUTEX_CMD_MASK;

    uint64_t phys = resolve_futex_key(uaddr, frame);
    if (!phys) return;

    switch (cmd) {

    case FUTEX_WAIT: {
        wait_futex(frame, phys, val, timeout_ptr, FUTEX_BITSET_MATCH_ANY, false);
        return;
    }

    case FUTEX_WAIT_BITSET: {
        if (val3 == 0) { frame->rax = (uint64_t)-EINVAL; return; }

        wait_futex(frame, phys, val, timeout_ptr, val3, true);
        return;
    }

    case FUTEX_WAKE: {
        if (val3 == 0) val3 = FUTEX_BITSET_MATCH_ANY;
        int woken = wake_futex(phys, val, FUTEX_BITSET_MATCH_ANY);
        frame->rax = (uint64_t)woken;
        return;
    }

    case FUTEX_WAKE_BITSET: {
        if (val3 == 0) { frame->rax = (uint64_t)-EINVAL; return; }
        int woken = wake_futex(phys, val, val3);
        frame->rax = (uint64_t)woken;
        return;
    }

    case FUTEX_REQUEUE: {
        uint32_t val2 = (uint32_t)(uintptr_t)timeout_ptr;

        if (!uaddr2 || !user_range_ok(current_task_ptr->ctx, (uint64_t)uaddr2, sizeof(uint32_t))) {
            frame->rax = (uint64_t)-EFAULT; return;
        }

        uint64_t phys2 = get_vmm_phys(current_task_ptr->ctx, (uint64_t)uaddr2);
        if (!phys2) { frame->rax = (uint64_t)-EFAULT; return; }

        int woken = 0, requeued = 0;
        uint64_t irq_flags;
        spin_lock_irqsave(&futex_lock, &irq_flags);

        for (int i = 0; i < MAX_FUTEX_WAITERS; i++) {
            if (futex_waiters[i].state != FW_WAITING) continue;
            if (futex_waiters[i].phys_addr != phys) continue;

            if ((uint32_t)woken < val) {
                futex_waiters[i].state = FW_WOKEN;
                int idx = futex_waiters[i].task_idx;
                if (idx >= 0 && idx < MAX_TASKS && tasks[idx]->state == TASK_STOPPED)
                    tasks[idx]->state = TASK_READY;
                woken++;
            } else if ((uint32_t)requeued < val2) {
                futex_waiters[i].phys_addr = phys2;
                requeued++;
            } else {
                break;
            }
        }

        spin_unlock_irqrestore(&futex_lock, irq_flags);
        frame->rax = (uint64_t)woken;
        return;
    }

    case FUTEX_CMP_REQUEUE: {
        uint32_t val2 = (uint32_t)(uintptr_t)timeout_ptr;

        if (!uaddr2 || !user_range_ok(current_task_ptr->ctx, (uint64_t)uaddr2, sizeof(uint32_t))) {
            frame->rax = (uint64_t)-EFAULT; return;
        }
        uint64_t phys2 = get_vmm_phys(current_task_ptr->ctx, (uint64_t)uaddr2);
        if (!phys2) { frame->rax = (uint64_t)-EFAULT; return; }

        uint64_t irq_flags;
        spin_lock_irqsave(&futex_lock, &irq_flags);

        uint32_t cur_val = 0;
        if (read_vmm(current_task_ptr->ctx, &cur_val, (uint64_t)uaddr, sizeof(uint32_t)) < 0) { spin_unlock_irqrestore(&futex_lock, irq_flags); frame->rax = (uint64_t)-EFAULT; return; }
        if (cur_val != val3) {
            spin_unlock_irqrestore(&futex_lock, irq_flags);
            frame->rax = (uint64_t)-EAGAIN;
            return;
        }

        int woken = 0, requeued = 0;
        for (int i = 0; i < MAX_FUTEX_WAITERS; i++) {
            if (futex_waiters[i].state != FW_WAITING) continue;
            if (futex_waiters[i].phys_addr != phys) continue;

            if ((uint32_t)woken < val) {
                futex_waiters[i].state = FW_WOKEN;
                int idx = futex_waiters[i].task_idx;
                if (idx >= 0 && idx < MAX_TASKS && tasks[idx]->state == TASK_STOPPED)
                    tasks[idx]->state = TASK_READY;
                woken++;
            } else if ((uint32_t)requeued < val2) {
                futex_waiters[i].phys_addr = phys2;
                requeued++;
            } else {
                break;
            }
        }

        spin_unlock_irqrestore(&futex_lock, irq_flags);
        frame->rax = (uint64_t)woken;
        return;
    }

    case FUTEX_WAKE_OP: {
        uint32_t val2 = (uint32_t)(uintptr_t)timeout_ptr;

        if (!uaddr2 || !user_write_range_ok(current_task_ptr->ctx, (uint64_t)uaddr2, sizeof(uint32_t))) {
            frame->rax = (uint64_t)-EFAULT; return;
        }
        uint64_t phys2 = get_vmm_phys(current_task_ptr->ctx, (uint64_t)uaddr2);
        if (!phys2) { frame->rax = (uint64_t)-EFAULT; return; }

        int      fop      = (int)((val3 >> 28) & 0x7U);
        int      fopshift = (int)((val3 >> 28) & 0x8U);
        uint32_t op_arg   = (val3 >> 12) & 0xFFFU;
        int      fcmp     = (int)((val3 >> 24) & 0xFU);
        uint32_t cmp_arg  = val3 & 0xFFFU;

        if (fopshift) op_arg = 1U << (op_arg & 0x1F);

        uint64_t irq_flags;
        spin_lock_irqsave(&futex_lock, &irq_flags);

        uint32_t oldval = 0;
        if (read_vmm(current_task_ptr->ctx, &oldval, (uint64_t)uaddr2, sizeof(uint32_t)) < 0) { spin_unlock_irqrestore(&futex_lock, irq_flags); frame->rax = (uint64_t)-EFAULT; return; }

        uint32_t newval = oldval;
        switch (fop) {
            case FUTEX_OP_SET:  newval = op_arg;           break;
            case FUTEX_OP_ADD:  newval = oldval + op_arg;  break;
            case FUTEX_OP_OR:   newval = oldval | op_arg;  break;
            case FUTEX_OP_ANDN: newval = oldval & ~op_arg; break;
            case FUTEX_OP_XOR:  newval = oldval ^ op_arg;  break;
            default:
                spin_unlock_irqrestore(&futex_lock, irq_flags);
                frame->rax = (uint64_t)-ENOSYS;
                return;
        }
        if (write_vmm(current_task_ptr->ctx, (uint64_t)uaddr2, &newval, sizeof(uint32_t)) < 0) { spin_unlock_irqrestore(&futex_lock, irq_flags); frame->rax = (uint64_t)-EFAULT; return; }

        int woken = 0;
        for (int i = 0; i < MAX_FUTEX_WAITERS && (uint32_t)woken < val; i++) {
            if (futex_waiters[i].state != FW_WAITING) continue;
            if (futex_waiters[i].phys_addr != phys) continue;
            futex_waiters[i].state = FW_WOKEN;
            int idx = futex_waiters[i].task_idx;
            if (idx >= 0 && idx < MAX_TASKS && tasks[idx]->state == TASK_STOPPED)
                tasks[idx]->state = TASK_READY;
            woken++;
        }

        bool cond = false;
        switch (fcmp) {
            case FUTEX_OP_CMP_EQ: cond = (oldval == cmp_arg);                         break;
            case FUTEX_OP_CMP_NE: cond = (oldval != cmp_arg);                         break;
            case FUTEX_OP_CMP_LT: cond = ((int32_t)oldval <  (int32_t)cmp_arg);       break;
            case FUTEX_OP_CMP_LE: cond = ((int32_t)oldval <= (int32_t)cmp_arg);       break;
            case FUTEX_OP_CMP_GT: cond = ((int32_t)oldval >  (int32_t)cmp_arg);       break;
            case FUTEX_OP_CMP_GE: cond = ((int32_t)oldval >= (int32_t)cmp_arg);       break;
            default:
                spin_unlock_irqrestore(&futex_lock, irq_flags);
                frame->rax = (uint64_t)-ENOSYS;
                return;
        }

        if (cond) {
            for (int i = 0; i < MAX_FUTEX_WAITERS && (uint32_t)(woken - (int)val) < val2; i++) {
                if (futex_waiters[i].state != FW_WAITING) continue;
                if (futex_waiters[i].phys_addr != phys2) continue;
                futex_waiters[i].state = FW_WOKEN;
                int idx = futex_waiters[i].task_idx;
                if (idx >= 0 && idx < MAX_TASKS && tasks[idx]->state == TASK_STOPPED)
                    tasks[idx]->state = TASK_READY;
                woken++;
            }
        }

        spin_unlock_irqrestore(&futex_lock, irq_flags);
        frame->rax = (uint64_t)woken;
        return;
    }

    default:
        frame->rax = (uint64_t)-ENOSYS;
        return;
    }
}

void sys_epoll_create(syscall_frame_t *frame) {
    int size = frame->rdi;
    (void)size; // size is ignored in modern Linux, but we still accept it for compatibility
    frame->rax = (uint64_t)(int64_t)do_epoll_create1(0);
}

void sys_epoll_wait(syscall_frame_t *frame) {
    int timeout_ms = (int)frame->r10;
    int64_t timeout_us = (timeout_ms < 0) ? -1 : (int64_t)timeout_ms * 1000LL;
    do_epoll_wait(frame, timeout_us, 0);
}

void sys_epoll_ctl(syscall_frame_t *frame) {
    int epfd = (int)frame->rdi;
    int op   = (int)frame->rsi;
    int fd   = (int)frame->rdx;
    struct epoll_event *user_event = (struct epoll_event *)frame->r10;

    fd_entry_t *ep_entry = get_current_fd(epfd);
    if (!ep_entry || !ep_entry->open || ep_entry->type != FD_EPOLL) {
        frame->rax = (uint64_t)-EBADF; return;
    }
    epoll_instance_t *epi = (epoll_instance_t *)ep_entry->handle;
    if (!epi) { frame->rax = (uint64_t)-EBADF; return; }

    // The target fd must be valid
    fd_entry_t *target = get_current_fd(fd);
    if (!target || !target->open) {
        frame->rax = (uint64_t)-EBADF; return;
    }
    // Cannot epoll an epoll fd (avoid loops)
    if (target->type == FD_EPOLL) {
        frame->rax = (uint64_t)-EINVAL; return;
    }

    switch (op) {
    case EPOLL_CTL_ADD: {
        if (!user_event || !user_range_ok(current_task_ptr->ctx, (uint64_t)user_event, sizeof(struct epoll_event))) {
            frame->rax = (uint64_t)-EFAULT; return;
        }
        if (epoll_find_interest(epi, fd) >= 0) {
            frame->rax = (uint64_t)-EEXIST; return;
        }
        if (epi->count >= MAX_EPOLL_INTERESTS) {
            frame->rax = (uint64_t)-ENOMEM; return;
        }
        struct epoll_event ev;
        if (copy_from_user(&ev, user_event, sizeof(ev)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
        epoll_interest_t *interest = &epi->interests[epi->count];
        interest->watched_fd      = fd;
        interest->events          = ev.events;
        interest->data            = ev.data;
        interest->oneshot_reported = false;
        epi->count++;
        frame->rax = 0;
        break;
    }
    case EPOLL_CTL_MOD: {
        if (!user_event || !user_range_ok(current_task_ptr->ctx, (uint64_t)user_event, sizeof(struct epoll_event))) {
            frame->rax = (uint64_t)-EFAULT; return;
        }
        int idx = epoll_find_interest(epi, fd);
        if (idx < 0) {
            frame->rax = (uint64_t)-ENOENT; return;
        }
        struct epoll_event ev;
        if (copy_from_user(&ev, user_event, sizeof(ev)) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
        epi->interests[idx].events           = ev.events;
        epi->interests[idx].data             = ev.data;
        epi->interests[idx].oneshot_reported = false;
        frame->rax = 0;
        break;
    }
    case EPOLL_CTL_DEL: {
        int idx = epoll_find_interest(epi, fd);
        if (idx < 0) {
            frame->rax = (uint64_t)-ENOENT; return;
        }
        // Swap with last and shrink
        epi->interests[idx] = epi->interests[epi->count - 1];
        epi->count--;
        frame->rax = 0;
        break;
    }
    default:
        frame->rax = (uint64_t)-EINVAL;
        break;
    }
}

void sys_epoll_pwait(syscall_frame_t *frame) {
    int epfd = (int)frame->rdi;
    struct epoll_event *user_events = (struct epoll_event *)frame->rsi;
    int maxevents = (int)frame->rdx;
    int timeout_ms = (int)frame->r10;
    uint64_t sigmask_ptr = frame->r8;
    uint64_t sigsetsize = frame->r9;

    int64_t timeout_us = (timeout_ms < 0) ? -1 : (int64_t)timeout_ms * 1000LL;

    if (maxevents <= 0) {
        frame->rax = (uint64_t)-EINVAL; return;
    }
    if (maxevents > MAX_EPOLL_INTERESTS) maxevents = MAX_EPOLL_INTERESTS;

    fd_entry_t *ep_entry = get_current_fd(epfd);
    if (!ep_entry || !ep_entry->open || ep_entry->type != FD_EPOLL) {
        frame->rax = (uint64_t)-EBADF; return;
    }
    epoll_instance_t *epi = (epoll_instance_t *)ep_entry->handle;
    if (!epi) {
        frame->rax = (uint64_t)-EBADF; return;
    }

    size_t events_bytes = (size_t)maxevents * sizeof(struct epoll_event);
    if (!user_events || !user_write_range_ok(current_task_ptr->ctx, (uint64_t)user_events, events_bytes)) {
        frame->rax = (uint64_t)-EFAULT; return;
    }

    uint64_t old_blocked = current_task_ptr->blocked_signals;
    int mask_swapped = 0;

    if (sigmask_ptr) {
        if (!user_range_ok(current_task_ptr->ctx, (uint64_t)(void *)sigmask_ptr, sigsetsize)) {
            frame->rax = (uint64_t)-EFAULT; return;
        }
        if (sigsetsize != 8) {
            frame->rax = (uint64_t)-EINVAL; return;
        }        uint64_t new_mask = 0;
        if (read_vmm(current_task_ptr->ctx, &new_mask, sigmask_ptr, 8) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
        new_mask &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
        current_task_ptr->blocked_signals = new_mask;
        mask_swapped = 1;
    }

    int j = 0;
    for (int i = 0; i < epi->count; i++) {
        fd_entry_t *e = get_current_fd(epi->interests[i].watched_fd);
        if (e && e->open) {
            if (j != i) epi->interests[j] = epi->interests[i];
            j++;
        }
    }

    epi->count = j;

    struct epoll_event *k_events = malloc(events_bytes);
    if (!k_events) {
        if (mask_swapped) current_task_ptr->blocked_signals = old_blocked;
        frame->rax = (uint64_t)-ENOMEM; return;
    }

    int count = epoll_collect(epi, k_events, maxevents);
    uint64_t start = get_monotonic_time_us();

    while (count == 0 && timeout_us != 0) {
        if (signal_pending()) {
            count = -EINTR;
            break;
        }
        if (timeout_us > 0 && (int64_t)(get_monotonic_time_us() - start) >= timeout_us) {
            count = 0;
            break;
        }

        spin_lock(&sched_lock);
        let_current_task_sleep(1000);
        spin_unlock(&sched_lock);

        count = epoll_collect(epi, k_events, maxevents);
    }

    if (mask_swapped) current_task_ptr->blocked_signals = old_blocked;

    if (count > 0 && copy_to_user(user_events, k_events, (size_t)count * sizeof(struct epoll_event)) < 0) count = -EFAULT;
    free(k_events);
    frame->rax = (uint64_t)count;
}

void sys_epoll_create1(syscall_frame_t *frame) {
    int flags = (int)frame->rdi;
    frame->rax = (uint64_t)(int64_t)do_epoll_create1(flags);
}

void sys_pipe2(syscall_frame_t *frame) {
    int *pipefd = (int *)frame->rdi;
    int flags = (int)frame->rsi;
    unix_handle_t *read_end = NULL;
    unix_handle_t *write_end = NULL;
    int fds[2];
    int fd_flags;
    int r;

    if (!pipefd) { frame->rax = (uint64_t)-EINVAL; return; }
    if (!user_write_range_ok(current_task_ptr->ctx, (uint64_t)pipefd, sizeof(int) * 2)) { frame->rax = (uint64_t)-EFAULT; return; }
    if (flags & ~(O_CLOEXEC | O_NONBLOCK)) { frame->rax = (uint64_t)-EINVAL; return; }

    r = create_unix_pipe(&read_end, &write_end);
    if (r < 0) { frame->rax = (uint64_t)r; return; }

    fd_flags = flags & O_NONBLOCK;
    fds[0] = alloc_fd_handle(&current_task_ptr->fd_table, "pipe:r", FD_PIPE, O_RDONLY | fd_flags, read_end);
    if (fds[0] < 0) {
        release_unix_handle(read_end);
        release_unix_handle(write_end);
        frame->rax = (uint64_t)fds[0];
        return;
    }

    fds[1] = alloc_fd_handle(&current_task_ptr->fd_table, "pipe:w", FD_PIPE, O_WRONLY | fd_flags, write_end);
    if (fds[1] < 0) {
        free_fd(&current_task_ptr->fd_table, fds[0]);
        release_unix_handle(write_end);
        frame->rax = (uint64_t)fds[1];
        return;
    }

    if (write_vmm(current_task_ptr->ctx, (uint64_t)pipefd, fds, sizeof(fds)) < 0) { free_fd(&current_task_ptr->fd_table, fds[0]); free_fd(&current_task_ptr->fd_table, fds[1]); frame->rax = (uint64_t)-EFAULT; return; }
    frame->rax = 0;
}

void sys_epoll_pwait2(syscall_frame_t *frame) {
    int epfd = (int)frame->rdi;
    struct epoll_event *user_events = (struct epoll_event *)frame->rsi;
    int maxevents = (int)frame->rdx;
    struct timespec *timeout_ptr = (struct timespec *)frame->r10;
    uint64_t sigmask_ptr = frame->r8;
    uint64_t sigsetsize = frame->r9;

    int64_t timeout_us = -1; // infinite by default

    if (maxevents <= 0) {
        frame->rax = (uint64_t)-EINVAL; return;
    }
    if (maxevents > MAX_EPOLL_INTERESTS) maxevents = MAX_EPOLL_INTERESTS;

    if (timeout_ptr) {
        if (!user_range_ok(current_task_ptr->ctx, (uint64_t)timeout_ptr, sizeof(struct timespec))) {
            frame->rax = (uint64_t)-EFAULT; return;
        }
        struct timespec ts;
        if (copy_from_user(&ts, timeout_ptr, sizeof(ts)) < 0) {
            frame->rax = (uint64_t)-EFAULT; return;
        }
        if (timespec_to_us(&ts, &timeout_us) < 0) { frame->rax = (uint64_t)-EINVAL; return; }
    }

    fd_entry_t *ep_entry = get_current_fd(epfd);
    if (!ep_entry || !ep_entry->open || ep_entry->type != FD_EPOLL) {
        frame->rax = (uint64_t)-EBADF; return;
    }
    epoll_instance_t *epi = (epoll_instance_t *)ep_entry->handle;
    if (!epi) {
        frame->rax = (uint64_t)-EBADF; return;
    }

    size_t events_bytes = (size_t)maxevents * sizeof(struct epoll_event);
    if (!user_events || !user_write_range_ok(current_task_ptr->ctx, (uint64_t)user_events, events_bytes)) {
        frame->rax = (uint64_t)-EFAULT; return;
    }

    uint64_t old_blocked = current_task_ptr->blocked_signals;
    int mask_swapped = 0;

    if (sigmask_ptr) {
        if (!user_range_ok(current_task_ptr->ctx, (uint64_t)(void *)sigmask_ptr, sigsetsize)) {
            frame->rax = (uint64_t)-EFAULT; return;
        }
        if (sigsetsize != 8) {
            frame->rax = (uint64_t)-EINVAL; return;
        }        uint64_t new_mask = 0;
        if (read_vmm(current_task_ptr->ctx, &new_mask, sigmask_ptr, 8) < 0) { frame->rax = (uint64_t)-EFAULT; return; }
        new_mask &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
        current_task_ptr->blocked_signals = new_mask;
        mask_swapped = 1;
    }

    int j = 0;
    for (int i = 0; i < epi->count; i++) {
        fd_entry_t *e = get_current_fd(epi->interests[i].watched_fd);
        if (e && e->open) {
            if (j != i) epi->interests[j] = epi->interests[i];
            j++;
        }
    }

    epi->count = j;

    struct epoll_event *k_events = malloc(events_bytes);
    if (!k_events) {
        if (mask_swapped) current_task_ptr->blocked_signals = old_blocked;
        frame->rax = (uint64_t)-ENOMEM; return;
    }

    int count = epoll_collect(epi, k_events, maxevents);
    uint64_t start = get_monotonic_time_us();

    while (count == 0 && timeout_us != 0) {
        if (signal_pending()) {
            count = -EINTR;
            break;
        }
        if (timeout_us > 0 && (int64_t)(get_monotonic_time_us() - start) >= timeout_us) {
            count = 0;
            break;
        }

        spin_lock(&sched_lock);
        let_current_task_sleep(1000);
        spin_unlock(&sched_lock);

        count = epoll_collect(epi, k_events, maxevents);
    }

    if (mask_swapped) current_task_ptr->blocked_signals = old_blocked;

    if (count > 0 && copy_to_user(user_events, k_events, (size_t)count * sizeof(struct epoll_event)) < 0) count = -EFAULT;
    free(k_events);
    frame->rax = (uint64_t)count;
}
