#include <stdbool.h>
#include <errno.h>
#include <netinet/in.h>
#include <main/string.h>
#include <main/spinlocks.h>
#include <main/sched.h>
#include <main/halt.h>
#include <io/net_sockets.h>
#include <io/net.h>
#include <io/io.h>
#include <mm/mm.h>

#define UDP_RING_SIZE   16
#define UDP_MAX_PAYLOAD 1472

typedef struct {
    uint8_t  data[UDP_MAX_PAYLOAD];
    uint16_t len;
    uint32_t src_ip;
    uint16_t src_port;
} udp_datagram_t;

typedef struct inet_socket_obj {
    spinlock_t       lock;
    uint32_t         local_ip;
    uint16_t         local_port;
    uint32_t         remote_ip;
    uint16_t         remote_port;
    tcp_socket_t    *tcp_sock;
    udp_datagram_t   udp_ring[UDP_RING_SIZE];
    uint32_t         udp_head;
    uint32_t         udp_tail;
    uint32_t         udp_count;
    struct inet_socket_obj *next;
} inet_socket_t;

static spinlock_t inet_sockets_lock = SPINLOCK_INIT;
static inet_socket_t *inet_sockets_list = NULL;

static int inet_op_bind(socket_t *sock, const void *addr, socklen_t addrlen) {
    if (!sock || !addr || addrlen < sizeof(sockaddr_in_t)) return -EINVAL;
    const sockaddr_in_t *in = (const sockaddr_in_t *)addr;
    if (in->sin_family != AF_INET) return -EAFNOSUPPORT;

    inet_socket_t *inet = (inet_socket_t *)sock->priv;
    if (!inet) return -EINVAL;

    inet->local_ip = in->sin_addr.s_addr;
    inet->local_port = ntohs(in->sin_port);
    return 0;
}

static int inet_op_connect(socket_t *sock, const void *addr, socklen_t addrlen) {
    if (!sock || !addr || addrlen < sizeof(sockaddr_in_t)) return -EINVAL;
    const sockaddr_in_t *in = (const sockaddr_in_t *)addr;
    if (in->sin_family != AF_INET) return -EAFNOSUPPORT;

    inet_socket_t *inet = (inet_socket_t *)sock->priv;
    if (!inet) return -EINVAL;

    inet->remote_ip = in->sin_addr.s_addr;
    inet->remote_port = ntohs(in->sin_port);

    if (sock->type == SOCK_STREAM) {
        if (inet->tcp_sock) close_tcp(inet->tcp_sock);
        inet->tcp_sock = connect_tcp(inet->remote_ip, inet->remote_port);
        if (!inet->tcp_sock) return -ECONNREFUSED;
    }
    return 0;
}

static int inet_op_listen(socket_t *sock, int backlog) {
    (void)backlog;
    if (!sock || sock->type != SOCK_STREAM) return -EOPNOTSUPP;
    return 0;
}

static int inet_op_accept(socket_t *sock, socket_t **out_sock) {
    (void)sock; (void)out_sock;
    return -EOPNOTSUPP;
}

static int64_t inet_op_sendto(socket_t *sock, const void *buf, size_t len, int flags, const void *dest_addr, socklen_t addrlen) {
    (void)flags;
    if (!sock || !buf) return -EINVAL;
    inet_socket_t *inet = (inet_socket_t *)sock->priv;
    if (!inet) return -EINVAL;

    if (sock->type == SOCK_STREAM) {
        if (!inet->tcp_sock) return -ENOTCONN;
        bool ok = send_tcp(inet->tcp_sock, buf, (uint16_t)len);
        return ok ? (int64_t)len : -EIO;
    } else if (sock->type == SOCK_DGRAM) {
        uint32_t dest_ip = inet->remote_ip;
        uint16_t dest_port = inet->remote_port;

        if (dest_addr && addrlen >= sizeof(sockaddr_in_t)) {
            const sockaddr_in_t *in = (const sockaddr_in_t *)dest_addr;
            if (in->sin_family == AF_INET) {
                dest_ip = in->sin_addr.s_addr;
                dest_port = ntohs(in->sin_port);
            }
        }
        if (!dest_ip) return -EDESTADDRREQ;
        uint16_t src_port = inet->local_port ? inet->local_port : 50000;
        bool ok = send_udp_packet(dest_ip, src_port, dest_port, buf, (uint16_t)len);
        return ok ? (int64_t)len : -EIO;
    }
    return -EOPNOTSUPP;
}

static int64_t inet_op_recvfrom(socket_t *sock, void *buf, size_t len, int flags, void *src_addr, socklen_t *addrlen) {
    if (!sock || !buf) return -EINVAL;
    inet_socket_t *inet = (inet_socket_t *)sock->priv;
    if (!inet) return -EINVAL;

    if (sock->type == SOCK_STREAM) {
        if (!inet->tcp_sock) return -ENOTCONN;
        int r;
        for (;;) {
            r = read_tcp(inet->tcp_sock, buf, (int)len);
            if (r > 0) break;
            if (!check_tcp_connected(inet->tcp_sock)) break;
            if ((sock->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT)) return -EAGAIN;
            poll_tcp(inet->tcp_sock);
            update_interval_timers();
            if (signal_pending()) return -EINTR;
            io_wait();
        }
        if (src_addr && addrlen && *addrlen >= sizeof(sockaddr_in_t)) {
            sockaddr_in_t *in = (sockaddr_in_t *)src_addr;
            memset(in, 0, sizeof(*in));
            in->sin_family = AF_INET;
            in->sin_port = htons(inet->remote_port);
            in->sin_addr.s_addr = inet->remote_ip;
            *addrlen = sizeof(*in);
        }
        return (int64_t)r;
    } else if (sock->type == SOCK_DGRAM) {
        uint64_t flags_lock;

        poll_net_device();

        if ((sock->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT)) {
            spin_lock_irqsave(&inet->lock, &flags_lock);
            if (inet->udp_count == 0) {
                spin_unlock_irqrestore(&inet->lock, flags_lock);
                return -EAGAIN;
            }
        } else {
            // isr32 skips schedule() while sched_lock is held, so we must
            // release it and fire cpu_yield() to actually yield to the scheduler, // letting NIC interrupts deliver incoming datagrams.
            spin_unlock(&sched_lock);
            do { yield_sched(); } while (inet->udp_count == 0);
            spin_lock(&sched_lock);

            spin_lock_irqsave(&inet->lock, &flags_lock);
        }

        udp_datagram_t *d = &inet->udp_ring[inet->udp_head];
        size_t copy_len = (len < d->len) ? len : d->len;
        memcpy(buf, d->data, copy_len);
        uint32_t s_ip = d->src_ip;
        uint16_t s_port = d->src_port;

        inet->udp_head = (inet->udp_head + 1) % UDP_RING_SIZE;
        inet->udp_count--;
        spin_unlock_irqrestore(&inet->lock, flags_lock);

        if (src_addr && addrlen && *addrlen >= sizeof(sockaddr_in_t)) {
            sockaddr_in_t *in = (sockaddr_in_t *)src_addr;
            memset(in, 0, sizeof(*in));
            in->sin_family = AF_INET;
            in->sin_port = htons(s_port);
            in->sin_addr.s_addr = s_ip;
            *addrlen = sizeof(*in);
        }
        return (int64_t)copy_len;
    }
    return -EOPNOTSUPP;
}

static int inet_op_getsockname(socket_t *sock, void *addr, socklen_t *addrlen) {
    if (!sock || !addr || !addrlen || *addrlen < sizeof(sockaddr_in_t)) return -EINVAL;
    inet_socket_t *inet = (inet_socket_t *)sock->priv;
    if (!inet) return -EINVAL;

    sockaddr_in_t *in = (sockaddr_in_t *)addr;
    memset(in, 0, sizeof(*in));
    in->sin_family = AF_INET;
    in->sin_port = htons(inet->local_port);
    in->sin_addr.s_addr = inet->local_ip;
    *addrlen = sizeof(*in);
    return 0;
}

static int inet_op_getpeername(socket_t *sock, void *addr, socklen_t *addrlen) {
    if (!sock || !addr || !addrlen || *addrlen < sizeof(sockaddr_in_t)) return -EINVAL;
    inet_socket_t *inet = (inet_socket_t *)sock->priv;
    if (!inet || !inet->remote_ip) return -ENOTCONN;

    sockaddr_in_t *in = (sockaddr_in_t *)addr;
    memset(in, 0, sizeof(*in));
    in->sin_family = AF_INET;
    in->sin_port = htons(inet->remote_port);
    in->sin_addr.s_addr = inet->remote_ip;
    *addrlen = sizeof(*in);
    return 0;
}

static int inet_op_getsockopt(socket_t *sock, int level, int optname, void *optval, socklen_t *optlen) {
    (void)sock; (void)level; (void)optname; (void)optval; (void)optlen;
    return 0;
}

static int inet_op_setsockopt(socket_t *sock, int level, int optname, const void *optval, socklen_t optlen) {
    (void)sock; (void)level; (void)optname; (void)optval; (void)optlen;
    return 0;
}

static int inet_op_shutdown(socket_t *sock, int how) {
    (void)how;
    inet_socket_t *inet = (inet_socket_t *)sock->priv;
    if (inet && inet->tcp_sock) close_tcp(inet->tcp_sock);
    return 0;
}

static void inet_op_close(socket_t *sock) {
    if (sock && sock->priv) {
        inet_socket_t *inet = (inet_socket_t *)sock->priv;

        uint64_t flags;
        spin_lock_irqsave(&inet_sockets_lock, &flags);
        if (inet_sockets_list == inet) {
            inet_sockets_list = inet->next;
        } else {
            inet_socket_t *curr = inet_sockets_list;
            while (curr && curr->next != inet) curr = curr->next;
            if (curr) curr->next = inet->next;
        }
        spin_unlock_irqrestore(&inet_sockets_lock, flags);

        if (inet->tcp_sock) {
            close_tcp(inet->tcp_sock);
            free_tcp(inet->tcp_sock);
        }
        free(inet);
        sock->priv = NULL;
    }
}

static int64_t inet_op_read(socket_t *sock, void *buf, size_t count, uint32_t fd_flags) {
    int flags = (fd_flags & O_NONBLOCK) ? MSG_DONTWAIT : 0;
    return inet_op_recvfrom(sock, buf, count, flags, NULL, NULL);
}

static int64_t inet_op_write(socket_t *sock, const void *buf, size_t count, uint32_t fd_flags) {
    (void)fd_flags;
    return inet_op_sendto(sock, buf, count, 0, NULL, 0);
}

static bool inet_op_is_readable(socket_t *sock) {
    if (!sock || !sock->priv) return false;
    inet_socket_t *inet = (inet_socket_t *)sock->priv;
    if (sock->type == SOCK_DGRAM) return inet->udp_count != 0;
    if (sock->type == SOCK_STREAM && inet->tcp_sock) return inet->tcp_sock->rx_head != inet->tcp_sock->rx_tail || inet->tcp_sock->rx_fin;
    return false;
}

void net_udp_tap_rx(uint32_t src_ip, uint16_t src_port, uint16_t dst_port, const uint8_t *payload, uint16_t len) {
    if (!payload || len == 0 || len > UDP_MAX_PAYLOAD) return;
    uint64_t flags;
    spin_lock_irqsave(&inet_sockets_lock, &flags);

    inet_socket_t *curr = inet_sockets_list;
    while (curr) {
        spin_lock(&curr->lock);
        if (curr->local_port == 0 || curr->local_port == dst_port) {
            if (curr->udp_count < UDP_RING_SIZE) {
                udp_datagram_t *d = &curr->udp_ring[curr->udp_tail];
                memcpy(d->data, payload, len);
                d->len = len;
                d->src_ip = src_ip;
                d->src_port = src_port;
                curr->udp_tail = (curr->udp_tail + 1) % UDP_RING_SIZE;
                curr->udp_count++;
            }
        }
        spin_unlock(&curr->lock);
        curr = curr->next;
    }

    spin_unlock_irqrestore(&inet_sockets_lock, flags);
}

static const socket_ops_t inet_socket_ops = {
    .bind        = inet_op_bind, .connect     = inet_op_connect, .listen      = inet_op_listen, .accept      = inet_op_accept, .sendto      = inet_op_sendto, .recvfrom    = inet_op_recvfrom, .getsockname = inet_op_getsockname, .getpeername = inet_op_getpeername, .getsockopt  = inet_op_getsockopt, .setsockopt  = inet_op_setsockopt, .shutdown    = inet_op_shutdown, .close       = inet_op_close, .read        = inet_op_read, .write       = inet_op_write, .is_readable = inet_op_is_readable,
};

int create_inet_socket_obj(int type, int protocol, socket_t **out) {
    if (!out) return -EINVAL;
    if (type != SOCK_STREAM && type != SOCK_DGRAM && type != SOCK_RAW) return -ESOCKTNOSUPPORT;

    inet_socket_t *inet = malloc(sizeof(inet_socket_t));
    if (!inet) return -ENOMEM;
    memset(inet, 0, sizeof(inet_socket_t));
    inet->lock = SPINLOCK_INIT;

    socket_t *s = malloc(sizeof(socket_t));
    if (!s) { free(inet); return -ENOMEM; }
    memset(s, 0, sizeof(socket_t));

    s->lock = SPINLOCK_INIT;
    s->refcount = 1;
    s->domain = AF_INET;
    s->type = type;
    s->protocol = protocol;
    s->ops = &inet_socket_ops;
    s->priv = inet;

    uint64_t flags;
    spin_lock_irqsave(&inet_sockets_lock, &flags);
    inet->next = inet_sockets_list;
    inet_sockets_list = inet;
    spin_unlock_irqrestore(&inet_sockets_lock, flags);

    *out = s;
    return 0;
}
