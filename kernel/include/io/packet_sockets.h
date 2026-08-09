#pragma once

#include <stdint.h>
#include <io/sockets.h>

int  create_packet_socket_obj(int type, int protocol, socket_t **out);
void net_packet_tap_rx(const uint8_t *packet, uint16_t len);
