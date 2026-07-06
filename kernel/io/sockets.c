#include <io/sockets.h>
#include <freestanding/errno.h>
#include <main/spinlocks.h>
#include <main/string.h>
#include <main/sched.h>
#include <mm/mm.h>

static spinlock_t registry_lock = SPINLOCK_INIT;
static unix_binding_t bindings[UNIX_MAX_BINDINGS];

static void yield_cpu(void) {
    current_task_ptr->state = TASK_READY;
    spin_unlock(&sched_lock);
    __asm__ volatile("int $32");
    spin_lock(&sched_lock);
    current_task_ptr->state = TASK_RUNNING;
}

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
    h->sock_type = UNIX_SOCK_STREAM;
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
    if (domain != UNIX_AF_UNIX && domain != UNIX_AF_LOCAL) return -EAFNOSUPPORT;
    if (type != UNIX_SOCK_STREAM) return -ESOCKTNOSUPPORT;
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
    if (domain != UNIX_AF_UNIX && domain != UNIX_AF_LOCAL) return -EAFNOSUPPORT;
    if (type != UNIX_SOCK_STREAM) return -ESOCKTNOSUPPORT;
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
        yield_cpu();
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
        yield_cpu();
    }
    return (int64_t)done;
}

static int sockaddr_path(const void *addr, uint32_t addrlen, char *out, size_t out_size) {
    const sockaddr_un_t *un = (const sockaddr_un_t *)addr;
    size_t max;
    if (!addr || addrlen < sizeof(uint16_t) + 1) return -EINVAL;
    if (un->sun_family != UNIX_AF_UNIX && un->sun_family != UNIX_AF_LOCAL) return -EAFNOSUPPORT;
    max = addrlen - sizeof(uint16_t);
    if (max >= out_size) max = out_size - 1;
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
    uint64_t flags;
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

    spin_lock_irqsave(&h->lock, &flags);
    h->in = client->in;
    h->out = client->out;
    retain_unix_channel(h->in);
    retain_unix_channel(h->out);
    add_unix_channel_reader(h->in);
    add_unix_channel_writer(h->out);
    spin_unlock_irqrestore(&h->lock, flags);
    release_unix_handle(client);

    spin_lock_irqsave(&listener->lock, &flags);
    if (listener->pending_len >= UNIX_MAX_PENDING) {
        spin_unlock_irqrestore(&listener->lock, flags);
        release_unix_handle(server);
        release_unix_handle(listener);
        return -ECONNREFUSED;
    }
    listener->pending[listener->pending_tail] = server;
    listener->pending_tail = (listener->pending_tail + 1) % UNIX_MAX_PENDING;
    listener->pending_len++;
    spin_unlock_irqrestore(&listener->lock, flags);
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
        yield_cpu();
    }
}

int shutdown_unix_socket(unix_handle_t *h, int how) {
    if (!h || h->kind != UH_SOCKET) return -ENOTSOCK;
    if (how == UNIX_SHUT_RD || how == UNIX_SHUT_RDWR) {
        if (!h->rd_shutdown && h->in) drop_unix_channel_reader(h->in);
        h->rd_shutdown = 1;
    }
    if (how == UNIX_SHUT_WR || how == UNIX_SHUT_RDWR) {
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
