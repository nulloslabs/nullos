#include <io/net.h>
#include <io/rtl8139.h>
#include <main/string.h>
#include <io/terminal.h>
#include <io/hpet.h>
#include <io/io.h>
#include <mm/mm.h>
#include <main/spinlock.h>

// ============================================================
// Byte order
// ============================================================

static inline uint16_t htons(uint16_t x) { return (uint16_t)((x >> 8) | (x << 8)); }
static inline uint16_t ntohs(uint16_t x) { return htons(x); }
static inline uint32_t htonl(uint32_t x) {
    return ((x & 0xFF000000) >> 24) | ((x & 0x00FF0000) >> 8)
         | ((x & 0x0000FF00) << 8)  | ((x & 0x000000FF) << 24);
}
static inline uint32_t ntohl(uint32_t x) { return htonl(x); }

// ============================================================
// Checksum
// ============================================================

uint16_t net_checksum(const void *data, size_t len) {
    const uint16_t *p = (const uint16_t *)data;
    uint32_t sum = 0;
    while (len > 1) { sum += *p++; len -= 2; }
    if (len) sum += *(const uint8_t *)p;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

// Transport pseudo-header checksum (TCP and UDP)
static uint16_t transport_checksum(uint32_t src_ip, uint32_t dst_ip,
                                    uint8_t proto,
                                    const void *hdr, uint16_t total_len) {
    // Allocate pseudo header + transport header + data on stack
    // max segment size is bounded so this is safe
    uint8_t pseudo[12 + total_len];
    memcpy(pseudo + 0, &src_ip, 4);
    memcpy(pseudo + 4, &dst_ip, 4);
    pseudo[8]  = 0;
    pseudo[9]  = proto;
    uint16_t tlen_be = htons(total_len);
    memcpy(pseudo + 10, &tlen_be, 2);
    memcpy(pseudo + 12, hdr, total_len);
    return net_checksum(pseudo, sizeof(pseudo));
}

// ============================================================
// IP send helper (used by TCP and UDP)
// ============================================================

static spinlock_t net_lock = SPINLOCK_INIT;
static uint16_t ip_id_counter = 1;

static bool ip_send(uint32_t dest_ip, uint8_t proto,
                    const void *payload, uint16_t payload_len) {
    uint8_t gw_mac[6];
    if (!resolve_arp(NET_GATEWAY_IP, gw_mac)) return false;

    // Build frame on stack: eth(14) + ip(20) + payload
    uint16_t total = 14 + 20 + payload_len;
    uint8_t frame[total];
    memset(frame, 0, total);

    // Ethernet
    memcpy(frame + 0, gw_mac, 6);
    memcpy(frame + 6, rtl8139.mac, 6);
    uint16_t et = htons(ETHERTYPE_IPV4);
    memcpy(frame + 12, &et, 2);

    // IPv4
    ipv4_hdr_t *ip = (ipv4_hdr_t *)(frame + 14);
    ip->ihl_ver   = 0x45;
    ip->total_len = htons(20 + payload_len);
    ip->id        = htons(ip_id_counter++);
    ip->ttl       = 64;
    ip->protocol  = proto;
    ip->src       = NET_MY_IP;
    ip->dst       = dest_ip;
    ip->checksum  = net_checksum(ip, 20);

    memcpy(frame + 34, payload, payload_len);
    return rtl8139_send(frame, total);
}

// ============================================================
// ARP
// ============================================================

static uint32_t arp_cached_ip  = 0;
static uint8_t  arp_cached_mac[6] = { 0 };
static bool     arp_cache_valid   = false;

static void send_arp_request(uint32_t target_ip) {
    arp_frame_t frame;
    static const uint8_t broadcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    memcpy(frame.dst, broadcast, 6);
    memcpy(frame.src, rtl8139.mac, 6);
    frame.ethertype  = htons(ETHERTYPE_ARP);
    frame.arp.htype  = htons(1);
    frame.arp.ptype  = htons(0x0800);
    frame.arp.hlen   = 6;
    frame.arp.plen   = 4;
    frame.arp.oper   = htons(1);
    memcpy(frame.arp.sha, rtl8139.mac, 6);
    frame.arp.spa    = NET_MY_IP;
    memset(frame.arp.tha, 0, 6);
    frame.arp.tpa    = target_ip;
    rtl8139_send(&frame, sizeof(arp_frame_t));
}

void handle_arp_rx(const uint8_t *frame, uint16_t len) {
    if (len < 14 + (int)sizeof(arp_packet_t)) return;
    if (ntohs(*(const uint16_t *)(frame + 12)) != ETHERTYPE_ARP) return;
    const arp_packet_t *arp = (const arp_packet_t *)(frame + 14);
    if (ntohs(arp->oper) != 2) return;
    
    uint64_t irq;
    spin_lock_irqsave(&net_lock, &irq);
    arp_cached_ip   = arp->spa;
    arp_cache_valid = true;
    memcpy(arp_cached_mac, arp->sha, 6);
    spin_unlock_irqrestore(&net_lock, irq);
}

bool resolve_arp(uint32_t ip, uint8_t mac_out[6]) {
    uint64_t irq;
    spin_lock_irqsave(&net_lock, &irq);
    if (arp_cache_valid && arp_cached_ip == ip) {
        memcpy(mac_out, arp_cached_mac, 6);
        spin_unlock_irqrestore(&net_lock, irq);
        return true;
    }
    arp_cache_valid = false;
    spin_unlock_irqrestore(&net_lock, irq);
    // Continue below unlocked so we don't block

    send_arp_request(ip);
    for (uint32_t i = 0; i < 1000000; i++) {
        rtl8139_poll();
        if (arp_cache_valid && arp_cached_ip == ip) {
            memcpy(mac_out, arp_cached_mac, 6);
            return true;
        }
        io_wait();
        if (i % 100000 == 0) printf(".");
    }
    printf("\nARP: failed to resolve %d.%d.%d.%d\n",
        ip&0xFF,(ip>>8)&0xFF,(ip>>16)&0xFF,(ip>>24)&0xFF);
    return false;
}

// ============================================================
// ICMP
// ============================================================

static volatile bool icmp_got_reply = false;
static uint16_t      icmp_ping_id   = 0x4E4F;
static uint16_t      icmp_ping_seq  = 0;

void handle_icmp_rx(const uint8_t *frame, uint16_t len) {
    if (len < 14 + 20 + (int)sizeof(icmp_hdr_t)) return;
    if (ntohs(*(const uint16_t *)(frame + 12)) != ETHERTYPE_IPV4) return;
    const ipv4_hdr_t *ip = (const ipv4_hdr_t *)(frame + 14);
    if (ip->protocol != IP_PROTO_ICMP) return;
    const icmp_hdr_t *icmp = (const icmp_hdr_t *)(frame + 14 + (ip->ihl_ver & 0xF) * 4);
    if (icmp->type != 0) return;
    if (ntohs(icmp->id) != icmp_ping_id || ntohs(icmp->seq) != icmp_ping_seq) return;
    
    uint64_t irq;
    spin_lock_irqsave(&net_lock, &irq);
    icmp_got_reply = true;
    spin_unlock_irqrestore(&net_lock, irq);
}

bool ping_icmp(uint32_t dest_ip) {
    uint8_t gw_mac[6];
    if (!resolve_arp(NET_GATEWAY_IP, gw_mac)) { printf("ICMP: ARP failed\n"); return false; }

    icmp_ping_seq++;
    icmp_got_reply = false;

    // Build ICMP payload
    uint8_t icmp_buf[sizeof(icmp_hdr_t) + 12];
    memset(icmp_buf, 0, sizeof(icmp_buf));
    icmp_hdr_t *icmp = (icmp_hdr_t *)icmp_buf;
    icmp->type     = 8;
    icmp->id       = htons(icmp_ping_id);
    icmp->seq      = htons(icmp_ping_seq);
    memcpy(icmp_buf + sizeof(icmp_hdr_t), "NullOS ping!", 12);
    icmp->checksum = net_checksum(icmp_buf, sizeof(icmp_buf));

    ip_send(dest_ip, IP_PROTO_ICMP, icmp_buf, sizeof(icmp_buf));

    printf("PING %d.%d.%d.%d seq=%d\n",
        dest_ip&0xFF,(dest_ip>>8)&0xFF,(dest_ip>>16)&0xFF,(dest_ip>>24)&0xFF,icmp_ping_seq);

    for (int i = 0; i < 2000; i++) {
        rtl8139_poll();
        if (icmp_got_reply) {
            printf("PONG from %d.%d.%d.%d seq=%d\n",
                dest_ip&0xFF,(dest_ip>>8)&0xFF,(dest_ip>>16)&0xFF,(dest_ip>>24)&0xFF,icmp_ping_seq);
            return true;
        }
        sleep(1);
    }
    printf("PING timeout\n");
    return false;
}

// ============================================================
// UDP
// ============================================================

typedef void (*udp_rx_callback_t)(uint32_t src_ip, uint16_t src_port,
                                   uint16_t dst_port, const uint8_t *data, uint16_t len);
static udp_rx_callback_t udp_callback = NULL;

bool udp_send(uint32_t dest_ip, uint16_t src_port, uint16_t dst_port,
              const void *data, uint16_t data_len) {
    uint16_t udp_len = sizeof(udp_hdr_t) + data_len;
    uint8_t buf[udp_len];
    memset(buf, 0, udp_len);

    udp_hdr_t *udp = (udp_hdr_t *)buf;
    udp->src_port  = htons(src_port);
    udp->dst_port  = htons(dst_port);
    udp->length    = htons(udp_len);
    memcpy(buf + sizeof(udp_hdr_t), data, data_len);
    udp->checksum  = transport_checksum(NET_MY_IP, dest_ip, IP_PROTO_UDP, buf, udp_len);

    return ip_send(dest_ip, IP_PROTO_UDP, buf, udp_len);
}

void udp_rx(const uint8_t *frame, uint16_t len) {
    if (len < 14 + 20 + (int)sizeof(udp_hdr_t)) return;
    if (ntohs(*(const uint16_t *)(frame + 12)) != ETHERTYPE_IPV4) return;
    const ipv4_hdr_t *ip = (const ipv4_hdr_t *)(frame + 14);
    if (ip->protocol != IP_PROTO_UDP) return;
    const udp_hdr_t *udp = (const udp_hdr_t *)(frame + 14 + (ip->ihl_ver & 0xF) * 4);
    uint16_t data_len = ntohs(udp->length) - sizeof(udp_hdr_t);
    const uint8_t *payload = (const uint8_t *)(udp + 1);
    
    uint64_t irq;
    spin_lock_irqsave(&net_lock, &irq);
    udp_rx_callback_t cb = udp_callback;
    spin_unlock_irqrestore(&net_lock, irq);

    if (cb) cb(ip->src, ntohs(udp->src_port), ntohs(udp->dst_port), payload, data_len);
}

// ============================================================
// DNS
// ============================================================

#define DNS_PORT     53
#define DNS_SRC_PORT 1053
#define DNS_TYPE_A   1
#define DNS_CLASS_IN 1

static volatile uint32_t dns_resolved_ip = 0;
static volatile bool     dns_got_reply   = false;
static uint16_t          dns_query_id    = 0xD175;

static int dns_encode_name(const char *name, uint8_t *out) {
    int total = 0;
    while (*name) {
        const char *dot = strchr(name, '.');
        int label_len   = dot ? (int)(dot - name) : (int)strlen(name);
        out[total++]    = (uint8_t)label_len;
        memcpy(out + total, name, label_len);
        total += label_len;
        name  += label_len;
        if (*name == '.') name++;
    }
    out[total++] = 0;
    return total;
}

void dns_rx(const uint8_t *payload, uint16_t len) {
    if (len < (int)sizeof(dns_hdr_t)) return;
    const dns_hdr_t *hdr = (const dns_hdr_t *)payload;
    if (ntohs(hdr->id) != dns_query_id) return;
    if (ntohs(hdr->ancount) == 0) return;

    const uint8_t *p   = payload + sizeof(dns_hdr_t);
    const uint8_t *end = payload + len;
    uint16_t qdcount   = ntohs(hdr->qdcount);

    // Skip questions
    for (uint16_t q = 0; q < qdcount && p < end; q++) {
        while (p < end) {
            uint8_t n = *p;
            if (n == 0) { p++; break; }
            if ((n & 0xC0) == 0xC0) { p += 2; break; }
            p += n + 1;
        }
        p += 4;  // QTYPE + QCLASS
    }

    // Parse answers
    uint16_t ancount = ntohs(hdr->ancount);
    for (uint16_t a = 0; a < ancount && p < end; a++) {
        // Skip name
        while (p < end) {
            uint8_t n = *p;
            if (n == 0) { p++; break; }
            if ((n & 0xC0) == 0xC0) { p += 2; break; }
            p += n + 1;
        }
        if (p + 10 > end) break;
        uint16_t rtype = ntohs(*(uint16_t *)(p + 0));
        uint16_t rdlen = ntohs(*(uint16_t *)(p + 8));
        p += 10;
        if (rtype == DNS_TYPE_A && rdlen == 4 && p + 4 <= end) {
            uint64_t irq;
            spin_lock_irqsave(&net_lock, &irq);
            memcpy((void *)&dns_resolved_ip, p, 4);
            dns_got_reply = true;
            spin_unlock_irqrestore(&net_lock, irq);
            return;
        }
        p += rdlen;
    }
}

static void dns_udp_rx(uint32_t src_ip, uint16_t src_port, uint16_t dst_port,
                       const uint8_t *data, uint16_t len) {
    (void)src_ip;
    if (dst_port == DNS_SRC_PORT && src_port == DNS_PORT)
        dns_rx(data, len);
}

uint32_t dns_resolve(const char *hostname) {
    uint8_t buf[512];
    memset(buf, 0, sizeof(buf));
    dns_hdr_t *hdr = (dns_hdr_t *)buf;
    hdr->id        = htons(dns_query_id);
    hdr->flags     = htons(0x0100);  // recursion desired
    hdr->qdcount   = htons(1);
    int off = sizeof(dns_hdr_t);
    off    += dns_encode_name(hostname, buf + off);
    buf[off++] = 0x00; buf[off++] = DNS_TYPE_A;
    buf[off++] = 0x00; buf[off++] = DNS_CLASS_IN;

    dns_got_reply   = false;
    dns_resolved_ip = 0;
    udp_callback    = dns_udp_rx;

    printf("DNS: resolving %s...\n", hostname);
    if (!udp_send(NET_DNS_IP, DNS_SRC_PORT, DNS_PORT, buf, (uint16_t)off)) {
        printf("DNS: send failed\n");
        udp_callback = NULL;
        return 0;
    }

    for (int i = 0; i < 3000; i++) {
        rtl8139_poll();
        if (dns_got_reply) {
            uint32_t ip = dns_resolved_ip;
            printf("DNS: %s = %d.%d.%d.%d\n", hostname,
                ip&0xFF,(ip>>8)&0xFF,(ip>>16)&0xFF,(ip>>24)&0xFF);
            udp_callback = NULL;
            return ip;
        }
        sleep(1);
    }
    printf("DNS: timeout\n");
    udp_callback = NULL;
    return 0;
}

// ============================================================
// TCP
// ============================================================

// Max TCP payload per segment (MSS)
#define TCP_MSS       1460
#define TCP_HDR_LEN   20    // no options
#define TCP_WINDOW    8192

// Active sockets table (simple single socket for now, easily expandable)
#define TCP_MAX_SOCKETS 8
static tcp_socket_t *tcp_sockets[TCP_MAX_SOCKETS] = { NULL };

static uint16_t tcp_next_port = 49152;  // ephemeral port range

static uint16_t tcp_alloc_port(void) {
    uint64_t irq;
    spin_lock_irqsave(&net_lock, &irq);
    uint16_t p = tcp_next_port++;
    if (tcp_next_port == 0) tcp_next_port = 49152;
    spin_unlock_irqrestore(&net_lock, irq);
    return p;
}

// Send a raw TCP segment
static bool tcp_send_segment(tcp_socket_t *sock, uint8_t flags,
                              const void *data, uint16_t data_len) {
    uint16_t tcp_len = TCP_HDR_LEN + data_len;
    uint8_t buf[tcp_len];
    memset(buf, 0, tcp_len);

    tcp_hdr_t *tcp    = (tcp_hdr_t *)buf;
    tcp->src_port     = htons(sock->local_port);
    tcp->dst_port     = htons(sock->remote_port);
    tcp->seq          = htonl(sock->local_seq);
    tcp->ack          = (flags & TCP_ACK) ? htonl(sock->remote_seq) : 0;
    tcp->data_off     = (TCP_HDR_LEN / 4) << 4;
    tcp->flags        = flags;
    tcp->window       = htons(TCP_WINDOW);

    if (data && data_len)
        memcpy(buf + TCP_HDR_LEN, data, data_len);

    tcp->checksum = transport_checksum(NET_MY_IP, sock->remote_ip,
                                       IP_PROTO_TCP, buf, tcp_len);
    return ip_send(sock->remote_ip, IP_PROTO_TCP, buf, tcp_len);
}

// Write data into socket RX ring buffer
static void tcp_rx_push(tcp_socket_t *sock, const uint8_t *data, uint16_t len) {
    for (uint16_t i = 0; i < len; i++) {
        uint32_t next = (sock->rx_head + 1) % TCP_RX_BUF_SIZE;
        if (next == sock->rx_tail) break;  // buffer full, drop
        sock->rx_buf[sock->rx_head] = data[i];
        sock->rx_head = next;
    }
}

void tcp_rx(const uint8_t *frame, uint16_t len) {
    if (len < 14 + TCP_HDR_LEN) return;
    if (ntohs(*(const uint16_t *)(frame + 12)) != ETHERTYPE_IPV4) return;

    const ipv4_hdr_t *ip = (const ipv4_hdr_t *)(frame + 14);
    if (ip->protocol != IP_PROTO_TCP) return;

    int ip_hlen = (ip->ihl_ver & 0xF) * 4;
    const tcp_hdr_t *tcp = (const tcp_hdr_t *)(frame + 14 + ip_hlen);
    int tcp_hlen = ((tcp->data_off >> 4) & 0xF) * 4;

    uint16_t ip_total    = ntohs(ip->total_len);
    int      payload_len = ip_total - ip_hlen - tcp_hlen;
    if (payload_len < 0) payload_len = 0;

    const uint8_t *payload = (const uint8_t *)tcp + tcp_hlen;

    uint16_t dst_port = ntohs(tcp->dst_port);
    uint32_t src_ip   = ip->src;

    // Find matching socket
    tcp_socket_t *sock = NULL;
    uint64_t irq;
    spin_lock_irqsave(&net_lock, &irq);
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        if (!tcp_sockets[i]) continue;
        if (tcp_sockets[i]->local_port  == dst_port &&
            tcp_sockets[i]->remote_port == ntohs(tcp->src_port) &&
            tcp_sockets[i]->remote_ip   == src_ip) {
            sock = tcp_sockets[i];
            break;
        }
    }
    spin_unlock_irqrestore(&net_lock, irq);
    if (!sock) return;

    uint32_t seg_seq = ntohl(tcp->seq);
    uint32_t seg_ack = ntohl(tcp->ack);
    uint8_t  flags   = tcp->flags;

    switch (sock->state) {

    case TCP_SYN_SENT:
        // Expect SYN+ACK
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
            sock->remote_seq = seg_seq + 1;
            sock->local_seq  = seg_ack;
            sock->state      = TCP_ESTABLISHED;
            // Send ACK
            tcp_send_segment(sock, TCP_ACK, NULL, 0);
        } else if (flags & TCP_RST) {
            sock->state = TCP_CLOSED;
        }
        break;

    case TCP_ESTABLISHED:
        if (flags & TCP_RST) {
            sock->state = TCP_CLOSED;
            break;
        }
        // Update remote window
        sock->remote_window = ntohs(tcp->window);

        if (payload_len > 0 && seg_seq == sock->remote_seq) {
            tcp_rx_push(sock, payload, (uint16_t)payload_len);
            sock->remote_seq += payload_len;
            // Send ACK
            tcp_send_segment(sock, TCP_ACK, NULL, 0);
        }

        if (flags & TCP_FIN) {
            sock->remote_seq++;
            sock->rx_fin = true;
            sock->state  = TCP_CLOSE_WAIT;
            tcp_send_segment(sock, TCP_ACK, NULL, 0);
        }
        break;

    case TCP_FIN_WAIT1:
        if (flags & TCP_ACK) {
            sock->state = TCP_FIN_WAIT2;
        }
        if (flags & TCP_FIN) {
            sock->remote_seq++;
            sock->rx_fin = true;
            tcp_send_segment(sock, TCP_ACK, NULL, 0);
            sock->state = TCP_TIME_WAIT;
        }
        break;

    case TCP_FIN_WAIT2:
        if (flags & TCP_FIN) {
            sock->remote_seq++;
            sock->rx_fin = true;
            tcp_send_segment(sock, TCP_ACK, NULL, 0);
            sock->state = TCP_TIME_WAIT;
        }
        break;

    case TCP_LAST_ACK:
        if (flags & TCP_ACK) {
            sock->state = TCP_CLOSED;
        }
        break;

    default:
        break;
    }
}

tcp_socket_t *tcp_connect(uint32_t remote_ip, uint16_t remote_port) {
    tcp_socket_t *sock = malloc(sizeof(tcp_socket_t));
    if (!sock) return NULL;
    memset(sock, 0, sizeof(tcp_socket_t));

    sock->remote_ip   = remote_ip;
    sock->remote_port = remote_port;
    sock->local_port  = tcp_alloc_port();
    sock->local_seq   = 0xA1B2C3D4;  // initial sequence number
    sock->state       = TCP_SYN_SENT;
    sock->remote_window = 1024;

    // Register socket
    uint64_t irq;
    spin_lock_irqsave(&net_lock, &irq);
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        if (!tcp_sockets[i]) { tcp_sockets[i] = sock; break; }
    }
    spin_unlock_irqrestore(&net_lock, irq);

    // Send SYN
    tcp_send_segment(sock, TCP_SYN, NULL, 0);
    sock->local_seq++;  // SYN consumes one seq number

    // Wait for SYN+ACK
    for (int i = 0; i < 5000; i++) {
        rtl8139_poll();
        if (sock->state == TCP_ESTABLISHED) return sock;
        if (sock->state == TCP_CLOSED) break;
        sleep(1);
    }

    printf("TCP: connect timeout to %d.%d.%d.%d:%d\n",
        remote_ip&0xFF,(remote_ip>>8)&0xFF,
        (remote_ip>>16)&0xFF,(remote_ip>>24)&0xFF, remote_port);
    tcp_free(sock);
    return NULL;
}

bool tcp_send(tcp_socket_t *sock, const void *data, uint16_t len) {
    if (!sock || sock->state != TCP_ESTABLISHED) return false;

    const uint8_t *ptr = (const uint8_t *)data;
    uint16_t remaining = len;

    while (remaining > 0) {
        uint16_t chunk = remaining < TCP_MSS ? remaining : TCP_MSS;
        if (!tcp_send_segment(sock, TCP_ACK | TCP_PSH, ptr, chunk)) return false;
        sock->local_seq += chunk;
        ptr       += chunk;
        remaining -= chunk;

        // Wait for ACK before sending next chunk
        uint32_t expected_ack = sock->local_seq;
        for (int i = 0; i < 3000; i++) {
            rtl8139_poll();
            // Remote ACK is tracked implicitly via tcp_rx updating local_seq
            // For simplicity, we just wait a bit and continue
            if (sock->state != TCP_ESTABLISHED) return false;
            if (i > 50) break;  // small wait, not full RTT tracking
            sleep(1);
        }
        (void)expected_ack;
    }
    return true;
}

int tcp_read(tcp_socket_t *sock, void *buf, int max_len) {
    if (!sock) return 0;
    uint8_t *out = (uint8_t *)buf;
    int count = 0;
    while (count < max_len) {
        if (sock->rx_tail == sock->rx_head) break;  // empty
        out[count++] = sock->rx_buf[sock->rx_tail];
        sock->rx_tail = (sock->rx_tail + 1) % TCP_RX_BUF_SIZE;
    }
    return count;
}

int tcp_read_all(tcp_socket_t *sock, void *buf, int max_len, int timeout_ms) {
    if (!sock) return 0;
    int total = 0;
    for (int i = 0; i < timeout_ms; i++) {
        rtl8139_poll();
        int n = tcp_read(sock, (uint8_t *)buf + total, max_len - total);
        total += n;
        if (total >= max_len) break;
        if (sock->rx_fin && sock->rx_tail == sock->rx_head) break;
        if (sock->state == TCP_CLOSED) break;
        sleep(1);
    }
    return total;
}

void tcp_close(tcp_socket_t *sock) {
    if (!sock) return;
    if (sock->state == TCP_ESTABLISHED) {
        sock->state = TCP_FIN_WAIT1;
        tcp_send_segment(sock, TCP_FIN | TCP_ACK, NULL, 0);
        sock->local_seq++;
        // Wait for FIN+ACK
        for (int i = 0; i < 3000; i++) {
            rtl8139_poll();
            if (sock->state == TCP_TIME_WAIT || sock->state == TCP_CLOSED) break;
            sleep(1);
        }
    } else if (sock->state == TCP_CLOSE_WAIT) {
        sock->state = TCP_LAST_ACK;
        tcp_send_segment(sock, TCP_FIN | TCP_ACK, NULL, 0);
        sock->local_seq++;
        for (int i = 0; i < 3000; i++) {
            rtl8139_poll();
            if (sock->state == TCP_CLOSED) break;
            sleep(1);
        }
    }
    sock->state = TCP_CLOSED;
}

void tcp_free(tcp_socket_t *sock) {
    if (!sock) return;
    uint64_t irq;
    spin_lock_irqsave(&net_lock, &irq);
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        if (tcp_sockets[i] == sock) { tcp_sockets[i] = NULL; break; }
    }
    spin_unlock_irqrestore(&net_lock, irq);
    free(sock);
}

void tcp_poll(tcp_socket_t *sock) {
    (void)sock;
    rtl8139_poll();
}

bool tcp_is_connected(tcp_socket_t *sock) {
    return sock && sock->state == TCP_ESTABLISHED;
}

// ============================================================
// RX dispatch
// ============================================================

void net_rx(const uint8_t *frame, uint16_t len) {
    handle_arp_rx(frame, len);
    handle_icmp_rx(frame, len);
    udp_rx(frame, len);
    tcp_rx(frame, len);
}