#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// --- IP address helper ---
#define MAKE_IP(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

extern uint32_t net_local_ip;
extern uint32_t net_gateway_ip;
extern uint32_t net_dns_ip;
extern uint32_t net_subnet_mask;

// Ethertypes
#define ETHERTYPE_ARP   0x0806
#define ETHERTYPE_IPV4  0x0800

// IP protocols
#define IP_PROTO_ICMP   1
#define IP_PROTO_UDP    17
#define IP_PROTO_TCP    6

#define NET_MAX_FRAME_SIZE 1514

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
#define TCP_RX_BUF_SIZE (16 * 1024)

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

// --- Network Device Interface ---
typedef struct net_device {
    uint8_t mac[6];
    bool (*send)(const void *data, uint16_t len);
    void (*poll)(void);
} net_device_t;

void register_net_device(net_device_t *dev);
extern net_device_t *net_current_device;
void poll_net_device(void);

// --- ARP ---
bool resolve_arp(uint32_t ip, uint8_t mac_out[6]);
void handle_arp_packet(const uint8_t *frame, uint16_t len);

// --- ICMP ---
bool ping_icmp(uint32_t dest_ip);
void handle_icmp_packet(const uint8_t *frame, uint16_t len);

// --- UDP ---
bool send_udp_packet(uint32_t dest_ip, uint16_t src_port, uint16_t dst_port,
                     const void *data, uint16_t data_len);
void handle_udp_packet(const uint8_t *frame, uint16_t len);

// --- DNS ---
uint32_t resolve_dns(const char *hostname);
void handle_dns_response(const uint8_t *payload, uint16_t len);

// --- TCP ---
// Connect to remote_ip:remote_port, returns socket or NULL on failure
tcp_socket_t *connect_tcp(uint32_t remote_ip, uint16_t remote_port);

// Send data on an established socket
bool send_tcp(tcp_socket_t *sock, const void *data, uint16_t len);

// Read available data from socket into buf, returns bytes read
int read_tcp(tcp_socket_t *sock, void *buf, int max_len);

// Read until connection closed or buf full, returns total bytes read
int read_all_tcp(tcp_socket_t *sock, void *buf, int max_len, int timeout_ms);

// Close connection (sends FIN)
void close_tcp(tcp_socket_t *sock);

// Free socket resources
void free_tcp(tcp_socket_t *sock);

// Poll for incoming TCP packets (call in wait loops)
void poll_tcp(tcp_socket_t *sock);

// Check if socket is still connected
bool check_tcp_connected(tcp_socket_t *sock);

// Internal RX handler
void handle_tcp_packet(const uint8_t *frame, uint16_t len);

// --- RX dispatch ---
void handle_net_packet(net_device_t *dev, const uint8_t *frame, uint16_t len);

// --- Checksum ---
uint16_t calculate_net_checksum(const void *data, size_t len);
