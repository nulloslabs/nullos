#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <main/spinlocks.h>

#define ETH_P_ALL 0x0003
#define ETH_P_IP  0x0800
#define ETH_P_ARP 0x0806

typedef struct {
    sa_family_t sa_family;
    char sa_data[14];
} sockaddr_t;

typedef struct {
    sa_family_t sun_family;
    char sun_path[108];
} sockaddr_un_t;

struct in_addr_sys {
    uint32_t s_addr;
};

typedef struct {
    sa_family_t sin_family;
    uint16_t sin_port;
    struct in_addr_sys sin_addr;
    char sin_zero[8];
} sockaddr_in_t;

typedef struct {
    uint16_t sll_family;
    uint16_t sll_protocol;
    int sll_ifindex;
    uint16_t sll_hatype;
    uint8_t sll_pkttype;
    uint8_t sll_halen;
    uint8_t sll_addr[8];
} sockaddr_ll_t;

typedef struct socket_obj socket_t;

typedef struct socket_ops {
    int     (*bind)       (socket_t *sock, const void *addr, socklen_t addrlen);
    int     (*connect)    (socket_t *sock, const void *addr, socklen_t addrlen);
    int     (*listen)     (socket_t *sock, int backlog);
    int     (*accept)     (socket_t *sock, socket_t **out_sock);
    int64_t (*sendto)     (socket_t *sock, const void *buf, size_t len, int flags, const void *dest_addr, socklen_t addrlen);
    int64_t (*recvfrom)   (socket_t *sock, void *buf, size_t len, int flags, void *src_addr, socklen_t *addrlen);
    int     (*getsockname)(socket_t *sock, void *addr, socklen_t *addrlen);
    int     (*getpeername)(socket_t *sock, void *addr, socklen_t *addrlen);
    int     (*getsockopt) (socket_t *sock, int level, int optname, void *optval, socklen_t *optlen);
    int     (*setsockopt) (socket_t *sock, int level, int optname, const void *optval, socklen_t optlen);
    int     (*shutdown)   (socket_t *sock, int how);
    void    (*close)      (socket_t *sock);
    int64_t (*read)       (socket_t *sock, void *buf, size_t count, uint32_t fd_flags);
    int64_t (*write)      (socket_t *sock, const void *buf, size_t count, uint32_t fd_flags);
    bool    (*is_readable)(socket_t *sock);
} socket_ops_t;

struct socket_obj {
    spinlock_t lock;
    int refcount;
    int domain;
    int type;
    int protocol;
    int flags;
    const socket_ops_t *ops;
    void *priv;
};

int create_socket(int domain, int type, int protocol, socket_t **out);
int create_socketpair(int domain, int type, int protocol, socket_t **a, socket_t **b);
void retain_socket(socket_t *s);
void release_socket(socket_t *s);
bool is_socket_readable(socket_t *s);
