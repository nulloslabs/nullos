#include <stdbool.h>
#include <errno.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <main/sched.h>
#include <main/string.h>
#include <io/net.h>
#include <io/netlink_sockets.h>
#include <mm/mm.h>

static bool append_netlink_data(netlink_message_t *message, const void *data, size_t length) {
    if (length > NETLINK_RESPONSE_SIZE - message->length) return false;
    memcpy(message->data + message->length, data, length);
    message->length += length;
    return true;
}

static bool append_netlink_attribute(netlink_message_t *message, uint16_t type, const void *data, uint16_t length) {
    struct rtattr attribute = { .rta_len = (uint16_t)RTA_LENGTH(length), .rta_type = type };
    size_t aligned_length = RTA_ALIGN(attribute.rta_len);
    if (aligned_length > NETLINK_RESPONSE_SIZE - message->length) return false;
    append_netlink_data(message, &attribute, sizeof(attribute));
    append_netlink_data(message, data, length);
    size_t padding = aligned_length - attribute.rta_len;
    if (padding) {
        uint32_t zero = 0;
        append_netlink_data(message, &zero, padding);
    }
    return true;
}

static bool append_netlink_link(netlink_message_t *message, uint32_t port_id, uint32_t sequence, int index, const char *name, uint16_t hardware_type, uint32_t flags, uint32_t mtu, const uint8_t address[6]) {
    size_t message_offset = message->length;
    struct nlmsghdr header = { .nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg)), .nlmsg_type = RTM_NEWLINK, .nlmsg_flags = NLM_F_MULTI, .nlmsg_seq = sequence, .nlmsg_pid = port_id };
    struct ifinfomsg info = { .ifi_family = AF_UNSPEC, .ifi_type = hardware_type, .ifi_index = index, .ifi_flags = flags, .ifi_change = 0xFFFFFFFFU };
    if (!append_netlink_data(message, &header, sizeof(header))) return false;
    if (!append_netlink_data(message, &info, sizeof(info))) return false;
    if (!append_netlink_attribute(message, IFLA_IFNAME, name, (uint16_t)(strlen(name) + 1))) return false;
    if (!append_netlink_attribute(message, IFLA_MTU, &mtu, sizeof(mtu))) return false;
    if (!append_netlink_attribute(message, IFLA_ADDRESS, address, 6)) return false;
    uint8_t broadcast[6];
    memset(broadcast, index == 1 ? 0 : 0xFF, sizeof(broadcast));
    if (!append_netlink_attribute(message, IFLA_BROADCAST, broadcast, sizeof(broadcast))) return false;
    uint8_t state = flags & IFF_RUNNING ? IF_OPER_UP : IF_OPER_DOWN;
    uint8_t link_mode = 0;
    if (!append_netlink_attribute(message, IFLA_OPERSTATE, &state, sizeof(state))) return false;
    if (!append_netlink_attribute(message, IFLA_LINKMODE, &link_mode, sizeof(link_mode))) return false;
    size_t message_length = message->length - message_offset;
    struct nlmsghdr *stored_header = (struct nlmsghdr *)(message->data + message_offset);
    stored_header->nlmsg_len = (uint32_t)message_length;
    size_t aligned_length = NLMSG_ALIGN(message_length);
    if (aligned_length > message_length) {
        uint32_t zero = 0;
        if (!append_netlink_data(message, &zero, aligned_length - message_length)) return false;
    }
    return true;
}

static bool append_netlink_done(netlink_message_t *message, uint32_t port_id, uint32_t sequence) {
    struct nlmsghdr header = { .nlmsg_len = NLMSG_LENGTH(0), .nlmsg_type = NLMSG_DONE, .nlmsg_flags = 0, .nlmsg_seq = sequence, .nlmsg_pid = port_id };
    return append_netlink_data(message, &header, sizeof(header));
}

static bool append_netlink_error(netlink_message_t *message, uint32_t port_id, const struct nlmsghdr *request, int error) {
    struct nlmsghdr header = { .nlmsg_len = NLMSG_LENGTH(sizeof(struct nlmsgerr)), .nlmsg_type = NLMSG_ERROR, .nlmsg_flags = 0, .nlmsg_seq = request->nlmsg_seq, .nlmsg_pid = port_id };
    struct nlmsgerr error_message = { .error = error, .msg = *request };
    return append_netlink_data(message, &header, sizeof(header)) && append_netlink_data(message, &error_message, sizeof(error_message));
}

static int bind_netlink_socket(socket_t *socket, const void *address, socklen_t address_length) {
    if (!socket || !address || address_length < sizeof(struct sockaddr_nl)) return -EINVAL;
    const struct sockaddr_nl *netlink_address = address;
    if (netlink_address->nl_family != AF_NETLINK) return -EAFNOSUPPORT;
    netlink_socket_t *netlink = socket->priv;
    if (!netlink) return -EINVAL;
    netlink->port_id = netlink_address->nl_pid;
    if (!netlink->port_id && current_task_ptr) netlink->port_id = (uint32_t)current_task_ptr->pid;
    netlink->groups = netlink_address->nl_groups;
    return 0;
}

static int connect_netlink_socket(socket_t *socket, const void *address, socklen_t address_length) {
    (void)socket;
    if (!address || address_length < sizeof(struct sockaddr_nl)) return -EINVAL;
    const struct sockaddr_nl *netlink_address = address;
    if (netlink_address->nl_family != AF_NETLINK || netlink_address->nl_pid != 0) return -EINVAL;
    return 0;
}

static int listen_netlink_socket(socket_t *socket, int backlog) {
    (void)socket;
    (void)backlog;
    return -EOPNOTSUPP;
}

static int accept_netlink_socket(socket_t *socket, socket_t **accepted) {
    (void)socket;
    (void)accepted;
    return -EOPNOTSUPP;
}

static int64_t send_netlink_message(socket_t *socket, const void *buffer, size_t length, int flags, const void *destination, socklen_t address_length) {
    (void)flags;
    if (!socket || !buffer || length < sizeof(struct nlmsghdr)) return -EINVAL;
    if (destination) {
        if (address_length < sizeof(struct sockaddr_nl)) return -EINVAL;
        const struct sockaddr_nl *netlink_address = destination;
        if (netlink_address->nl_family != AF_NETLINK || netlink_address->nl_pid != 0) return -EINVAL;
    }
    netlink_socket_t *netlink = socket->priv;
    if (!netlink) return -EINVAL;
    const struct nlmsghdr *request = buffer;
    if (request->nlmsg_len < sizeof(*request) || request->nlmsg_len > length) return -EINVAL;
    uint64_t lock_flags;
    spin_lock_irqsave(&netlink->lock, &lock_flags);
    uint32_t next_head = (netlink->queue_head + 1) % NETLINK_QUEUE_SIZE;
    if (next_head == netlink->queue_tail) {
        spin_unlock_irqrestore(&netlink->lock, lock_flags);
        return -ENOBUFS;
    }
    netlink_message_t *message = &netlink->queue[netlink->queue_head];
    message->length = 0;
    message->offset = 0;
    netlink->dump_complete = false;
    netlink->building = message;
    if (!(request->nlmsg_flags & NLM_F_REQUEST)) {
        append_netlink_error(message, netlink->port_id, request, -EINVAL);
    } else if (request->nlmsg_type != RTM_GETLINK) {
        append_netlink_error(message, netlink->port_id, request, -EOPNOTSUPP);
    } else {
        const struct ifinfomsg *requested_link = NULL;
        if (request->nlmsg_len >= NLMSG_LENGTH(sizeof(struct ifinfomsg))) requested_link = NLMSG_DATA(request);
        int requested_index = requested_link ? requested_link->ifi_index : 0;
        uint8_t loopback_address[6] = {0};
        if (!requested_index || requested_index == NET_LOOPBACK_INTERFACE_INDEX) append_netlink_link(message, netlink->port_id, request->nlmsg_seq, NET_LOOPBACK_INTERFACE_INDEX, "lo", 772, IFF_UP | IFF_LOOPBACK | IFF_RUNNING, 65536, loopback_address);
        if (net_current_device && (!requested_index || requested_index == NET_ETHERNET_INTERFACE_INDEX)) append_netlink_link(message, netlink->port_id, request->nlmsg_seq, NET_ETHERNET_INTERFACE_INDEX, "eth0", ARPHRD_ETHER, IFF_UP | IFF_BROADCAST | IFF_RUNNING | IFF_MULTICAST, 1500, net_current_device->mac);
        if (requested_index && requested_index != NET_LOOPBACK_INTERFACE_INDEX && (requested_index != NET_ETHERNET_INTERFACE_INDEX || !net_current_device)) {
            message->length = 0;
            append_netlink_error(message, netlink->port_id, request, -ENODEV);
        } else {
            append_netlink_done(message, netlink->port_id, request->nlmsg_seq);
        }
    }
    netlink->building = NULL;
    netlink->queue_head = next_head;
    spin_unlock_irqrestore(&netlink->lock, lock_flags);
    return (int64_t)length;
}

static int64_t receive_netlink_message(socket_t *socket, void *buffer, size_t length, int flags, void *source, socklen_t *address_length) {
    if (!socket || !buffer) return -EINVAL;
    netlink_socket_t *netlink = socket->priv;
    if (!netlink) return -EINVAL;
    uint64_t lock_flags;
    for (;;) {
        spin_lock_irqsave(&netlink->lock, &lock_flags);
        if (netlink->queue_tail != netlink->queue_head) break;
        spin_unlock_irqrestore(&netlink->lock, lock_flags);
        if ((flags & MSG_DONTWAIT) || (socket->flags & SOCK_NONBLOCK)) return -EAGAIN;
        if (netlink->dump_complete) return 0;
        return 0;
    }
    netlink_message_t *message = &netlink->queue[netlink->queue_tail];
    size_t remaining = message->length - message->offset;
    size_t datagram_length = remaining;
    if (remaining >= sizeof(struct nlmsghdr)) {
        struct nlmsghdr *header = (struct nlmsghdr *)(message->data + message->offset);
        if (header->nlmsg_len >= sizeof(*header) && header->nlmsg_len <= remaining) datagram_length = NLMSG_ALIGN(header->nlmsg_len);
        if (header->nlmsg_type == NLMSG_DONE) netlink->dump_complete = true;
    }
    size_t copied = length < datagram_length ? length : datagram_length;
    memcpy(buffer, message->data + message->offset, copied);
    if (!(flags & MSG_PEEK)) {
        message->offset += copied;
        if (message->offset == message->length) {
            message->length = 0;
            message->offset = 0;
            netlink->queue_tail = (netlink->queue_tail + 1) % NETLINK_QUEUE_SIZE;
        }
    }
    spin_unlock_irqrestore(&netlink->lock, lock_flags);
    if (source && address_length) {
        struct sockaddr_nl kernel_address = { .nl_family = AF_NETLINK, .nl_pid = 0, .nl_groups = 0 };
        socklen_t copied_address = *address_length < sizeof(kernel_address) ? *address_length : sizeof(kernel_address);
        memcpy(source, &kernel_address, copied_address);
        *address_length = sizeof(kernel_address);
    }
    return (int64_t)copied;
}

static int get_netlink_socket_name(socket_t *socket, void *address, socklen_t *address_length) {
    if (!socket || !address || !address_length) return -EINVAL;
    netlink_socket_t *netlink = socket->priv;
    if (!netlink) return -EINVAL;
    struct sockaddr_nl local_address = { .nl_family = AF_NETLINK, .nl_pid = netlink->port_id, .nl_groups = netlink->groups };
    socklen_t copied = *address_length < sizeof(local_address) ? *address_length : sizeof(local_address);
    memcpy(address, &local_address, copied);
    *address_length = sizeof(local_address);
    return 0;
}

static int get_netlink_peer_name(socket_t *socket, void *address, socklen_t *address_length) {
    (void)socket;
    (void)address;
    (void)address_length;
    return -ENOTCONN;
}

static int get_netlink_socket_option(socket_t *socket, int level, int option, void *value, socklen_t *length) {
    (void)socket;
    (void)level;
    (void)option;
    (void)value;
    (void)length;
    return -ENOPROTOOPT;
}

static int set_netlink_socket_option(socket_t *socket, int level, int option, const void *value, socklen_t length) {
    (void)socket;
    (void)level;
    (void)option;
    (void)value;
    (void)length;
    return -ENOPROTOOPT;
}

static int shutdown_netlink_socket(socket_t *socket, int how) {
    (void)socket;
    (void)how;
    return -EOPNOTSUPP;
}

static void close_netlink_socket(socket_t *socket) {
    if (!socket || !socket->priv) return;
    free(socket->priv);
    socket->priv = NULL;
}

static int64_t read_netlink_socket(socket_t *socket, void *buffer, size_t count, uint32_t flags) {
    (void)flags;
    return receive_netlink_message(socket, buffer, count, 0, NULL, NULL);
}

static int64_t write_netlink_socket(socket_t *socket, const void *buffer, size_t count, uint32_t flags) {
    (void)flags;
    return send_netlink_message(socket, buffer, count, 0, NULL, 0);
}

static bool check_netlink_socket_readable(socket_t *socket) {
    if (!socket || !socket->priv) return false;
    netlink_socket_t *netlink = socket->priv;
    uint64_t lock_flags;
    spin_lock_irqsave(&netlink->lock, &lock_flags);
    bool readable = netlink->queue_tail != netlink->queue_head;
    spin_unlock_irqrestore(&netlink->lock, lock_flags);
    return readable;
}

static const socket_ops_t netlink_socket_ops = {
    .bind = bind_netlink_socket, .connect = connect_netlink_socket, .listen = listen_netlink_socket, .accept = accept_netlink_socket, .sendto = send_netlink_message, .recvfrom = receive_netlink_message, .getsockname = get_netlink_socket_name, .getpeername = get_netlink_peer_name, .getsockopt = get_netlink_socket_option, .setsockopt = set_netlink_socket_option, .shutdown = shutdown_netlink_socket, .close = close_netlink_socket, .read = read_netlink_socket, .write = write_netlink_socket, .is_readable = check_netlink_socket_readable,
};

int create_netlink_socket(int type, int protocol, socket_t **out) {
    if (!out) return -EINVAL;
    int socket_type = type & SOCK_TYPE_MASK;
    if (socket_type != SOCK_RAW && socket_type != SOCK_DGRAM) return -ESOCKTNOSUPPORT;
    if (protocol != NETLINK_ROUTE) return -EPROTONOSUPPORT;
    netlink_socket_t *netlink = malloc(sizeof(*netlink));
    if (!netlink) return -ENOMEM;
    memset(netlink, 0, sizeof(*netlink));
    netlink->lock = SPINLOCK_INIT;
    socket_t *socket = malloc(sizeof(*socket));
    if (!socket) {
        free(netlink);
        return -ENOMEM;
    }
    memset(socket, 0, sizeof(*socket));
    socket->lock = SPINLOCK_INIT;
    socket->refcount = 1;
    socket->domain = AF_NETLINK;
    socket->type = socket_type;
    socket->protocol = protocol;
    socket->ops = &netlink_socket_ops;
    socket->priv = netlink;
    *out = socket;
    return 0;
}
