#pragma once

#include <freestanding/stdint.h>
#include <freestanding/stdbool.h>
#include <freestanding/stddef.h>

// --- IP address helper ---
#define MAKE_IP(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

// QEMU user-mode network defaults
#define NET_GATEWAY_IP  MAKE_IP(10, 0, 2, 2)
#define NET_DNS_IP      MAKE_IP(10, 0, 2, 3)
#define NET_MY_IP       MAKE_IP(10, 0, 2, 15)

// Ethertypes
#define ETHERTYPE_ARP   0x0806
#define ETHERTYPE_IPV4  0x0800

// IP protocols
#define IP_PROTO_ICMP   1
#define IP_PROTO_UDP    17
#define IP_PROTO_TCP    6

// TCP flags
#define TCP_FIN  0x01
#define TCP_SYN  0x02
#define TCP_RST  0x04
#define TCP_PSH  0x08
#define TCP_ACK  0x10
#define TCP_URG  0x20

// --- Packet structs ---

typedef struct {
    uint16_t htype;
    uint16_t ptype;
    uint8_t  hlen;
    uint8_t  plen;
    uint16_t oper;
    uint8_t  sha[6];
    uint32_t spa;
    uint8_t  tha[6];
    uint32_t tpa;
} __attribute__((packed)) arp_packet_t;

typedef struct {
    uint8_t      dst[6];
    uint8_t      src[6];
    uint16_t     ethertype;
    arp_packet_t arp;
} __attribute__((packed)) arp_frame_t;

typedef struct {
    uint8_t  ihl_ver;
    uint8_t  tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t frag_off;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint32_t src;
    uint32_t dst;
} __attribute__((packed)) ipv4_hdr_t;

typedef struct {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
    uint8_t  data[32];
} __attribute__((packed)) icmp_hdr_t;

typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} __attribute__((packed)) udp_hdr_t;

typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  data_off;   // upper 4 bits = header length in 32-bit words
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} __attribute__((packed)) tcp_hdr_t;

typedef struct {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} __attribute__((packed)) dns_hdr_t;

// --- TCP connection states ---
typedef enum {
    TCP_CLOSED = 0,
    TCP_SYN_SENT,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT1,
    TCP_FIN_WAIT2,
    TCP_CLOSE_WAIT,
    TCP_LAST_ACK,
    TCP_TIME_WAIT,
} tcp_state_t;

// --- TCP socket ---
#define TCP_RX_BUF_SIZE (64 * 1024)

typedef struct {
    tcp_state_t state;
    uint32_t    remote_ip;
    uint16_t    local_port;
    uint16_t    remote_port;
    uint32_t    local_seq;   // our sequence number
    uint32_t    remote_seq;  // their sequence number (our ack)
    uint16_t    remote_window;

    // RX ring buffer
    uint8_t     rx_buf[TCP_RX_BUF_SIZE];
    uint32_t    rx_head;
    uint32_t    rx_tail;

    bool        rx_fin;      // remote sent FIN
} tcp_socket_t;

// --- ARP ---
bool resolve_arp(uint32_t ip, uint8_t mac_out[6]);
void handle_arp_rx(const uint8_t *frame, uint16_t len);

// --- ICMP ---
bool ping_icmp(uint32_t dest_ip);
void handle_icmp_rx(const uint8_t *frame, uint16_t len);

// --- UDP ---
bool udp_send(uint32_t dest_ip, uint16_t src_port, uint16_t dst_port,
              const void *data, uint16_t data_len);
void udp_rx(const uint8_t *frame, uint16_t len);

// --- DNS ---
uint32_t dns_resolve(const char *hostname);
void dns_rx(const uint8_t *payload, uint16_t len);

// --- TCP ---
// Connect to remote_ip:remote_port, returns socket or NULL on failure
tcp_socket_t *tcp_connect(uint32_t remote_ip, uint16_t remote_port);

// Send data on an established socket
bool tcp_send(tcp_socket_t *sock, const void *data, uint16_t len);

// Read available data from socket into buf, returns bytes read
int tcp_read(tcp_socket_t *sock, void *buf, int max_len);

// Read until connection closed or buf full, returns total bytes read
int tcp_read_all(tcp_socket_t *sock, void *buf, int max_len, int timeout_ms);

// Close connection (sends FIN)
void tcp_close(tcp_socket_t *sock);

// Free socket resources
void tcp_free(tcp_socket_t *sock);

// Poll for incoming TCP packets (call in wait loops)
void tcp_poll(tcp_socket_t *sock);

// Check if socket is still connected
bool tcp_is_connected(tcp_socket_t *sock);

// Internal RX handler
void tcp_rx(const uint8_t *frame, uint16_t len);

// --- RX dispatch ---
void net_rx(const uint8_t *frame, uint16_t len);

// --- Checksum ---
uint16_t net_checksum(const void *data, size_t len);