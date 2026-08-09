#include <errno.h>
#include <main/string.h>
#include <main/spinlocks.h>
#include <main/sched.h>
#include <io/packet_sockets.h>
#include <io/net.h>
#include <mm/mm.h>

#define PACKET_RING_SIZE 16
#define PACKET_MAX_LEN 1514

typedef struct {
    uint8_t data[PACKET_MAX_LEN];
    uint16_t len;
} raw_frame_t;

typedef struct packet_socket_obj {
    spinlock_t lock;
    uint16_t protocol;
    int ifindex;
    raw_frame_t ring[PACKET_RING_SIZE];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    struct packet_socket_obj *next;
} packet_socket_t;

static spinlock_t packet_sockets_lock = SPINLOCK_INIT;
static packet_socket_t *packet_sockets_list = NULL;

void net_packet_tap_rx(const uint8_t *packet, uint16_t len) {
    if (!packet || len == 0 || len > PACKET_MAX_LEN) return;
    uint64_t flags;
    spin_lock_irqsave(&packet_sockets_lock, &flags);

    packet_socket_t *curr = packet_sockets_list;
    while (curr) {
        spin_lock(&curr->lock);
        if (curr->count < PACKET_RING_SIZE) {
            raw_frame_t *f = &curr->ring[curr->tail];
            memcpy(f->data, packet, len);
            f->len = len;
            curr->tail = (curr->tail + 1) % PACKET_RING_SIZE;
            curr->count++;
        }
        spin_unlock(&curr->lock);
        curr = curr->next;
    }

    spin_unlock_irqrestore(&packet_sockets_lock, flags);
}

static int packet_op_bind(socket_t *sock, const void *addr, socklen_t addrlen) {
    if (!sock || !addr || addrlen < sizeof(sockaddr_ll_t)) return -EINVAL;
    const sockaddr_ll_t *sll = (const sockaddr_ll_t *)addr;
    packet_socket_t *pkt = (packet_socket_t *)sock->priv;
    if (!pkt) return -EINVAL;

    pkt->protocol = sll->sll_protocol;
    pkt->ifindex = sll->sll_ifindex;
    return 0;
}

static int packet_op_connect(socket_t *sock, const void *addr, socklen_t addrlen) {
    (void)sock; (void)addr; (void)addrlen;
    return -EOPNOTSUPP;
}

static int packet_op_listen(socket_t *sock, int backlog) {
    (void)sock; (void)backlog;
    return -EOPNOTSUPP;
}

static int packet_op_accept(socket_t *sock, socket_t **out_sock) {
    (void)sock; (void)out_sock;
    return -EOPNOTSUPP;
}

static int64_t packet_op_sendto(socket_t *sock, const void *buf, size_t len, int flags, const void *dest_addr, socklen_t addrlen) {
    (void)flags; (void)dest_addr; (void)addrlen;
    if (!sock || !buf || len == 0 || len > PACKET_MAX_LEN) return -EINVAL;
    if (!net_current_device || !net_current_device->send) return -ENETDOWN;

    bool ok = net_current_device->send(buf, (uint16_t)len);
    return ok ? (int64_t)len : -EIO;
}

static int64_t packet_op_recvfrom(socket_t *sock, void *buf, size_t len, int flags, void *src_addr, socklen_t *addrlen) {
    (void)flags;
    if (!sock || !buf) return -EINVAL;
    packet_socket_t *pkt = (packet_socket_t *)sock->priv;
    if (!pkt) return -EINVAL;

    uint64_t flags_lock;
    spin_lock_irqsave(&pkt->lock, &flags_lock);
    if (pkt->count == 0) {
        spin_unlock_irqrestore(&pkt->lock, flags_lock);
        if (sock->flags & O_NONBLOCK) return -EAGAIN;
        return 0;
    }

    raw_frame_t *f = &pkt->ring[pkt->head];
    size_t copy_len = (len < f->len) ? len : f->len;
    memcpy(buf, f->data, copy_len);
    pkt->head = (pkt->head + 1) % PACKET_RING_SIZE;
    pkt->count--;
    spin_unlock_irqrestore(&pkt->lock, flags_lock);

    if (src_addr && addrlen && *addrlen >= sizeof(sockaddr_ll_t)) {
        sockaddr_ll_t *sll = (sockaddr_ll_t *)src_addr;
        memset(sll, 0, sizeof(*sll));
        sll->sll_family = AF_PACKET;
        sll->sll_protocol = pkt->protocol;
        sll->sll_ifindex = 1;
        *addrlen = sizeof(*sll);
    }
    return (int64_t)copy_len;
}

static int packet_op_getsockname(socket_t *sock, void *addr, socklen_t *addrlen) {
    if (!sock || !addr || !addrlen || *addrlen < sizeof(sockaddr_ll_t)) return -EINVAL;
    packet_socket_t *pkt = (packet_socket_t *)sock->priv;
    if (!pkt) return -EINVAL;

    sockaddr_ll_t *sll = (sockaddr_ll_t *)addr;
    memset(sll, 0, sizeof(*sll));
    sll->sll_family = AF_PACKET;
    sll->sll_protocol = pkt->protocol;
    sll->sll_ifindex = pkt->ifindex;
    *addrlen = sizeof(*sll);
    return 0;
}

static int packet_op_getpeername(socket_t *sock, void *addr, socklen_t *addrlen) {
    (void)sock; (void)addr; (void)addrlen;
    return -ENOTCONN;
}

static int packet_op_getsockopt(socket_t *sock, int level, int optname, void *optval, socklen_t *optlen) {
    (void)sock; (void)level; (void)optname; (void)optval; (void)optlen;
    return 0;
}

static int packet_op_setsockopt(socket_t *sock, int level, int optname, const void *optval, socklen_t optlen) {
    (void)sock; (void)level; (void)optname; (void)optval; (void)optlen;
    return 0;
}

static int packet_op_shutdown(socket_t *sock, int how) {
    (void)sock; (void)how;
    return 0;
}

static void packet_op_close(socket_t *sock) {
    if (!sock || !sock->priv) return;
    packet_socket_t *pkt = (packet_socket_t *)sock->priv;

    uint64_t flags;
    spin_lock_irqsave(&packet_sockets_lock, &flags);
    if (packet_sockets_list == pkt) {
        packet_sockets_list = pkt->next;
    } else {
        packet_socket_t *curr = packet_sockets_list;
        while (curr && curr->next != pkt) curr = curr->next;
        if (curr) curr->next = pkt->next;
    }
    spin_unlock_irqrestore(&packet_sockets_lock, flags);

    free(pkt);
    sock->priv = NULL;
}

static int64_t packet_op_read(socket_t *sock, void *buf, size_t count, uint32_t fd_flags) {
    (void)fd_flags;
    return packet_op_recvfrom(sock, buf, count, 0, NULL, NULL);
}

static int64_t packet_op_write(socket_t *sock, const void *buf, size_t count, uint32_t fd_flags) {
    (void)fd_flags;
    return packet_op_sendto(sock, buf, count, 0, NULL, 0);
}

static const socket_ops_t packet_socket_ops = {
    .bind        = packet_op_bind,
    .connect     = packet_op_connect,
    .listen      = packet_op_listen,
    .accept      = packet_op_accept,
    .sendto      = packet_op_sendto,
    .recvfrom    = packet_op_recvfrom,
    .getsockname = packet_op_getsockname,
    .getpeername = packet_op_getpeername,
    .getsockopt  = packet_op_getsockopt,
    .setsockopt  = packet_op_setsockopt,
    .shutdown    = packet_op_shutdown,
    .close       = packet_op_close,
    .read        = packet_op_read,
    .write       = packet_op_write,
};

int create_packet_socket_obj(int type, int protocol, socket_t **out) {
    if (!out) return -EINVAL;
    if (type != SOCK_RAW && type != SOCK_DGRAM) return -ESOCKTNOSUPPORT;

    packet_socket_t *pkt = malloc(sizeof(packet_socket_t));
    if (!pkt) return -ENOMEM;
    memset(pkt, 0, sizeof(packet_socket_t));
    pkt->lock = SPINLOCK_INIT;
    pkt->protocol = (uint16_t)protocol;

    socket_t *s = malloc(sizeof(socket_t));
    if (!s) { free(pkt); return -ENOMEM; }
    memset(s, 0, sizeof(socket_t));

    s->lock = SPINLOCK_INIT;
    s->refcount = 1;
    s->domain = AF_PACKET;
    s->type = type;
    s->protocol = protocol;
    s->ops = &packet_socket_ops;
    s->priv = pkt;

    uint64_t flags;
    spin_lock_irqsave(&packet_sockets_lock, &flags);
    pkt->next = packet_sockets_list;
    packet_sockets_list = pkt;
    spin_unlock_irqrestore(&packet_sockets_lock, flags);

    *out = s;
    return 0;
}
