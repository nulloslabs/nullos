#pragma once

#include <stdint.h>
#include <sys/socket.h>

#define NETLINK_ROUTE 0

#define NLM_F_REQUEST 0x0001
#define NLM_F_MULTI   0x0002
#define NLM_F_ACK     0x0004
#define NLM_F_ROOT    0x0100
#define NLM_F_MATCH   0x0200
#define NLM_F_DUMP    (NLM_F_ROOT | NLM_F_MATCH)

#define NLMSG_NOOP    0x0001
#define NLMSG_ERROR   0x0002
#define NLMSG_DONE    0x0003
#define NLMSG_OVERRUN 0x0004

#define NLMSG_ALIGNTO 4U
#define NLMSG_ALIGN(length) (((length) + NLMSG_ALIGNTO - 1) & ~(NLMSG_ALIGNTO - 1))
#define NLMSG_LENGTH(length) ((length) + NLMSG_ALIGN(sizeof(struct nlmsghdr)))
#define NLMSG_SPACE(length) NLMSG_ALIGN(NLMSG_LENGTH(length))
#define NLMSG_DATA(header) ((void *)((char *)(header) + NLMSG_LENGTH(0)))

struct sockaddr_nl {
    sa_family_t nl_family;
    unsigned short nl_pad;
    uint32_t nl_pid;
    uint32_t nl_groups;
};

struct nlmsghdr {
    uint32_t nlmsg_len;
    uint16_t nlmsg_type;
    uint16_t nlmsg_flags;
    uint32_t nlmsg_seq;
    uint32_t nlmsg_pid;
};

struct nlmsgerr {
    int error;
    struct nlmsghdr msg;
};
