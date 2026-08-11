#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define DHCP_CLIENT_PORT 68
#define DHCP_SERVER_PORT 67
#define DHCP_MAGIC_COOKIE 0x63825363U
#define DHCP_OPTIONS_SIZE 312
#define DHCP_RETRY_COUNT 3
#define DHCP_TIMEOUT_MS 1000
#define DHCP_DISCOVER 1
#define DHCP_OFFER 2
#define DHCP_REQUEST 3
#define DHCP_ACK 5
#define DHCP_NAK 6
#define DHCP_OPTION_SUBNET_MASK 1
#define DHCP_OPTION_ROUTER 3
#define DHCP_OPTION_DNS 6
#define DHCP_OPTION_REQUESTED_IP 50
#define DHCP_OPTION_LEASE_TIME 51
#define DHCP_OPTION_MESSAGE_TYPE 53
#define DHCP_OPTION_SERVER_ID 54
#define DHCP_OPTION_PARAMETER_LIST 55
#define DHCP_OPTION_CLIENT_ID 61
#define DHCP_OPTION_END 255

typedef enum {
    DHCP_STATE_IDLE,
    DHCP_STATE_WAIT_OFFER,
    DHCP_STATE_OFFERED,
    DHCP_STATE_WAIT_ACK,
    DHCP_STATE_ACKED,
    DHCP_STATE_NAKED,
} dhcp_state_t;

typedef struct {
    uint8_t op;
    uint8_t htype;
    uint8_t hlen;
    uint8_t hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;
    uint32_t yiaddr;
    uint32_t siaddr;
    uint32_t giaddr;
    uint8_t chaddr[16];
    uint8_t sname[64];
    uint8_t file[128];
    uint32_t magic_cookie;
    uint8_t options[DHCP_OPTIONS_SIZE];
} __attribute__((packed)) dhcp_packet_t;

typedef struct {
    uint32_t address;
    uint32_t subnet_mask;
    uint32_t gateway;
    uint32_t dns;
    uint32_t server;
    uint32_t lease_time;
} dhcp_lease_t;

bool configure_dhcp(void);
void handle_dhcp_packet(uint16_t src_port, uint16_t dst_port, const uint8_t *data, uint16_t len);
