#pragma once

#include <stddef.h>
#include <stdint.h>
#include <main/spinlocks.h>
#include <io/sockets.h>

#define NETLINK_RESPONSE_SIZE 1024
#define NETLINK_QUEUE_SIZE 8

typedef struct {
    uint8_t data[NETLINK_RESPONSE_SIZE];
    size_t length;
    size_t offset;
} netlink_message_t;

typedef struct {
    spinlock_t lock;
    uint32_t port_id;
    uint32_t groups;
    netlink_message_t queue[NETLINK_QUEUE_SIZE];
    uint32_t queue_head;
    uint32_t queue_tail;
    bool dump_complete;
    netlink_message_t *building;
} netlink_socket_t;

int create_netlink_socket(int type, int protocol, socket_t **out);
