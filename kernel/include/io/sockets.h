#pragma once

#include <freestanding/stdint.h>
#include <freestanding/stddef.h>

#include <main/spinlock.h>

#define UNIX_AF_UNIX 1
#define UNIX_AF_LOCAL UNIX_AF_UNIX

#define UNIX_SOCK_STREAM 1
#define UNIX_SOCK_DGRAM 2

#define UNIX_SHUT_RD 0
#define UNIX_SHUT_WR 1
#define UNIX_SHUT_RDWR 2

#define UNIX_BUF_SIZE 4096
#define UNIX_MAX_BINDINGS 32
#define UNIX_MAX_PENDING 16
#define O_NONBLOCK 0x0800

typedef struct unix_channel {
    spinlock_t lock;
    int refs;
    int readers;
    int writers;
    uint8_t buf[UNIX_BUF_SIZE];
    size_t head;
    size_t tail;
    size_t len;
} unix_channel_t;

typedef enum {
    UH_PIPE_READ,
    UH_PIPE_WRITE,
    UH_SOCKET,
} unix_handle_kind_t;

typedef struct unix_handle {
    spinlock_t lock;
    int refs;
    unix_handle_kind_t kind;
    int sock_type;
    int bound;
    int listening;
    int rd_shutdown;
    int wr_shutdown;
    char path[108];
    unix_channel_t *in;
    unix_channel_t *out;
    struct unix_handle *pending[UNIX_MAX_PENDING];
    int pending_head;
    int pending_tail;
    int pending_len;
} unix_handle_t;

typedef struct {
    char path[108];
    unix_handle_t *listener;
} unix_binding_t;

typedef struct {
    uint16_t sa_family;
    char sa_data[14];
} sockaddr_t;

typedef struct {
    uint16_t sun_family;
    char sun_path[108];
} sockaddr_un_t;

int create_unix_pipe(unix_handle_t **read_end, unix_handle_t **write_end);
int create_unix_socket(int domain, int type, int protocol, unix_handle_t **out);
int create_unix_socketpair(int domain, int type, int protocol, unix_handle_t **a, unix_handle_t **b);

void retain_unix_handle(unix_handle_t *h);
void release_unix_handle(unix_handle_t *h);

int64_t read_unix_handle(unix_handle_t *h, void *buf, size_t count, uint32_t fd_flags);
int64_t write_unix_handle(unix_handle_t *h, const void *buf, size_t count, uint32_t fd_flags);

int bind_unix_socket(unix_handle_t *h, const void *addr, uint32_t addrlen);
int listen_unix_socket(unix_handle_t *h, int backlog);
int accept_unix_socket(unix_handle_t *h, unix_handle_t **out);
int connect_unix_socket(unix_handle_t *h, const void *addr, uint32_t addrlen);
int shutdown_unix_socket(unix_handle_t *h, int how);
int get_unix_socket_error(unix_handle_t *h);
int get_unix_socket_type(unix_handle_t *h);