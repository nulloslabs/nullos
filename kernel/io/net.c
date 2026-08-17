#include <stdbool.h>
#include <netinet/in.h>
#include <main/string.h>
#include <main/spinlocks.h>
#include <io/dhcp.h>
#include <io/net.h>
#include <io/sockets.h>
#include <io/net_sockets.h>
#include <io/packet_sockets.h>
#include <io/io.h>
#include <io/time.h>
#include <mm/mm.h>

uint16_t calculate_net_checksum(const void *data, size_t len) {
    const uint16_t *p = (const uint16_t *)data;
    uint32_t sum = 0;
    while (len > 1) { sum += *p++; len -= 2; }
    if (len) sum += *(const uint8_t *)p;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

// Transport pseudo-header checksum (TCP and UDP)
// Uses heap allocation to avoid VLA stack overflow
static uint16_t calculate_transport_checksum(uint32_t src_ip, uint32_t dst_ip, uint8_t proto, const void *hdr, uint16_t total_len) {
    size_t buf_size = 12 + total_len;
    uint8_t *pseudo = (uint8_t *)malloc(buf_size);
    if (!pseudo) return 0; // OOM: return 0 checksum (best effort)
    memcpy(pseudo + 0, &src_ip, 4);
    memcpy(pseudo + 4, &dst_ip, 4);
    pseudo[8]  = 0;
    pseudo[9]  = proto;
    uint16_t tlen_be = htons(total_len);
    memcpy(pseudo + 10, &tlen_be, 2);
    memcpy(pseudo + 12, hdr, total_len);
    uint16_t cksum = calculate_net_checksum(pseudo, buf_size);
    free(pseudo);
    return cksum;
}


static spinlock_t net_lock = SPINLOCK_INIT;
static uint16_t ip_id_counter = 1;

net_device_t *net_current_device = NULL;
uint32_t net_local_ip = 0;
uint32_t net_gateway_ip = 0;
uint32_t net_dns_ip = 0;
uint32_t net_subnet_mask = 0;

static bool parse_ipv4_packet(const uint8_t *frame, uint16_t len, uint8_t protocol, const ipv4_hdr_t **ip_out, uint16_t *header_len_out, uint16_t *total_len_out) {
    if (!frame || len < 14 + sizeof(ipv4_hdr_t)) return false;
    if (ntohs(*(const uint16_t *)(frame + 12)) != ETHERTYPE_IPV4) return false;
    const ipv4_hdr_t *ip = (const ipv4_hdr_t *)(frame + 14);
    if ((ip->ihl_ver >> 4) != 4 || ip->protocol != protocol) return false;
    uint16_t header_len = (uint16_t)(ip->ihl_ver & 0x0F) * 4;
    uint16_t total_len = ntohs(ip->total_len);
    if (header_len < sizeof(ipv4_hdr_t) || total_len < header_len || total_len > len - 14) return false;
    if (ntohs(ip->frag_off) & 0x3FFF) return false;
    *ip_out = ip;
    *header_len_out = header_len;
    *total_len_out = total_len;
    return true;
}

void register_net_device(net_device_t *dev) {
    if (!dev || !dev->send) return;
    uint64_t irq;
    spin_lock_irqsave(&net_lock, &irq);
    if (!net_current_device) { net_current_device = dev; }
    spin_unlock_irqrestore(&net_lock, irq);
}

void poll_net_device(void) {
    net_device_t *dev;
    uint64_t irq;
    spin_lock_irqsave(&net_lock, &irq);
    dev = net_current_device;
    spin_unlock_irqrestore(&net_lock, irq);
    if (dev && dev->poll) dev->poll();
}

static bool send_ip_packet(uint32_t dest_ip, uint8_t proto, const void *payload, uint16_t payload_len) {
    if ((!payload && payload_len) || payload_len > NET_MAX_FRAME_SIZE - 14 - 20) return false;
    if (!net_current_device) return false;

    uint8_t dest_mac[6];
    if (dest_ip == UINT32_MAX) {
        memset(dest_mac, 0xFF, sizeof(dest_mac));
    } else {
        if (!net_local_ip || !net_subnet_mask) return false;
        uint32_t next_hop = (dest_ip & net_subnet_mask) == (net_local_ip & net_subnet_mask) ? dest_ip : net_gateway_ip;
        if (!next_hop || !resolve_arp(next_hop, dest_mac)) return false;
    }

    // Build frame on stack: eth(14) + ip(20) + payload
    uint16_t total = 14 + 20 + payload_len;
    uint8_t frame[total];
    memset(frame, 0, total);

    // Ethernet
    memcpy(frame + 0, dest_mac, 6);
    memcpy(frame + 6, net_current_device->mac, 6);
    uint16_t et = htons(ETHERTYPE_IPV4);
    memcpy(frame + 12, &et, 2);

    // IPv4
    ipv4_hdr_t *ip = (ipv4_hdr_t *)(frame + 14);
    ip->ihl_ver   = 0x45;
    ip->total_len = htons(20 + payload_len);
    ip->id        = htons(ip_id_counter++);
    ip->ttl       = 64;
    ip->protocol  = proto;
    ip->src       = net_local_ip;
    ip->dst       = dest_ip;
    ip->checksum  = calculate_net_checksum(ip, 20);

    memcpy(frame + 34, payload, payload_len);
    return net_current_device->send(frame, total);
}


static uint32_t arp_cached_ip  = 0;
static uint8_t  arp_cached_mac[6] = { 0 };
static bool     arp_cache_valid   = false;

static void send_arp_request(uint32_t target_ip) {
    if (!net_current_device) return;
    arp_frame_t frame;
    static const uint8_t broadcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    memcpy(frame.dst, broadcast, 6);
    memcpy(frame.src, net_current_device->mac, 6);
    frame.ethertype  = htons(ETHERTYPE_ARP);
    frame.arp.htype  = htons(1);
    frame.arp.ptype  = htons(0x0800);
    frame.arp.hlen   = 6;
    frame.arp.plen   = 4;
    frame.arp.oper   = htons(1);
    memcpy(frame.arp.sha, net_current_device->mac, 6);
    frame.arp.spa    = net_local_ip;
    memset(frame.arp.tha, 0, 6);
    frame.arp.tpa    = target_ip;
    net_current_device->send(&frame, sizeof(arp_frame_t));
}

void handle_arp_packet(const uint8_t *frame, uint16_t len) {
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
        poll_net_device();
        if (arp_cache_valid && arp_cached_ip == ip) {
            memcpy(mac_out, arp_cached_mac, 6);
            return true;
        }
        io_wait();
    }
    return false;
}


static volatile bool icmp_got_reply = false;
static uint16_t      icmp_ping_id   = 0x4E4F;
static uint16_t      icmp_ping_seq  = 0;

void handle_icmp_packet(const uint8_t *frame, uint16_t len) {
    const ipv4_hdr_t *ip;
    uint16_t ip_hlen;
    uint16_t ip_total;
    if (!parse_ipv4_packet(frame, len, IP_PROTO_ICMP, &ip, &ip_hlen, &ip_total)) return;
    uint16_t ip_payload_len = ip_total - ip_hlen;
    if (ip_payload_len < (uint16_t)sizeof(icmp_hdr_t)) return;
    const icmp_hdr_t *icmp = (const icmp_hdr_t *)(frame + 14 + ip_hlen);
    if (icmp->type != 0) return;
    if (ntohs(icmp->id) != icmp_ping_id || ntohs(icmp->seq) != icmp_ping_seq) return;
    
    uint64_t irq;
    spin_lock_irqsave(&net_lock, &irq);
    icmp_got_reply = true;
    spin_unlock_irqrestore(&net_lock, irq);
}

bool ping_icmp(uint32_t dest_ip) {
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
    icmp->checksum = calculate_net_checksum(icmp_buf, sizeof(icmp_buf));

    send_ip_packet(dest_ip, IP_PROTO_ICMP, icmp_buf, sizeof(icmp_buf));

    for (int i = 0; i < 2000; i++) {
        if (icmp_got_reply) {
            return true;
        }
        sleep(1);
    }
    return false;
}


typedef void (*udp_rx_callback_t)(uint32_t src_ip, uint16_t src_port, uint16_t dst_port, const uint8_t *data, uint16_t len);
static udp_rx_callback_t udp_callback = NULL;

bool send_udp_packet(uint32_t dest_ip, uint16_t src_port, uint16_t dst_port, const void *data, uint16_t data_len) {
    if ((!data && data_len) || data_len > NET_MAX_FRAME_SIZE - 14 - 20 - sizeof(udp_hdr_t)) return false;
    uint16_t udp_len = sizeof(udp_hdr_t) + data_len;
    uint8_t buf[udp_len];
    memset(buf, 0, udp_len);

    udp_hdr_t *udp = (udp_hdr_t *)buf;
    udp->src_port  = htons(src_port);
    udp->dst_port  = htons(dst_port);
    udp->length    = htons(udp_len);
    memcpy(buf + sizeof(udp_hdr_t), data, data_len);
    udp->checksum  = calculate_transport_checksum(net_local_ip, dest_ip, IP_PROTO_UDP, buf, udp_len);

    return send_ip_packet(dest_ip, IP_PROTO_UDP, buf, udp_len);
}

void handle_udp_packet(const uint8_t *frame, uint16_t len) {
    const ipv4_hdr_t *ip;
    uint16_t ip_hlen;
    uint16_t ip_total;
    if (!parse_ipv4_packet(frame, len, IP_PROTO_UDP, &ip, &ip_hlen, &ip_total)) return;
    uint16_t ip_payload_len = ip_total - ip_hlen;
    if (ip_payload_len < (uint16_t)sizeof(udp_hdr_t)) return;
    const udp_hdr_t *udp = (const udp_hdr_t *)(frame + 14 + ip_hlen);
    uint16_t udp_total = ntohs(udp->length);
    if (udp_total < (uint16_t)sizeof(udp_hdr_t) || udp_total > ip_payload_len) return;
    uint16_t data_len = udp_total - (uint16_t)sizeof(udp_hdr_t);
    const uint8_t *payload = (const uint8_t *)(udp + 1);
    
    uint64_t irq;
    spin_lock_irqsave(&net_lock, &irq);
    udp_rx_callback_t cb = udp_callback;
    spin_unlock_irqrestore(&net_lock, irq);

    net_udp_tap_rx(ip->src, ntohs(udp->src_port), ntohs(udp->dst_port), payload, data_len);
    handle_dhcp_packet(ntohs(udp->src_port), ntohs(udp->dst_port), payload, data_len);
    if (cb) cb(ip->src, ntohs(udp->src_port), ntohs(udp->dst_port), payload, data_len);
}


#define DNS_PORT     53
#define DNS_SRC_PORT 1053
#define DNS_TYPE_A   1
#define DNS_CLASS_IN 1

static volatile uint32_t dns_resolved_ip = 0;
static volatile bool     dns_got_reply   = false;
static uint16_t          dns_query_id    = 0xD175;

static int encode_dns_name(const char *name, uint8_t *out) {
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

void handle_dns_response(const uint8_t *payload, uint16_t len) {
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
            if ((n & 0xC0) == 0xC0) { if (end - p < 2) return; p += 2; break; }
            if (n > 63 || (size_t)(end - p) < (size_t)n + 1) return;
            p += n + 1;
        }
        if (end - p < 4) return;
        p += 4;  // QTYPE + QCLASS
    }

    // Parse answers
    uint16_t ancount = ntohs(hdr->ancount);
    for (uint16_t a = 0; a < ancount && p < end; a++) {
        // Skip name
        while (p < end) {
            uint8_t n = *p;
            if (n == 0) { p++; break; }
            if ((n & 0xC0) == 0xC0) { if (end - p < 2) return; p += 2; break; }
            if (n > 63 || (size_t)(end - p) < (size_t)n + 1) return;
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
        if ((size_t)(end - p) < rdlen) break;
        p += rdlen;
    }
}

static void handle_dns_udp(uint32_t src_ip, uint16_t src_port, uint16_t dst_port, const uint8_t *data, uint16_t len) {
    (void)src_ip;
    if (dst_port == DNS_SRC_PORT && src_port == DNS_PORT)
        handle_dns_response(data, len);
}

uint32_t resolve_dns(const char *hostname) {
    uint8_t buf[512];
    memset(buf, 0, sizeof(buf));
    dns_hdr_t *hdr = (dns_hdr_t *)buf;
    hdr->id        = htons(dns_query_id);
    hdr->flags     = htons(0x0100);  // recursion desired
    hdr->qdcount   = htons(1);
    int off = sizeof(dns_hdr_t);
    off    += encode_dns_name(hostname, buf + off);
    buf[off++] = 0x00; buf[off++] = DNS_TYPE_A;
    buf[off++] = 0x00; buf[off++] = DNS_CLASS_IN;

    dns_got_reply   = false;
    dns_resolved_ip = 0;
    udp_callback    = handle_dns_udp;

    if (!net_dns_ip || !send_udp_packet(net_dns_ip, DNS_SRC_PORT, DNS_PORT, buf, (uint16_t)off)) { udp_callback = NULL; return 0; }

    for (int i = 0; i < 3000; i++) {
        poll_net_device();
        if (dns_got_reply) {
            uint32_t ip = dns_resolved_ip;
            udp_callback = NULL;
            return ip;
        }
        sleep(1);
    }
    udp_callback = NULL;
    return 0;
}


// Max TCP payload per segment (MSS)
#define TCP_MSS       1460
#define TCP_HDR_LEN   20    // no options
#define TCP_WINDOW    8192

// Active sockets table (simple single socket for now, easily expandable)
#define TCP_MAX_SOCKETS 8
static tcp_socket_t *tcp_sockets[TCP_MAX_SOCKETS] = { NULL };

static uint16_t tcp_next_port = 49152;  // ephemeral port range

static void retain_tcp_socket(tcp_socket_t *sock) { __atomic_add_fetch(&sock->refcount, 1, __ATOMIC_ACQ_REL); }

static void release_tcp_socket(tcp_socket_t *sock) {
    if (__atomic_sub_fetch(&sock->refcount, 1, __ATOMIC_ACQ_REL) == 0) free(sock);
}

static uint16_t allocate_tcp_port(void) {
    uint64_t irq;
    spin_lock_irqsave(&net_lock, &irq);
    uint16_t p = tcp_next_port++;
    if (tcp_next_port == 0) tcp_next_port = 49152;
    spin_unlock_irqrestore(&net_lock, irq);
    return p;
}

// Send a raw TCP segment
static bool send_tcp_segment(tcp_socket_t *sock, uint8_t flags, const void *data, uint16_t data_len) {
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

    tcp->checksum = calculate_transport_checksum(net_local_ip, sock->remote_ip, IP_PROTO_TCP, buf, tcp_len);
    return send_ip_packet(sock->remote_ip, IP_PROTO_TCP, buf, tcp_len);
}

// Write data into socket RX ring buffer
static void push_tcp_rx(tcp_socket_t *sock, const uint8_t *data, uint16_t len) {
    for (uint16_t i = 0; i < len; i++) {
        uint32_t next = (sock->rx_head + 1) % TCP_RX_BUF_SIZE;
        if (next == sock->rx_tail) break;  // buffer full, drop
        sock->rx_buf[sock->rx_head] = data[i];
        sock->rx_head = next;
    }
}

void handle_tcp_packet(const uint8_t *frame, uint16_t len) {
    const ipv4_hdr_t *ip;
    uint16_t ip_hlen;
    uint16_t ip_total;
    if (!parse_ipv4_packet(frame, len, IP_PROTO_TCP, &ip, &ip_hlen, &ip_total)) return;
    if (ip_total - ip_hlen < TCP_HDR_LEN) return;
    const tcp_hdr_t *tcp = (const tcp_hdr_t *)(frame + 14 + ip_hlen);
    int tcp_hlen = ((tcp->data_off >> 4) & 0xF) * 4;
    if (tcp_hlen < TCP_HDR_LEN || tcp_hlen > ip_total - ip_hlen) return;
    int      payload_len = ip_total - ip_hlen - tcp_hlen;

    const uint8_t *payload = (const uint8_t *)tcp + tcp_hlen;

    uint16_t dst_port = ntohs(tcp->dst_port);
    uint32_t src_ip   = ip->src;

    // Find matching socket
    tcp_socket_t *sock = NULL;
    uint64_t irq;
    spin_lock_irqsave(&net_lock, &irq);
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        if (!tcp_sockets[i]) continue;
        if (tcp_sockets[i]->local_port  == dst_port && tcp_sockets[i]->remote_port == ntohs(tcp->src_port) && tcp_sockets[i]->remote_ip   == src_ip) { sock = tcp_sockets[i]; retain_tcp_socket(sock); break; }
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
            send_tcp_segment(sock, TCP_ACK, NULL, 0);
        } else if (flags & TCP_RST) { sock->state = TCP_CLOSED; }
        break;

    case TCP_ESTABLISHED:
        if (flags & TCP_RST) { sock->state = TCP_CLOSED; break; }
        // Update remote window
        sock->remote_window = ntohs(tcp->window);

        if (payload_len > 0 && seg_seq == sock->remote_seq) {
            push_tcp_rx(sock, payload, (uint16_t)payload_len);
            sock->remote_seq += payload_len;
            // Send ACK
            send_tcp_segment(sock, TCP_ACK, NULL, 0);
        }

        if (flags & TCP_FIN) {
            sock->remote_seq++;
            sock->rx_fin = true;
            sock->state  = TCP_CLOSE_WAIT;
            send_tcp_segment(sock, TCP_ACK, NULL, 0);
        }
        break;

    case TCP_FIN_WAIT1:
        if (flags & TCP_ACK) { sock->state = TCP_FIN_WAIT2; }
        if (flags & TCP_FIN) {
            sock->remote_seq++;
            sock->rx_fin = true;
            send_tcp_segment(sock, TCP_ACK, NULL, 0);
            sock->state = TCP_TIME_WAIT;
        }
        break;

    case TCP_FIN_WAIT2:
        if (flags & TCP_FIN) {
            sock->remote_seq++;
            sock->rx_fin = true;
            send_tcp_segment(sock, TCP_ACK, NULL, 0);
            sock->state = TCP_TIME_WAIT;
        }
        break;

    case TCP_LAST_ACK:
        if (flags & TCP_ACK) { sock->state = TCP_CLOSED; }
        break;

    default:
        break;
    }
    release_tcp_socket(sock);
}

tcp_socket_t *connect_tcp(uint32_t remote_ip, uint16_t remote_port) {
    tcp_socket_t *sock = malloc(sizeof(tcp_socket_t));
    if (!sock) return NULL;
    memset(sock, 0, sizeof(tcp_socket_t));

    sock->refcount   = 1;
    sock->remote_ip   = remote_ip;
    sock->remote_port = remote_port;
    sock->local_port  = allocate_tcp_port();
    sock->local_seq   = (uint32_t)(get_raw_time_counter() ^ (get_raw_time_counter() >> 17)); // random ISN
    if (sock->local_seq == 0) sock->local_seq = 1;
    sock->state       = TCP_SYN_SENT;
    sock->remote_window = 1024;

    // Register socket
    uint64_t irq;
    spin_lock_irqsave(&net_lock, &irq);
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) { if (!tcp_sockets[i]) { tcp_sockets[i] = sock; break; } }
    spin_unlock_irqrestore(&net_lock, irq);

    // Send SYN
    send_tcp_segment(sock, TCP_SYN, NULL, 0);
    sock->local_seq++;  // SYN consumes one seq number

    // Wait for SYN+ACK
    for (int i = 0; i < 5000; i++) {
        poll_net_device();
        if (sock->state == TCP_ESTABLISHED) return sock;
        if (sock->state == TCP_CLOSED) break;
        sleep(1);
    }

    free_tcp(sock);
    return NULL;
}

bool send_tcp(tcp_socket_t *sock, const void *data, uint16_t len) {
    if (!sock || sock->state != TCP_ESTABLISHED) return false;

    const uint8_t *ptr = (const uint8_t *)data;
    uint16_t remaining = len;

    while (remaining > 0) {
        uint16_t chunk = remaining < TCP_MSS ? remaining : TCP_MSS;
        if (!send_tcp_segment(sock, TCP_ACK | TCP_PSH, ptr, chunk)) return false;
        sock->local_seq += chunk;
        ptr       += chunk;
        remaining -= chunk;

        // Wait for ACK before sending next chunk
        uint32_t expected_ack = sock->local_seq;
        for (int i = 0; i < 3000; i++) {
            // Remote ACK is tracked implicitly by the receive handler updating local_seq
            // For simplicity, we just wait a bit and continue
            if (sock->state != TCP_ESTABLISHED) return false;
            if (i > 50) break;  // small wait, not full RTT tracking
            sleep(1);
        }
        (void)expected_ack;
    }
    return true;
}

int read_tcp(tcp_socket_t *sock, void *buf, int max_len) {
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

int read_all_tcp(tcp_socket_t *sock, void *buf, int max_len, int timeout_ms) {
    if (!sock) return 0;
    int total = 0;
    for (int i = 0; i < timeout_ms; i++) {
        int n = read_tcp(sock, (uint8_t *)buf + total, max_len - total);
        total += n;
        if (total >= max_len) break;
        if (sock->rx_fin && sock->rx_tail == sock->rx_head) break;
        if (sock->state == TCP_CLOSED) break;
        sleep(1);
    }
    return total;
}

void close_tcp(tcp_socket_t *sock) {
    if (!sock) return;
    if (sock->state == TCP_ESTABLISHED) {
        sock->state = TCP_FIN_WAIT1;
        send_tcp_segment(sock, TCP_FIN | TCP_ACK, NULL, 0);
        sock->local_seq++;
        // Wait for FIN+ACK
        for (int i = 0; i < 3000; i++) { if (sock->state == TCP_TIME_WAIT || sock->state == TCP_CLOSED) break; sleep(1); }
    } else if (sock->state == TCP_CLOSE_WAIT) {
        sock->state = TCP_LAST_ACK;
        send_tcp_segment(sock, TCP_FIN | TCP_ACK, NULL, 0);
        sock->local_seq++;
        for (int i = 0; i < 3000; i++) { if (sock->state == TCP_CLOSED) break; sleep(1); }
    }
    sock->state = TCP_CLOSED;
}

void free_tcp(tcp_socket_t *sock) {
    if (!sock) return;
    uint64_t irq;
    spin_lock_irqsave(&net_lock, &irq);
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) { if (tcp_sockets[i] == sock) { tcp_sockets[i] = NULL; break; } }
    spin_unlock_irqrestore(&net_lock, irq);
    release_tcp_socket(sock);
}

void poll_tcp(tcp_socket_t *sock) { (void)sock; poll_net_device(); }

bool check_tcp_connected(tcp_socket_t *sock) { return sock && sock->state == TCP_ESTABLISHED; }


void handle_net_packet(net_device_t *dev, const uint8_t *frame, uint16_t len) {
    if (!dev || !frame || len < 14 || len > NET_MAX_FRAME_SIZE) return;

    uint64_t irq;
    spin_lock_irqsave(&net_lock, &irq);
    bool is_current_device = dev == net_current_device;
    spin_unlock_irqrestore(&net_lock, irq);
    if (!is_current_device) return;

    net_packet_tap_rx(frame, len);
    handle_arp_packet(frame, len);
    handle_icmp_packet(frame, len);
    handle_udp_packet(frame, len);
    handle_tcp_packet(frame, len);
}
