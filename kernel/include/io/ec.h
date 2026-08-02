#pragma once

#include <stdint.h>
#include <main/spinlocks.h>
#include <uacpi/status.h>

#define EC_STATUS_OBF 0x01
#define EC_STATUS_IBF 0x02
#define EC_COMMAND_READ 0x80
#define EC_COMMAND_WRITE 0x81
#define EC_TIMEOUT_US 1000000

typedef struct {
    uint16_t data_port;
    uint16_t command_port;
    spinlock_t lock;
    uacpi_namespace_node *node;
} ec_controller_t;

typedef struct {
    ec_controller_t *ec;
    uint16_t ports[2];
    uint8_t port_count;
    uacpi_status status;
} ec_discovery_t;

uacpi_status init_ec(void);
