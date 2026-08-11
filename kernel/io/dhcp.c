#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <netinet/in.h>
#include <main/log.h>
#include <main/boot_args.h>
#include <main/rng.h>
#include <main/string.h>
#include <io/dhcp.h>
#include <io/hpet.h>
#include <io/net.h>

static volatile dhcp_state_t dhcp_state = DHCP_STATE_IDLE;
static uint32_t dhcp_transaction_id = 0;
static dhcp_lease_t dhcp_pending_lease;

static bool parse_dhcp_options(const dhcp_packet_t *packet, uint16_t len, dhcp_lease_t *lease, uint8_t *message_type) {
    const uint8_t *cursor = packet->options;
    const uint8_t *end = (const uint8_t *)packet + len;
    *message_type = 0;

    while (cursor < end) {
        uint8_t code = *cursor++;
        if (code == 0) continue;
        if (code == DHCP_OPTION_END) break;
        if (cursor >= end) return false;
        uint8_t option_len = *cursor++;
        if ((size_t)(end - cursor) < option_len) return false;

        if (code == DHCP_OPTION_MESSAGE_TYPE && option_len == 1) *message_type = cursor[0];
        else if (code == DHCP_OPTION_SUBNET_MASK && option_len >= 4) memcpy(&lease->subnet_mask, cursor, 4);
        else if (code == DHCP_OPTION_ROUTER && option_len >= 4) memcpy(&lease->gateway, cursor, 4);
        else if (code == DHCP_OPTION_DNS && option_len >= 4) memcpy(&lease->dns, cursor, 4);
        else if (code == DHCP_OPTION_SERVER_ID && option_len == 4) memcpy(&lease->server, cursor, 4);
        else if (code == DHCP_OPTION_LEASE_TIME && option_len == 4) {
            uint32_t lease_time;
            memcpy(&lease_time, cursor, 4);
            lease->lease_time = ntohl(lease_time);
        }
        cursor += option_len;
    }
    return *message_type != 0;
}

static bool send_dhcp_message(uint8_t message_type, uint32_t requested_ip, uint32_t server) {
    if (!net_current_device) return false;

    dhcp_packet_t packet;
    memset(&packet, 0, sizeof(packet));
    packet.op = 1;
    packet.htype = 1;
    packet.hlen = 6;
    packet.xid = dhcp_transaction_id;
    packet.flags = htons(0x8000);
    memcpy(packet.chaddr, net_current_device->mac, 6);
    packet.magic_cookie = htonl(DHCP_MAGIC_COOKIE);

    size_t offset = 0;
    packet.options[offset++] = DHCP_OPTION_MESSAGE_TYPE;
    packet.options[offset++] = 1;
    packet.options[offset++] = message_type;
    if (requested_ip) {
        packet.options[offset++] = DHCP_OPTION_REQUESTED_IP;
        packet.options[offset++] = 4;
        memcpy(packet.options + offset, &requested_ip, 4);
        offset += 4;
    }
    if (server) {
        packet.options[offset++] = DHCP_OPTION_SERVER_ID;
        packet.options[offset++] = 4;
        memcpy(packet.options + offset, &server, 4);
        offset += 4;
    }
    packet.options[offset++] = DHCP_OPTION_CLIENT_ID;
    packet.options[offset++] = 7;
    packet.options[offset++] = 1;
    memcpy(packet.options + offset, net_current_device->mac, 6);
    offset += 6;
    packet.options[offset++] = DHCP_OPTION_PARAMETER_LIST;
    packet.options[offset++] = 4;
    packet.options[offset++] = DHCP_OPTION_SUBNET_MASK;
    packet.options[offset++] = DHCP_OPTION_ROUTER;
    packet.options[offset++] = DHCP_OPTION_DNS;
    packet.options[offset++] = DHCP_OPTION_LEASE_TIME;
    packet.options[offset++] = DHCP_OPTION_END;

    size_t length = offsetof(dhcp_packet_t, options) + offset;
    if (length < 300) length = 300;
    return send_udp_packet(UINT32_MAX, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, &packet, (uint16_t)length);
}

void handle_dhcp_packet(uint16_t src_port, uint16_t dst_port, const uint8_t *data, uint16_t len) {
    if (src_port != DHCP_SERVER_PORT || dst_port != DHCP_CLIENT_PORT || !data) return;
    if (len < offsetof(dhcp_packet_t, options)) return;

    const dhcp_packet_t *packet = (const dhcp_packet_t *)data;
    if (packet->op != 2 || packet->htype != 1 || packet->hlen != 6) return;
    if (packet->xid != dhcp_transaction_id || ntohl(packet->magic_cookie) != DHCP_MAGIC_COOKIE) return;
    if (!net_current_device || memcmp(packet->chaddr, net_current_device->mac, 6)) return;

    dhcp_lease_t lease = dhcp_state == DHCP_STATE_WAIT_ACK ? dhcp_pending_lease : (dhcp_lease_t){0};
    if (packet->yiaddr) lease.address = packet->yiaddr;
    uint8_t message_type;
    if (!parse_dhcp_options(packet, len, &lease, &message_type)) return;

    if (message_type == DHCP_OFFER && dhcp_state == DHCP_STATE_WAIT_OFFER && lease.address && lease.server) {
        dhcp_pending_lease = lease;
        dhcp_state = DHCP_STATE_OFFERED;
    } else if (message_type == DHCP_ACK && dhcp_state == DHCP_STATE_WAIT_ACK && lease.address) {
        dhcp_pending_lease = lease;
        dhcp_state = DHCP_STATE_ACKED;
    } else if (message_type == DHCP_NAK && dhcp_state == DHCP_STATE_WAIT_ACK) {
        dhcp_state = DHCP_STATE_NAKED;
    }
}

bool configure_dhcp(void) {
    net_local_ip = 0;
    net_gateway_ip = 0;
    net_dns_ip = 0;
    net_subnet_mask = 0;

    if (!has_boot_arg("ip=dhcp") && !has_boot_arg("ip=on") && !has_boot_arg("ip=any")) {
        if (!get_arg_value("ip") || has_boot_arg("ip=off") || has_boot_arg("ip=none")) return false;
        log("net: unsupported ip configuration\n");
        return false;
    }
    if (!net_current_device) { log("dhcp: no network device available\n"); return false; }

    for (int attempt = 0; attempt < DHCP_RETRY_COUNT; attempt++) {
        get_random_bytes(&dhcp_transaction_id, sizeof(dhcp_transaction_id));
        if (!dhcp_transaction_id) dhcp_transaction_id = htonl(1);
        memset(&dhcp_pending_lease, 0, sizeof(dhcp_pending_lease));
        dhcp_state = DHCP_STATE_WAIT_OFFER;
        if (!send_dhcp_message(DHCP_DISCOVER, 0, 0)) continue;

        for (int elapsed = 0; elapsed < DHCP_TIMEOUT_MS && dhcp_state == DHCP_STATE_WAIT_OFFER; elapsed++) {
            poll_net_device();
            sleep(1);
        }
        if (dhcp_state != DHCP_STATE_OFFERED) continue;

        dhcp_state = DHCP_STATE_WAIT_ACK;
        if (!send_dhcp_message(DHCP_REQUEST, dhcp_pending_lease.address, dhcp_pending_lease.server)) continue;
        for (int elapsed = 0; elapsed < DHCP_TIMEOUT_MS && dhcp_state == DHCP_STATE_WAIT_ACK; elapsed++) {
            poll_net_device();
            sleep(1);
        }
        if (dhcp_state != DHCP_STATE_ACKED) continue;

        net_local_ip = dhcp_pending_lease.address;
        net_gateway_ip = dhcp_pending_lease.gateway;
        net_dns_ip = dhcp_pending_lease.dns;
        net_subnet_mask = dhcp_pending_lease.subnet_mask;
        dhcp_state = DHCP_STATE_IDLE;
        log("dhcp: configured dhcp\n");
        return true;
    }

    dhcp_state = DHCP_STATE_IDLE;
    log("dhcp: no lease, network remains unconfigured\n");
    return false;
}
