#pragma once

#define AF_UNSPEC 0
#define AF_UNIX   1
#define AF_LOCAL  AF_UNIX
#define AF_INET   2
#define AF_INET6  10

#define PF_UNSPEC AF_UNSPEC
#define PF_UNIX   AF_UNIX
#define PF_LOCAL  AF_LOCAL
#define PF_INET   AF_INET
#define PF_INET6  AF_INET6

#define SOCK_STREAM    1
#define SOCK_DGRAM     2
#define SOCK_RAW       3
#define SOCK_SEQPACKET 5

#define SOL_SOCKET 1
#define SO_REUSEADDR 2
#define SO_KEEPALIVE 9
#define SO_BROADCAST 6
#define SO_LINGER 13
#define SO_ERROR 4
#define SO_TYPE 3

#define MSG_OOB       0x01
#define MSG_PEEK      0x02
#define MSG_DONTROUTE 0x04
#define MSG_WAITALL   0x100

#define SHUT_RD 0
#define SHUT_WR 1
#define SHUT_RDWR 2

typedef unsigned int socklen_t;
typedef unsigned short sa_family_t;

struct sockaddr {
    sa_family_t sa_family;
    char sa_data[14];
};

struct sockaddr_storage {
    sa_family_t ss_family;
    unsigned long __ss_align;
    char __ss_padding[128 - sizeof(sa_family_t) - sizeof(unsigned long)];
};

struct linger {
    int l_onoff;
    int l_linger;
};
