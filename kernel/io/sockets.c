#include <errno.h>
#include <main/string.h>
#include <main/spinlocks.h>
#include <io/sockets.h>
#include <io/unix_sockets.h>
#include <io/net_sockets.h>
#include <io/packet_sockets.h>
#include <mm/mm.h>

int create_socket(int domain, int type, int protocol, socket_t **out) {
    if (!out) return -EINVAL;

    if (domain == AF_UNIX || domain == AF_LOCAL) {
        unix_handle_t *uh = NULL;
        int r = create_unix_socket(domain, type, protocol, &uh);
        if (r < 0) return r;
        socket_t *s = create_unix_socket_obj(uh);
        if (!s) {
            release_unix_handle(uh);
            return -ENOMEM;
        }
        *out = s;
        return 0;
    } else if (domain == AF_INET) {
        return create_inet_socket_obj(type, protocol, out);
    } else if (domain == AF_PACKET) {
        return create_packet_socket_obj(type, protocol, out);
    }

    return -EAFNOSUPPORT;
}

int create_socketpair(int domain, int type, int protocol, socket_t **a, socket_t **b) {
    if (!a || !b) return -EINVAL;

    if (domain == AF_UNIX || domain == AF_LOCAL) {
        unix_handle_t *ha = NULL, *hb = NULL;
        int r = create_unix_socketpair(domain, type, protocol, &ha, &hb);
        if (r < 0) return r;
        socket_t *sa = create_unix_socket_obj(ha);
        socket_t *sb = create_unix_socket_obj(hb);
        if (!sa || !sb) {
            if (sa) release_socket(sa); else if (ha) release_unix_handle(ha);
            if (sb) release_socket(sb); else if (hb) release_unix_handle(hb);
            return -ENOMEM;
        }
        *a = sa;
        *b = sb;
        return 0;
    }

    return -EOPNOTSUPP;
}

void retain_socket(socket_t *s) {
    if (!s) return;
    uint64_t flags;
    spin_lock_irqsave(&s->lock, &flags);
    s->refcount++;
    spin_unlock_irqrestore(&s->lock, flags);
}

void release_socket(socket_t *s) {
    if (!s) return;
    uint64_t flags;
    int free_it = 0;

    spin_lock_irqsave(&s->lock, &flags);
    s->refcount--;
    if (s->refcount <= 0) free_it = 1;
    spin_unlock_irqrestore(&s->lock, flags);

    if (free_it) {
        if (s->ops && s->ops->close) {
            s->ops->close(s);
        }
        free(s);
    }
}
