#pragma once

#include <stdint.h>
#include <stddef.h>
#include <io/sockets.h>

int create_inet_socket_obj(int type, int protocol, socket_t **out);
void net_udp_tap_rx(uint32_t src_ip, uint16_t src_port, uint16_t dst_port, const uint8_t *payload, uint16_t len);
