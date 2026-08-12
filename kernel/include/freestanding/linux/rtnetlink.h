#pragma once

#include <stdint.h>
#include <linux/netlink.h>

#define RTM_NEWLINK 16
#define RTM_DELLINK 17
#define RTM_GETLINK 18

#define RTMGRP_LINK 1

#define IFLA_UNSPEC    0
#define IFLA_ADDRESS   1
#define IFLA_BROADCAST 2
#define IFLA_IFNAME    3
#define IFLA_MTU       4
#define IFLA_LINK      5
#define IFLA_OPERSTATE 16
#define IFLA_LINKMODE  17

#define IF_OPER_UNKNOWN 0
#define IF_OPER_DOWN    2
#define IF_OPER_UP      6

#define RTA_ALIGNTO 4U
#define RTA_ALIGN(length) (((length) + RTA_ALIGNTO - 1) & ~(RTA_ALIGNTO - 1))
#define RTA_LENGTH(length) (RTA_ALIGN(sizeof(struct rtattr)) + (length))
#define RTA_SPACE(length) RTA_ALIGN(RTA_LENGTH(length))
#define RTA_DATA(attribute) ((void *)((char *)(attribute) + RTA_LENGTH(0)))

struct ifinfomsg {
    uint8_t ifi_family;
    uint8_t __ifi_pad;
    uint16_t ifi_type;
    int32_t ifi_index;
    uint32_t ifi_flags;
    uint32_t ifi_change;
};

struct rtattr {
    uint16_t rta_len;
    uint16_t rta_type;
};
