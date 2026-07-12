#pragma once

#include <freestanding/stdint.h>

#define EPOLLIN      0x0001
#define EPOLLPRI     0x0002
#define EPOLLOUT     0x0004
#define EPOLLERR     0x0008
#define EPOLLHUP     0x0010
#define EPOLLRDNORM  0x0040
#define EPOLLRDBAND  0x0080
#define EPOLLWRNORM  0x0100
#define EPOLLWRBAND  0x0200
#define EPOLLMSG     0x0400
#define EPOLLRDHUP   0x2000
#define EPOLLONESHOT (1u << 30)
#define EPOLLET      (1u << 31)

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

#define EPOLL_CLOEXEC 02000000

#define MAX_EPOLL_INTERESTS 64

typedef union epoll_data {
    void    *ptr;
    int      fd;
    uint32_t u32;
    uint64_t u64;
} epoll_data_t;

struct epoll_event {
    uint32_t     events;
    epoll_data_t data;
} __attribute__((packed));

typedef struct {
    int          watched_fd;
    uint32_t     events;
    epoll_data_t data;
    bool         oneshot_reported;
} epoll_interest_t;

typedef struct {
    epoll_interest_t interests[MAX_EPOLL_INTERESTS];
    int count;
} epoll_instance_t;
