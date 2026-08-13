#include <stdint.h>
#include <main/log.h>
#include <main/spinlocks.h>
#include <io/ec.h>
#include <io/time.h>
#include <io/io.h>
#include <uacpi/acpi.h>
#include <uacpi/namespace.h>
#include <uacpi/opregion.h>
#include <uacpi/resources.h>
#include <uacpi/tables.h>
#include <uacpi/utilities.h>

static ec_controller_t ec_controller;

static uacpi_status ec_wait_status(ec_controller_t *ec, uint8_t mask, uint8_t expected) {
    uint64_t start = get_monotonic_time_us();
    uint64_t spins = 0;
    while ((inb(ec->command_port) & mask) != expected) {
        if (start && get_monotonic_time_us() - start >= EC_TIMEOUT_US) return UACPI_STATUS_HARDWARE_TIMEOUT;
        if (!start && ++spins >= EC_TIMEOUT_US * 16) return UACPI_STATUS_HARDWARE_TIMEOUT;
        __asm__ volatile("pause");
    }
    return UACPI_STATUS_OK;
}

static void ec_drain_output(ec_controller_t *ec) {
    for (int i = 0; i < 16 && (inb(ec->command_port) & EC_STATUS_OBF); i++) (void)inb(ec->data_port);
}

static uacpi_status ec_read_byte(ec_controller_t *ec, uint8_t address, uint8_t *value) {
    ec_drain_output(ec);
    uacpi_status status = ec_wait_status(ec, EC_STATUS_IBF, 0);
    if (uacpi_unlikely_error(status)) return status;
    outb(ec->command_port, EC_COMMAND_READ);
    status = ec_wait_status(ec, EC_STATUS_IBF, 0);
    if (uacpi_unlikely_error(status)) return status;
    outb(ec->data_port, address);
    status = ec_wait_status(ec, EC_STATUS_OBF, EC_STATUS_OBF);
    if (uacpi_unlikely_error(status)) return status;
    *value = inb(ec->data_port);
    return UACPI_STATUS_OK;
}

static uacpi_status ec_write_byte(ec_controller_t *ec, uint8_t address, uint8_t value) {
    ec_drain_output(ec);
    uacpi_status status = ec_wait_status(ec, EC_STATUS_IBF, 0);
    if (uacpi_unlikely_error(status)) return status;
    outb(ec->command_port, EC_COMMAND_WRITE);
    status = ec_wait_status(ec, EC_STATUS_IBF, 0);
    if (uacpi_unlikely_error(status)) return status;
    outb(ec->data_port, address);
    status = ec_wait_status(ec, EC_STATUS_IBF, 0);
    if (uacpi_unlikely_error(status)) return status;
    outb(ec->data_port, value);
    return ec_wait_status(ec, EC_STATUS_IBF, 0);
}

static uacpi_status ec_region_handler(uacpi_region_op op, uacpi_handle op_data) {
    if (op == UACPI_REGION_OP_ATTACH) {
        uacpi_region_attach_data *data = op_data;
        data->out_region_context = data->handler_context;
        return UACPI_STATUS_OK;
    }
    if (op == UACPI_REGION_OP_DETACH) return UACPI_STATUS_OK;
    if (op != UACPI_REGION_OP_READ && op != UACPI_REGION_OP_WRITE) return UACPI_STATUS_INVALID_ARGUMENT;
    uacpi_region_rw_data *data = op_data;
    ec_controller_t *ec = data->handler_context;
    if (!ec || !data->byte_width || data->offset > 0xFF || data->byte_width > 8 || data->offset + data->byte_width > 0x100) return UACPI_STATUS_INVALID_ARGUMENT;
    uint64_t flags;
    spin_lock_irqsave(&ec->lock, &flags);
    uacpi_status status = UACPI_STATUS_OK;
    if (op == UACPI_REGION_OP_READ) data->value = 0;
    for (uacpi_u8 i = 0; i < data->byte_width; i++) {
        if (op == UACPI_REGION_OP_READ) {
            uint8_t value;
            status = ec_read_byte(ec, (uint8_t)(data->offset + i), &value);
            if (uacpi_unlikely_error(status)) break;
            data->value |= (uacpi_u64)value << (i * 8);
        } else {
            status = ec_write_byte(ec, (uint8_t)(data->offset + i), (uint8_t)(data->value >> (i * 8)));
            if (uacpi_unlikely_error(status)) break;
        }
    }
    spin_unlock_irqrestore(&ec->lock, flags);
    return status;
}

static uacpi_iteration_decision ec_resource_callback(void *user, uacpi_resource *resource) {
    ec_discovery_t *discovery = user;
    uint16_t port = 0;
    if (resource->type == UACPI_RESOURCE_TYPE_IO && resource->io.length) port = resource->io.minimum;
    else if (resource->type == UACPI_RESOURCE_TYPE_FIXED_IO && resource->fixed_io.length) port = resource->fixed_io.address;
    if (port && discovery->port_count < 2) discovery->ports[discovery->port_count++] = port;
    return discovery->port_count == 2 ? UACPI_ITERATION_DECISION_BREAK : UACPI_ITERATION_DECISION_CONTINUE;
}

static uacpi_iteration_decision ec_device_callback(void *user, uacpi_namespace_node *node, uacpi_u32 depth) {
    (void)depth;
    ec_discovery_t *discovery = user;
    if (!discovery->ec->data_port || !discovery->ec->command_port) {
        discovery->port_count = 0;
        discovery->status = uacpi_for_each_device_resource(node, "_CRS", ec_resource_callback, discovery);
        if (uacpi_unlikely_error(discovery->status) || discovery->port_count != 2) return UACPI_ITERATION_DECISION_CONTINUE;
        discovery->ec->data_port = discovery->ports[0];
        discovery->ec->command_port = discovery->ports[1];
    }
    discovery->ec->node = node;
    discovery->status = uacpi_install_address_space_handler(node, UACPI_ADDRESS_SPACE_EMBEDDED_CONTROLLER, ec_region_handler, discovery->ec);
    if (uacpi_unlikely_error(discovery->status)) return UACPI_ITERATION_DECISION_BREAK;
    discovery->status = uacpi_reg_all_opregions(node, UACPI_ADDRESS_SPACE_EMBEDDED_CONTROLLER);
    return UACPI_ITERATION_DECISION_BREAK;
}

static void ec_discover_ecdt(ec_controller_t *ec) {
    uacpi_table table;
    if (uacpi_table_find_by_signature(ACPI_ECDT_SIGNATURE, &table) != UACPI_STATUS_OK) return;
    struct acpi_ecdt *ecdt = table.ptr;
    if (ecdt->ec_data.address_space_id == UACPI_ADDRESS_SPACE_SYSTEM_IO && ecdt->ec_control.address_space_id == UACPI_ADDRESS_SPACE_SYSTEM_IO && ecdt->ec_data.address <= UINT16_MAX && ecdt->ec_control.address <= UINT16_MAX) {
        ec->data_port = (uint16_t)ecdt->ec_data.address;
        ec->command_port = (uint16_t)ecdt->ec_control.address;
    }
    uacpi_table_unref(&table);
}

uacpi_status init_ec(void) {
    ec_controller.data_port = 0;
    ec_controller.command_port = 0;
    ec_controller.lock = SPINLOCK_INIT;
    ec_controller.node = NULL;
    ec_discover_ecdt(&ec_controller);
    ec_discovery_t discovery = { .ec = &ec_controller, .status = UACPI_STATUS_NOT_FOUND };
    uacpi_status status = uacpi_find_devices("PNP0C09", ec_device_callback, &discovery);
    if (uacpi_unlikely_error(status)) return status;
    if (uacpi_unlikely_error(discovery.status)) return discovery.status;
    log("ec: initialized ec\n");
    return UACPI_STATUS_OK;
}
