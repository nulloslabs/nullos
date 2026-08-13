#include <stdbool.h>
#include <errno.h>
#include <main/string.h>
#include <main/spinlocks.h>
#include <main/sched.h>
#include <io/unix_sockets.h>
#include <io/tmpfs.h>
#include <mm/mm.h>

static spinlock_t registry_lock = SPINLOCK_INIT;
static unix_binding_t bindings[UNIX_MAX_BINDINGS];

static unix_channel_t *create_unix_channel(void) {
    unix_channel_t *ch = malloc(sizeof(unix_channel_t));
    if (!ch) return NULL;
    memset(ch, 0, sizeof(*ch));
    ch->refs = 1;
    ch->lock = SPINLOCK_INIT;
    return ch;
}

static void retain_unix_channel(unix_channel_t *ch) {
    uint64_t flags;
    if (!ch) return;
    spin_lock_irqsave(&ch->lock, &flags);
    ch->refs++;
    spin_unlock_irqrestore(&ch->lock, flags);
}

static void release_unix_channel(unix_channel_t *ch) {
    uint64_t flags;
    int free_it = 0;
    if (!ch) return;
    spin_lock_irqsave(&ch->lock, &flags);
    ch->refs--;
    if (ch->refs == 0) free_it = 1;
    spin_unlock_irqrestore(&ch->lock, flags);
    if (free_it) free(ch);
}

static unix_handle_t *create_unix_handle(unix_handle_kind_t kind) {
    unix_handle_t *h = malloc(sizeof(unix_handle_t));
    if (!h) return NULL;
    memset(h, 0, sizeof(*h));
    h->lock = SPINLOCK_INIT;
    h->refs = 1;
    h->kind = kind;
    h->sock_type = SOCK_STREAM;
    return h;
}

static void add_unix_channel_reader(unix_channel_t *ch) {
    uint64_t flags;
    spin_lock_irqsave(&ch->lock, &flags);
    ch->readers++;
    spin_unlock_irqrestore(&ch->lock, flags);
}

static void add_unix_channel_writer(unix_channel_t *ch) {
    uint64_t flags;
    spin_lock_irqsave(&ch->lock, &flags);
    ch->writers++;
    spin_unlock_irqrestore(&ch->lock, flags);
}

static void drop_unix_channel_reader(unix_channel_t *ch) {
    uint64_t flags;
    spin_lock_irqsave(&ch->lock, &flags);
    if (ch->readers > 0) ch->readers--;
    spin_unlock_irqrestore(&ch->lock, flags);
}

static void drop_unix_channel_writer(unix_channel_t *ch) {
    uint64_t flags;
    spin_lock_irqsave(&ch->lock, &flags);
    if (ch->writers > 0) ch->writers--;
    spin_unlock_irqrestore(&ch->lock, flags);
}

static void attach_unix_handle_read(unix_handle_t *h, unix_channel_t *ch) {
    h->in = ch;
    retain_unix_channel(ch);
    add_unix_channel_reader(ch);
}

static void attach_unix_handle_write(unix_handle_t *h, unix_channel_t *ch) {
    h->out = ch;
    retain_unix_channel(ch);
    add_unix_channel_writer(ch);
}

void retain_unix_handle(unix_handle_t *h) {
    uint64_t flags;
    if (!h) return;
    spin_lock_irqsave(&h->lock, &flags);
    h->refs++;
    spin_unlock_irqrestore(&h->lock, flags);
}

static void unbind_unix_registry(unix_handle_t *h) {
    uint64_t flags;
    spin_lock_irqsave(&registry_lock, &flags);
    for (int i = 0; i < UNIX_MAX_BINDINGS; i++) {
        if (bindings[i].listener == h) {
            bindings[i].listener = NULL;
            bindings[i].path[0] = '\0';
        }
    }
    spin_unlock_irqrestore(&registry_lock, flags);
}

void release_unix_handle(unix_handle_t *h) {
    uint64_t flags;
    int free_it = 0;
    if (!h) return;

    spin_lock_irqsave(&h->lock, &flags);
    h->refs--;
    if (h->refs == 0) free_it = 1;
    spin_unlock_irqrestore(&h->lock, flags);
    if (!free_it) return;

    if (h->bound) unbind_unix_registry(h);
    while (h->pending_len > 0) {
        unix_handle_t *p = h->pending[h->pending_head];
        h->pending[h->pending_head] = NULL;
        h->pending_head = (h->pending_head + 1) % UNIX_MAX_PENDING;
        h->pending_len--;
        release_unix_handle(p);
    }
    if (h->in) {
        if (!h->rd_shutdown) drop_unix_channel_reader(h->in);
        release_unix_channel(h->in);
    }
    if (h->out) {
        if (!h->wr_shutdown) drop_unix_channel_writer(h->out);
        release_unix_channel(h->out);
    }
    free(h);
}

int create_unix_pipe(unix_handle_t **read_end, unix_handle_t **write_end) {
    unix_channel_t *ch;
    unix_handle_t *r, *w;
    if (!read_end || !write_end) return -EINVAL;

    ch = create_unix_channel();
    if (!ch) return -ENOMEM;
    r = create_unix_handle(UH_PIPE_READ);
    w = create_unix_handle(UH_PIPE_WRITE);
    if (!r || !w) {
        if (r) release_unix_handle(r);
        if (w) release_unix_handle(w);
        release_unix_channel(ch);
        return -ENOMEM;
    }

    attach_unix_handle_read(r, ch);
    attach_unix_handle_write(w, ch);
    release_unix_channel(ch);
    *read_end = r;
    *write_end = w;
    return 0;
}

int create_unix_socket(int domain, int type, int protocol, unix_handle_t **out) {
    unix_handle_t *h;
    if (!out) return -EINVAL;
    if (domain != AF_UNIX && domain != AF_LOCAL) return -EAFNOSUPPORT;
    if (type != SOCK_STREAM && type != SOCK_DGRAM) return -ESOCKTNOSUPPORT;
    if (protocol != 0) return -EPROTONOSUPPORT;
    h = create_unix_handle(UH_SOCKET);
    if (!h) return -ENOMEM;
    h->sock_type = type;
    *out = h;
    return 0;
}

static int create_unix_connected_pair(unix_handle_t **a, unix_handle_t **b) {
    unix_channel_t *ab = create_unix_channel();
    unix_channel_t *ba = create_unix_channel();
    unix_handle_t *ha = create_unix_handle(UH_SOCKET);
    unix_handle_t *hb = create_unix_handle(UH_SOCKET);
    if (!ab || !ba || !ha || !hb) {
        if (ha) release_unix_handle(ha);
        if (hb) release_unix_handle(hb);
        if (ab) release_unix_channel(ab);
        if (ba) release_unix_channel(ba);
        return -ENOMEM;
    }
    attach_unix_handle_read(ha, ba);
    attach_unix_handle_write(ha, ab);
    attach_unix_handle_read(hb, ab);
    attach_unix_handle_write(hb, ba);
    release_unix_channel(ab);
    release_unix_channel(ba);
    *a = ha;
    *b = hb;
    return 0;
}

int create_unix_socketpair(int domain, int type, int protocol, unix_handle_t **a, unix_handle_t **b) {
    if (domain != AF_UNIX && domain != AF_LOCAL) return -EAFNOSUPPORT;
    if (type != SOCK_STREAM) return -ESOCKTNOSUPPORT;
    if (protocol != 0) return -EPROTONOSUPPORT;
    return create_unix_connected_pair(a, b);
}

int64_t read_unix_handle(unix_handle_t *h, void *buf, size_t count, uint32_t fd_flags) {
    unix_channel_t *ch;
    uint8_t *out = (uint8_t *)buf;
    size_t done = 0;
    if (!h || !buf) return -EINVAL;
    if (!h->in || h->rd_shutdown) return -EPIPE;
    ch = h->in;

    while (done < count) {
        if (signal_pending()) return -EINTR;

        uint64_t flags;
        spin_lock_irqsave(&ch->lock, &flags);
        while (done < count && ch->len > 0) {
            out[done++] = ch->buf[ch->head];
            ch->head = (ch->head + 1) % UNIX_BUF_SIZE;
            ch->len--;
        }
        int writers = ch->writers;
        spin_unlock_irqrestore(&ch->lock, flags);

        if (done || count == 0) return (int64_t)done;
        if (writers == 0) return 0;
        if (fd_flags & O_NONBLOCK) return -EAGAIN;
        current_task_ptr->state = TASK_READY;
        spin_unlock(&sched_lock);
        __asm__ volatile("int $32");
        spin_lock(&sched_lock);
        current_task_ptr->state = TASK_RUNNING;
    }
    return (int64_t)done;
}

int64_t write_unix_handle(unix_handle_t *h, const void *buf, size_t count, uint32_t fd_flags) {
    unix_channel_t *ch;
    const uint8_t *in = (const uint8_t *)buf;
    size_t done = 0;
    if (!h || !buf) return -EINVAL;
    if (!h->out || h->wr_shutdown) return -EPIPE;
    ch = h->out;

    while (done < count) {
        uint64_t flags;
        spin_lock_irqsave(&ch->lock, &flags);
        if (ch->readers == 0) {
            spin_unlock_irqrestore(&ch->lock, flags);
            return done ? (int64_t)done : -EPIPE;
        }
        while (done < count && ch->len < UNIX_BUF_SIZE) {
            ch->buf[ch->tail] = in[done++];
            ch->tail = (ch->tail + 1) % UNIX_BUF_SIZE;
            ch->len++;
        }
        spin_unlock_irqrestore(&ch->lock, flags);

        if (done == count || count == 0) return (int64_t)done;
        if (fd_flags & O_NONBLOCK) return done ? (int64_t)done : -EAGAIN;
        current_task_ptr->state = TASK_READY;
        spin_unlock(&sched_lock);
        __asm__ volatile("int $32");
        spin_lock(&sched_lock);
        current_task_ptr->state = TASK_RUNNING;
    }
    return (int64_t)done;
}

static int sockaddr_path(const void *addr, uint32_t addrlen, char *out, size_t out_size) {
    const sockaddr_un_t *un = (const sockaddr_un_t *)addr;
    size_t max;
    if (!addr || addrlen < sizeof(uint16_t) + 1) return -EINVAL;
    if (un->sun_family != AF_UNIX && un->sun_family != AF_LOCAL) return -EAFNOSUPPORT;
    max = addrlen - sizeof(uint16_t);
    if (max >= out_size) max = out_size - 1;

    /* Abstract sockets: sun_path[0] == '\0'. Encode as '@' + hex of bytes */
    if (un->sun_path[0] == '\0') {
        const unsigned char *p = (const unsigned char *)un->sun_path;
        size_t w = 0;
        if (out_size < 2) return -EINVAL;
        out[w++] = '@';
        for (size_t i = 1; i < max && (w + 2) < out_size; i++) {
            const char *hex = "0123456789abcdef";
            unsigned char b = p[i];
            out[w++] = hex[(b >> 4) & 0xf];
            out[w++] = hex[b & 0xf];
        }
        out[w] = '\0';
        if (!out[0]) return -EINVAL;
        return 0;
    }

    memcpy(out, un->sun_path, max);
    out[max] = '\0';
    if (!out[0]) return -EINVAL;
    return 0;
}

int bind_unix_socket(unix_handle_t *h, const void *addr, uint32_t addrlen) {
    char path[108];
    uint64_t flags;
    int slot = -1;
    int r = sockaddr_path(addr, addrlen, path, sizeof(path));
    if (r < 0) return r;
    if (!h || h->kind != UH_SOCKET) return -ENOTSOCK;

    spin_lock_irqsave(&registry_lock, &flags);
    for (int i = 0; i < UNIX_MAX_BINDINGS; i++) {
        if (bindings[i].listener && strcmp(bindings[i].path, path) == 0) {
            spin_unlock_irqrestore(&registry_lock, flags);
            return -EADDRINUSE;
        }
        if (!bindings[i].listener && slot < 0) slot = i;
    }
    if (slot < 0) {
        spin_unlock_irqrestore(&registry_lock, flags);
        return -ENOSPC;
    }
    strncpy(bindings[slot].path, path, sizeof(bindings[slot].path) - 1);
    bindings[slot].path[sizeof(bindings[slot].path) - 1] = '\0';
    bindings[slot].listener = h;
    h->bound = 1;
    strncpy(h->path, path, sizeof(h->path) - 1);
    h->path[sizeof(h->path) - 1] = '\0';
    spin_unlock_irqrestore(&registry_lock, flags);
    if (path[0] != '@') {
        int in = write_tmpfs(path, NULL, 0, S_IFSOCK | 0777,
                             current_task_ptr->euid, current_task_ptr->egid);
        if (in < 0) {
            unbind_unix_registry(h);
            return in;
        }
    }
    return 0;
}

int listen_unix_socket(unix_handle_t *h, int backlog) {
    (void)backlog;
    if (!h || h->kind != UH_SOCKET) return -ENOTSOCK;
    if (!h->bound) return -EINVAL;
    h->listening = 1;
    return 0;
}

static unix_handle_t *find_unix_listener(const char *path) {
    unix_handle_t *h = NULL;
    uint64_t flags;
    spin_lock_irqsave(&registry_lock, &flags);
    for (int i = 0; i < UNIX_MAX_BINDINGS; i++) {
        if (bindings[i].listener && strcmp(bindings[i].path, path) == 0) {
            h = bindings[i].listener;
            retain_unix_handle(h);
            break;
        }
    }
    spin_unlock_irqrestore(&registry_lock, flags);
    return h;
}

int connect_unix_socket(unix_handle_t *h, const void *addr, uint32_t addrlen) {
    char path[108];
    unix_handle_t *listener;
    unix_handle_t *client;
    unix_handle_t *server;
    uint64_t listener_flags;
    uint64_t handle_flags;
    int r = sockaddr_path(addr, addrlen, path, sizeof(path));
    if (r < 0) return r;
    if (!h || h->kind != UH_SOCKET) return -ENOTSOCK;
    if (h->in || h->out) return -EISCONN;

    listener = find_unix_listener(path);
    if (!listener) return -ECONNREFUSED;
    if (!listener->listening) {
        release_unix_handle(listener);
        return -ECONNREFUSED;
    }

    r = create_unix_connected_pair(&client, &server);
    if (r < 0) {
        release_unix_handle(listener);
        return r;
    }

    spin_lock_irqsave(&listener->lock, &listener_flags);
    if (!listener->listening || listener->pending_len >= UNIX_MAX_PENDING) {
        spin_unlock_irqrestore(&listener->lock, listener_flags);
        release_unix_handle(client);
        release_unix_handle(server);
        release_unix_handle(listener);
        return -ECONNREFUSED;
    }

    spin_lock_irqsave(&h->lock, &handle_flags);
    if (h->in || h->out) {
        spin_unlock_irqrestore(&h->lock, handle_flags);
        spin_unlock_irqrestore(&listener->lock, listener_flags);
        release_unix_handle(client);
        release_unix_handle(server);
        release_unix_handle(listener);
        return -EISCONN;
    }
    h->in = client->in;
    h->out = client->out;
    retain_unix_channel(h->in);
    retain_unix_channel(h->out);
    add_unix_channel_reader(h->in);
    add_unix_channel_writer(h->out);
    spin_unlock_irqrestore(&h->lock, handle_flags);
    release_unix_handle(client);

    listener->pending[listener->pending_tail] = server;
    listener->pending_tail = (listener->pending_tail + 1) % UNIX_MAX_PENDING;
    listener->pending_len++;
    spin_unlock_irqrestore(&listener->lock, listener_flags);
    release_unix_handle(listener);
    return 0;
}

int accept_unix_socket(unix_handle_t *h, unix_handle_t **out) {
    if (!h || h->kind != UH_SOCKET || !out) return -EINVAL;
    if (!h->listening) return -EINVAL;
    for (;;) {
        uint64_t flags;
        spin_lock_irqsave(&h->lock, &flags);
        if (h->pending_len > 0) {
            unix_handle_t *p = h->pending[h->pending_head];
            h->pending[h->pending_head] = NULL;
            h->pending_head = (h->pending_head + 1) % UNIX_MAX_PENDING;
            h->pending_len--;
            spin_unlock_irqrestore(&h->lock, flags);
            *out = p;
            return 0;
        }
        spin_unlock_irqrestore(&h->lock, flags);
        current_task_ptr->state = TASK_READY;
        spin_unlock(&sched_lock);
        __asm__ volatile("int $32");
        spin_lock(&sched_lock);
        current_task_ptr->state = TASK_RUNNING;
    }
}

int shutdown_unix_socket(unix_handle_t *h, int how) {
    if (!h || h->kind != UH_SOCKET) return -ENOTSOCK;
    if (how == SHUT_RD || how == SHUT_RDWR) {
        if (!h->rd_shutdown && h->in) drop_unix_channel_reader(h->in);
        h->rd_shutdown = 1;
    }
    if (how == SHUT_WR || how == SHUT_RDWR) {
        if (!h->wr_shutdown && h->out) drop_unix_channel_writer(h->out);
        h->wr_shutdown = 1;
    }
    return 0;
}

int get_unix_socket_error(unix_handle_t *h) {
    if (!h || h->kind != UH_SOCKET) return -ENOTSOCK;
    return 0;
}

int get_unix_socket_type(unix_handle_t *h) {
    if (!h || h->kind != UH_SOCKET) return -ENOTSOCK;
    return h->sock_type;
}

/* --- Unix Socket VTable Operations --- */

static int unix_op_bind(socket_t *sock, const void *addr, socklen_t addrlen) {
    return bind_unix_socket((unix_handle_t *)sock->priv, addr, addrlen);
}

static int unix_op_connect(socket_t *sock, const void *addr, socklen_t addrlen) {
    return connect_unix_socket((unix_handle_t *)sock->priv, addr, addrlen);
}

static int unix_op_listen(socket_t *sock, int backlog) {
    return listen_unix_socket((unix_handle_t *)sock->priv, backlog);
}

static int unix_op_accept(socket_t *sock, socket_t **out_sock) {
    unix_handle_t *accepted = NULL;
    int r = accept_unix_socket((unix_handle_t *)sock->priv, &accepted);
    if (r < 0) return r;
    *out_sock = create_unix_socket_obj(accepted);
    if (!*out_sock) {
        release_unix_handle(accepted);
        return -ENOMEM;
    }
    return 0;
}

static int64_t unix_op_sendto(socket_t *sock, const void *buf, size_t len, int flags, const void *dest_addr, socklen_t addrlen) {
    (void)flags; (void)dest_addr; (void)addrlen;
    return write_unix_handle((unix_handle_t *)sock->priv, buf, len, sock->flags);
}

static int64_t unix_op_recvfrom(socket_t *sock, void *buf, size_t len, int flags, void *src_addr, socklen_t *addrlen) {
    (void)flags; (void)src_addr;
    if (addrlen) *addrlen = 0;
    return read_unix_handle((unix_handle_t *)sock->priv, buf, len, sock->flags);
}

static int unix_op_getsockname(socket_t *sock, void *addr, socklen_t *addrlen) {
    unix_handle_t *h = (unix_handle_t *)sock->priv;
    if (!h || !addr || !addrlen) return -EINVAL;
    sockaddr_un_t un;
    memset(&un, 0, sizeof(un));
    un.sun_family = AF_UNIX;
    strncpy(un.sun_path, h->path, sizeof(un.sun_path) - 1);
    socklen_t copy = sizeof(un);
    if (*addrlen < copy) copy = *addrlen;
    memcpy(addr, &un, copy);
    *addrlen = copy;
    return 0;
}

static int unix_op_getpeername(socket_t *sock, void *addr, socklen_t *addrlen) {
    (void)sock; (void)addr; (void)addrlen;
    return -ENOTCONN;
}

static int unix_op_getsockopt(socket_t *sock, int level, int optname, void *optval, socklen_t *optlen) {
    (void)sock; (void)level; (void)optname; (void)optval; (void)optlen;
    return 0;
}

static int unix_op_setsockopt(socket_t *sock, int level, int optname, const void *optval, socklen_t optlen) {
    (void)sock; (void)level; (void)optname; (void)optval; (void)optlen;
    return 0;
}

static int unix_op_shutdown(socket_t *sock, int how) {
    return shutdown_unix_socket((unix_handle_t *)sock->priv, how);
}

static void unix_op_close(socket_t *sock) {
    if (sock && sock->priv) {
        release_unix_handle((unix_handle_t *)sock->priv);
        sock->priv = NULL;
    }
}

static int64_t unix_op_read(socket_t *sock, void *buf, size_t count, uint32_t fd_flags) {
    return read_unix_handle((unix_handle_t *)sock->priv, buf, count, fd_flags);
}

static int64_t unix_op_write(socket_t *sock, const void *buf, size_t count, uint32_t fd_flags) {
    return write_unix_handle((unix_handle_t *)sock->priv, buf, count, fd_flags);
}

static bool unix_op_is_readable(socket_t *sock) {
    if (!sock || !sock->priv) return false;
    unix_handle_t *h = (unix_handle_t *)sock->priv;
    if (h->listening) return h->pending_len != 0;
    return h->in && (h->in->len != 0 || h->in->writers == 0);
}

static const socket_ops_t unix_socket_ops = {
    .bind        = unix_op_bind,
    .connect     = unix_op_connect,
    .listen      = unix_op_listen,
    .accept      = unix_op_accept,
    .sendto      = unix_op_sendto,
    .recvfrom    = unix_op_recvfrom,
    .getsockname = unix_op_getsockname,
    .getpeername = unix_op_getpeername,
    .getsockopt  = unix_op_getsockopt,
    .setsockopt  = unix_op_setsockopt,
    .shutdown    = unix_op_shutdown,
    .close       = unix_op_close,
    .read        = unix_op_read,
    .write       = unix_op_write,
    .is_readable = unix_op_is_readable,
};

socket_t *create_unix_socket_obj(unix_handle_t *h) {
    if (!h) return NULL;
    socket_t *s = malloc(sizeof(socket_t));
    if (!s) return NULL;
    memset(s, 0, sizeof(socket_t));
    s->lock = SPINLOCK_INIT;
    s->refcount = 1;
    s->domain = AF_UNIX;
    s->type = h->sock_type;
    s->protocol = 0;
    s->ops = &unix_socket_ops;
    s->priv = h;
    return s;
}
