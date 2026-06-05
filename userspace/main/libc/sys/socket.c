#include <stddef.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

int socket(int domain, int type, int protocol) {
    return (int)syscall(SYS_socket, domain, type, protocol);
}

int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    return (int)syscall(SYS_bind, sockfd, addr, addrlen);
}

int listen(int sockfd, int backlog) {
    return (int)syscall(SYS_listen, sockfd, backlog);
}

int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    (void)addr;
    (void)addrlen;
    return (int)syscall(SYS_accept, sockfd, addr, addrlen);
}

int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    return (int)syscall(SYS_connect, sockfd, addr, addrlen);
}

ssize_t send(int sockfd, const void *buf, size_t len, int flags) {
    return sendto(sockfd, buf, len, flags, NULL, 0);
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags) {
    return recvfrom(sockfd, buf, len, flags, NULL, NULL);
}

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest_addr, socklen_t addrlen) {
    return (ssize_t)syscall(SYS_sendto, sockfd, buf, len, flags, dest_addr, addrlen);
}

ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
                 struct sockaddr *src_addr, socklen_t *addrlen) {
    return (ssize_t)syscall(SYS_recvfrom, sockfd, buf, len, flags, src_addr, addrlen);
}

int getsockopt(int sockfd, int level, int optname, void *optval,
               socklen_t *optlen) {
    return (int)syscall(SYS_getsockopt, sockfd, level, optname, optval, optlen);
}

int setsockopt(int sockfd, int level, int optname, const void *optval,
               socklen_t optlen) {
    return (int)syscall(SYS_setsockopt, sockfd, level, optname, optval, optlen);
}

int shutdown(int sockfd, int how) {
    return (int)syscall(SYS_shutdown, sockfd, how);
}

int socketpair(int domain, int type, int protocol, int sv[2]) {
    return (int)syscall(SYS_socketpair, domain, type, protocol, sv);
}
