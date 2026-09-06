#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <errno.h>
#include <main/log.h>
#include <main/string.h>
#include <io/devices.h>
#include <io/gpt.h>
#include <io/mbr.h>
#include <io/time.h>
#include <io/usb.h>
#include <io/usb_bot.h>
#include <mm/mm.h>

static uint32_t bot_next_tag = 1;
usb_bot_entry_t bot_entries[USB_BOT_MAX_DEVICES];

static void write_be16(uint8_t *buf, uint16_t val) {
    buf[0] = (uint8_t)(val >> 8);
    buf[1] = (uint8_t)val;
}

static uint32_t read_be32(uint8_t *buf) {
    return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) | ((uint32_t)buf[2] << 8) | (uint32_t)buf[3];
}

static void write_be32(uint8_t *buf, uint32_t val) {
    buf[0] = (uint8_t)(val >> 24);
    buf[1] = (uint8_t)(val >> 16);
    buf[2] = (uint8_t)(val >> 8);
    buf[3] = (uint8_t)val;
}

static uint32_t get_next_tag(void) {
    uint32_t tag = __sync_fetch_and_add(&bot_next_tag, 1);
    while (tag == 0) tag = __sync_fetch_and_add(&bot_next_tag, 1);
    if (bot_next_tag == 0) bot_next_tag = 1;
    return tag;
}

static int find_bot_entry(usb_hcd_t *hcd, uint8_t port_id) {
    for (int i = 0; i < USB_BOT_MAX_DEVICES; i++) { if (bot_entries[i].present && bot_entries[i].hcd == hcd && bot_entries[i].dev && bot_entries[i].dev->port_id == port_id) return i; }
    return -1;
}

static int alloc_bot_entry(void) {
    for (int i = 0; i < USB_BOT_MAX_DEVICES; i++) { if (!bot_entries[i].present) return i; }
    return -1;
}

static bool valid_bulk_mps(uint16_t mps, uint8_t speed) {
    if (mps == 8 || mps == 16 || mps == 32 || mps == 64) return true;
    if (speed == USB_SPEED_HIGH && mps == 512) return true;
    return false;
}

static int parse_bot_config(uint8_t *buf, uint16_t total_len, uint8_t speed, uint8_t *iface, uint8_t *in_ep, uint8_t *out_ep, uint16_t *in_mps, uint16_t *out_mps) {
    if (!buf || !iface || !in_ep || !out_ep || !in_mps || !out_mps) return -1;
    if (total_len < 9) return -1;
    int cur_iface = -1;
    uint8_t found_in = 0;
    uint8_t found_out = 0;
    uint16_t found_in_mps = 0;
    uint16_t found_out_mps = 0;
    uint8_t found_iface = 0;
    uint16_t offset = 0;
    while (offset + 2 <= total_len) {
        uint8_t desc_len = buf[offset];
        uint8_t desc_type = buf[offset + 1];
        if (desc_len < 2 || offset + desc_len > total_len) break;
        if (desc_type == USB_DESC_INTERFACE && offset + 9 <= total_len) { if (buf[offset + 5] == USB_MSC_CLASS && buf[offset + 6] == USB_MSC_SUBCLASS_SCSI && buf[offset + 7] == USB_MSC_PROTOCOL_BULK_ONLY && buf[offset + 4] == 2) { cur_iface = buf[offset + 2]; found_iface = (uint8_t)cur_iface; found_in = 0; found_out = 0; } else cur_iface = -1; }
        if (cur_iface >= 0 && desc_type == USB_DESC_ENDPOINT && offset + 7 <= total_len) {
            uint8_t addr = buf[offset + 2];
            uint8_t attr = buf[offset + 3];
            uint16_t mps = (uint16_t)(buf[offset + 4] | ((uint16_t)buf[offset + 5] << 8));
            mps &= 0x7FF;
            if ((attr & 3) == USB_EP_TYPE_BULK && (addr & 0x0F) != 0 && valid_bulk_mps(mps, speed)) { if (addr & 0x80 && !found_in) { found_in = addr; found_in_mps = mps; } if (!(addr & 0x80) && !found_out) { found_out = addr; found_out_mps = mps; } }
            if (found_in && found_out) { *iface = found_iface; *in_ep = found_in; *out_ep = found_out; *in_mps = found_in_mps; *out_mps = found_out_mps; return (int)found_iface; }
        }
        offset += desc_len;
    }
    return -1;
}

static int retry_control_transfer(usb_hcd_t *hcd, usb_device_t *dev, usb_setup_packet_t *setup, void *data, uint16_t len, int retries, uint64_t delay) {
    if (!hcd || !dev || !setup) return -1;
    if (retries < 1) retries = 1;
    int ret = -1;
    for (int i = 0; i < retries; i++) { ret = hcd->control_transfer(hcd, dev, setup, data, len); if (ret >= 0) return ret; if (i + 1 < retries && delay) sleep(delay); }
    return ret;
}

static int transfer_bulk_chunks(usb_hcd_t *hcd, usb_device_t *dev, uint8_t ep, uint8_t *data, uint32_t len, uint32_t *done_out) {
    if (!hcd || !dev || !data) return -1;
    if (done_out) *done_out = 0;
    if (len == 0) return 0;
    uint32_t done = 0;
    while (done < len) {
        uint32_t chunk = len - done;
        if (chunk > USB_BOT_BULK_CHUNK) chunk = USB_BOT_BULK_CHUNK;
        int ret = hcd->bulk_transfer(hcd, dev, ep, data + done, (uint16_t)chunk);
        if (ret < 0) return -1;
        if (ret == 0) break;
        done += (uint32_t)ret;
        if ((uint32_t)ret < chunk) break;
    }
    if (done_out) *done_out = done;
    return (int)done;
}

static int get_max_lun(usb_hcd_t *hcd, usb_device_t *dev, uint8_t iface) {
    if (!hcd || !dev) return 0;
    uint8_t lun = 0;
    usb_setup_packet_t setup;
    setup.bmRequestType = USB_REQTYPE_DIR_IN | USB_REQTYPE_CLASS | USB_REQTYPE_INTERFACE;
    setup.bRequest = USB_BOT_GET_MAX_LUN;
    setup.wValue = 0;
    setup.wIndex = iface;
    setup.wLength = 1;
    int ret = hcd->control_transfer(hcd, dev, &setup, &lun, 1);
    if (ret != 1) return 0;
    if (lun > USB_BOT_MAX_LUN) return 0;
    return (int)lun;
}

static int clear_bulk_halt(usb_hcd_t *hcd, usb_device_t *dev, uint8_t ep) {
    if (!hcd || !dev) return -1;
    usb_setup_packet_t setup;
    setup.bmRequestType = USB_REQTYPE_DIR_OUT | USB_REQTYPE_STANDARD | USB_REQTYPE_ENDPOINT;
    setup.bRequest = USB_REQ_CLEAR_FEATURE;
    setup.wValue = 0;
    setup.wIndex = ep;
    setup.wLength = 0;
    int ret = hcd->control_transfer(hcd, dev, &setup, NULL, 0);
    if (ret < 0) return -1;
    if (ep & 0x80) dev->bulk_in_toggle = 0;
    else dev->bulk_out_toggle = 0;
    return 0;
}

static int reset_bot_device(usb_hcd_t *hcd, usb_device_t *dev, uint8_t iface, uint8_t in_ep, uint8_t out_ep) {
    if (!hcd || !dev) return -1;
    usb_setup_packet_t setup;
    setup.bmRequestType = USB_REQTYPE_DIR_OUT | USB_REQTYPE_CLASS | USB_REQTYPE_INTERFACE;
    setup.bRequest = USB_BOT_MASS_RESET;
    setup.wValue = 0;
    setup.wIndex = iface;
    setup.wLength = 0;
    int reset_ret = hcd->control_transfer(hcd, dev, &setup, NULL, 0);
    sleep(100);
    clear_bulk_halt(hcd, dev, in_ep);
    clear_bulk_halt(hcd, dev, out_ep);
    dev->bulk_in_toggle = 0;
    dev->bulk_out_toggle = 0;
    if (reset_ret < 0) return -1;
    return 0;
}

static int bot_transfer(usb_hcd_t *hcd, usb_device_t *dev, uint8_t iface, uint8_t lun, uint8_t *cdb, uint8_t cdb_len, uint8_t *data, uint32_t data_len, bool dir_in, uint8_t *csw_status) {
    if (!hcd || !dev || !cdb || cdb_len == 0 || cdb_len > 16) return -1;
    if (data_len && !data) return -1;
    if (lun > USB_BOT_MAX_LUN) return -1;
    uint8_t in_ep = dev->bulk_in_endpoint;
    uint8_t out_ep = dev->bulk_out_endpoint;
    if ((in_ep & 0x80) == 0 || (out_ep & 0x80) != 0) return -1;
    usb_bot_cbw_t cbw;
    memset(&cbw, 0, sizeof(cbw));
    cbw.signature = USB_BOT_CBW_SIGNATURE;
    cbw.tag = get_next_tag();
    cbw.data_length = data_len;
    cbw.flags = 0;
    if (dir_in && data_len) cbw.flags = 0x80;
    cbw.lun = lun & 0x0F;
    cbw.cb_length = cdb_len;
    memcpy(cbw.cb, cdb, cdb_len);
    int ret = hcd->bulk_transfer(hcd, dev, out_ep, &cbw, USB_BOT_CBW_LENGTH);
    if (ret != USB_BOT_CBW_LENGTH) { reset_bot_device(hcd, dev, iface, in_ep, out_ep); return -1; }
    uint32_t done = 0;
    if (data_len) {
        if (dir_in) {
            int dret = transfer_bulk_chunks(hcd, dev, in_ep, data, data_len, &done);
            if (dret < 0) clear_bulk_halt(hcd, dev, in_ep);
        } else {
            int dret = transfer_bulk_chunks(hcd, dev, out_ep, data, data_len, &done);
            if (dret < 0 || done != data_len) { clear_bulk_halt(hcd, dev, out_ep); reset_bot_device(hcd, dev, iface, in_ep, out_ep); return -1; }
        }
    }
    usb_bot_csw_t csw;
    for (int i = 0; i < USB_BOT_CSW_RETRIES; i++) {
        memset(&csw, 0, sizeof(csw));
        ret = hcd->bulk_transfer(hcd, dev, in_ep, &csw, USB_BOT_CSW_LENGTH);
        if (ret != USB_BOT_CSW_LENGTH) { clear_bulk_halt(hcd, dev, in_ep); continue; }
        if (csw.signature != USB_BOT_CSW_SIGNATURE || csw.tag != cbw.tag) { reset_bot_device(hcd, dev, iface, in_ep, out_ep); return -1; }
        if (csw.residue > data_len) { reset_bot_device(hcd, dev, iface, in_ep, out_ep); return -1; }
        if (csw.status == USB_BOT_STATUS_PASS) {
            if (csw.residue != data_len - done) { log("usb bot: residue mismatch (%u != %u) on port %d\n", csw.residue, data_len - done, dev->port_id); reset_bot_device(hcd, dev, iface, in_ep, out_ep); return -1; }
            if (done != data_len) { log("usb bot: short data phase (%u of %u bytes) on port %d\n", done, data_len, dev->port_id); return -1; }
            return 0;
        }
        if (csw.status == USB_BOT_STATUS_FAIL) { if (csw_status) *csw_status = csw.status; return -1; }
        if (csw.status == USB_BOT_STATUS_PHASE) { reset_bot_device(hcd, dev, iface, in_ep, out_ep); return -1; }
        return -1;
    }
    reset_bot_device(hcd, dev, iface, in_ep, out_ep);
    return -1;
}

static int request_sense_data(usb_hcd_t *hcd, usb_device_t *dev, uint8_t iface, uint8_t lun, uint8_t *buf) {
    uint8_t cdb[6];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = USB_SCSI_REQUEST_SENSE;
    cdb[4] = USB_BOT_SENSE_LENGTH;
    return bot_transfer(hcd, dev, iface, lun, cdb, 6, buf, USB_BOT_SENSE_LENGTH, true, NULL);
}

static void report_bot_sense(usb_hcd_t *hcd, usb_device_t *dev, uint8_t iface, uint8_t lun) {
    uint8_t sense[USB_BOT_SENSE_LENGTH];
    memset(sense, 0, sizeof(sense));
    if (request_sense_data(hcd, dev, iface, lun, sense) != 0) return;
    log("usb bot: command failed (port %d, lun %d)\n", dev->port_id, lun);
}

static int run_bot_command(usb_hcd_t *hcd, usb_device_t *dev, uint8_t iface, uint8_t lun, uint8_t *cdb, uint8_t cdb_len, uint8_t *data, uint32_t data_len, bool dir_in) {
    uint8_t csw_status = 0;
    int ret = bot_transfer(hcd, dev, iface, lun, cdb, cdb_len, data, data_len, dir_in, &csw_status);
    if (ret < 0 && csw_status == USB_BOT_STATUS_FAIL) report_bot_sense(hcd, dev, iface, lun);
    return ret;
}

static int test_unit_ready(usb_hcd_t *hcd, usb_device_t *dev, uint8_t iface, uint8_t lun) {
    uint8_t cdb[6];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = USB_SCSI_TEST_UNIT_READY;
    return run_bot_command(hcd, dev, iface, lun, cdb, 6, NULL, 0, false);
}

static int get_inquiry_data(usb_hcd_t *hcd, usb_device_t *dev, uint8_t iface, uint8_t lun, uint8_t *buf) {
    uint8_t cdb[6];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = USB_SCSI_INQUIRY;
    cdb[4] = USB_BOT_INQUIRY_LENGTH;
    return run_bot_command(hcd, dev, iface, lun, cdb, 6, buf, USB_BOT_INQUIRY_LENGTH, true);
}

static int get_device_capacity(usb_hcd_t *hcd, usb_device_t *dev, uint8_t iface, uint8_t lun, uint64_t *count, uint32_t *size) {
    if (!hcd || !dev || !count || !size) return -1;
    uint8_t cdb[10];
    uint8_t buf[USB_BOT_CAPACITY_LENGTH];
    memset(cdb, 0, sizeof(cdb));
    memset(buf, 0, sizeof(buf));
    cdb[0] = USB_SCSI_READ_CAPACITY10;
    int ret = run_bot_command(hcd, dev, iface, lun, cdb, 10, buf, USB_BOT_CAPACITY_LENGTH, true);
    if (ret != 0) return -1;
    uint32_t last = read_be32(buf);
    uint32_t blen = read_be32(buf + 4);
    if (blen != 512 && blen != 1024 && blen != 2048 && blen != 4096) return -1;
    if (last == 0xFFFFFFFF) {
        uint8_t cdb16[16];
        uint8_t buf16[32];
        memset(cdb16, 0, sizeof(cdb16));
        memset(buf16, 0, sizeof(buf16));
        cdb16[0] = 0x9E;
        cdb16[1] = 0x10;
        write_be32(cdb16 + 10, 32);
        ret = run_bot_command(hcd, dev, iface, lun, cdb16, 16, buf16, 32, true);
        if (ret != 0) return -1;
        uint64_t last64 = ((uint64_t)buf16[0] << 56) | ((uint64_t)buf16[1] << 48) | ((uint64_t)buf16[2] << 40) | ((uint64_t)buf16[3] << 32) | ((uint64_t)buf16[4] << 24) | ((uint64_t)buf16[5] << 16) | ((uint64_t)buf16[6] << 8) | (uint64_t)buf16[7];
        uint32_t blen16 = read_be32(buf16 + 8);
        if (blen16 != 512 && blen16 != 1024 && blen16 != 2048 && blen16 != 4096) return -1;
        if (last64 == 0 || last64 == (uint64_t)-1) return -1;
        *count = last64 + 1;
        *size = blen16;
        return 0;
    }
    *count = (uint64_t)last + 1;
    *size = blen;
    return 0;
}

static int read_bot_blocks(usb_bot_entry_t *entry, uint64_t lba, uint16_t blocks, uint8_t *buf) {
    if (!entry || !entry->dev || !entry->hcd || !buf || blocks == 0) return -1;
    uint32_t blen = entry->block_size;
    if (blen != 512 && blen != 1024 && blen != 2048 && blen != 4096) return -1;
    uint32_t total = (uint32_t)blocks * blen;
    if (lba <= 0xFFFFFFFF) {
        uint8_t cdb[10];
        memset(cdb, 0, sizeof(cdb));
        cdb[0] = USB_SCSI_READ10;
        write_be32(cdb + 2, (uint32_t)lba);
        write_be16(cdb + 7, blocks);
        return run_bot_command(entry->hcd, entry->dev, entry->interface_number, entry->lun, cdb, 10, buf, total, true);
    }
    uint8_t cdb16[16];
    memset(cdb16, 0, sizeof(cdb16));
    cdb16[0] = 0x88;
    cdb16[2] = (uint8_t)(lba >> 56);
    cdb16[3] = (uint8_t)(lba >> 48);
    cdb16[4] = (uint8_t)(lba >> 40);
    cdb16[5] = (uint8_t)(lba >> 32);
    cdb16[6] = (uint8_t)(lba >> 24);
    cdb16[7] = (uint8_t)(lba >> 16);
    cdb16[8] = (uint8_t)(lba >> 8);
    cdb16[9] = (uint8_t)lba;
    cdb16[10] = (uint8_t)(blocks >> 24);
    cdb16[11] = (uint8_t)(blocks >> 16);
    cdb16[12] = (uint8_t)(blocks >> 8);
    cdb16[13] = (uint8_t)blocks;
    return run_bot_command(entry->hcd, entry->dev, entry->interface_number, entry->lun, cdb16, 16, buf, total, true);
}

static int write_bot_blocks(usb_bot_entry_t *entry, uint64_t lba, uint16_t blocks, uint8_t *buf) {
    if (!entry || !entry->dev || !entry->hcd || !buf || blocks == 0) return -1;
    uint32_t blen = entry->block_size;
    if (blen != 512 && blen != 1024 && blen != 2048 && blen != 4096) return -1;
    uint32_t total = (uint32_t)blocks * blen;
    if (lba <= 0xFFFFFFFF) {
        uint8_t cdb[10];
        memset(cdb, 0, sizeof(cdb));
        cdb[0] = USB_SCSI_WRITE10;
        write_be32(cdb + 2, (uint32_t)lba);
        write_be16(cdb + 7, blocks);
        return run_bot_command(entry->hcd, entry->dev, entry->interface_number, entry->lun, cdb, 10, buf, total, false);
    }
    uint8_t cdb16[16];
    memset(cdb16, 0, sizeof(cdb16));
    cdb16[0] = 0x8A;
    cdb16[2] = (uint8_t)(lba >> 56);
    cdb16[3] = (uint8_t)(lba >> 48);
    cdb16[4] = (uint8_t)(lba >> 40);
    cdb16[5] = (uint8_t)(lba >> 32);
    cdb16[6] = (uint8_t)(lba >> 24);
    cdb16[7] = (uint8_t)(lba >> 16);
    cdb16[8] = (uint8_t)(lba >> 8);
    cdb16[9] = (uint8_t)lba;
    cdb16[10] = (uint8_t)(blocks >> 24);
    cdb16[11] = (uint8_t)(blocks >> 16);
    cdb16[12] = (uint8_t)(blocks >> 8);
    cdb16[13] = (uint8_t)blocks;
    return run_bot_command(entry->hcd, entry->dev, entry->interface_number, entry->lun, cdb16, 16, buf, total, false);
}

static int probe_bot_lun(usb_hcd_t *hcd, usb_device_t *dev, uint8_t iface, uint8_t lun) {
    bool ready = false;
    for (int i = 0; i < USB_BOT_TUR_RETRIES; i++) {
        if (test_unit_ready(hcd, dev, iface, lun) == 0) { ready = true; break; }
        report_bot_sense(hcd, dev, iface, lun);
        sleep(100);
    }
    if (!ready) { log("usb bot: lun %d never became ready (port %d)\n", lun, dev->port_id); return -1; }
    uint8_t inquiry[USB_BOT_INQUIRY_LENGTH];
    memset(inquiry, 0, sizeof(inquiry));
    get_inquiry_data(hcd, dev, iface, lun, inquiry);
    uint64_t blocks = 0;
    uint32_t blen = 0;
    if (get_device_capacity(hcd, dev, iface, lun, &blocks, &blen) != 0) { log("usb bot: read capacity failed (lun %d, port %d)\n", lun, dev->port_id); return -1; }
    if (blocks == 0 || blen == 0 || blocks > (uint64_t)-1 / blen) { log("usb bot: invalid capacity (lun %d, port %d)\n", lun, dev->port_id); return -1; }
    uint64_t total = blocks * blen;
    int slot = alloc_bot_entry();
    if (slot < 0) { log("usb bot: no free device slots, dropping disk (lun %d, port %d)\n", lun, dev->port_id); return -1; }
    char disk_name[16];
    bool named = false;
    for (int i = 0; i < 26; i++) {
        char cand[16];
        if (!make_usb_bot_name(cand, sizeof(cand), i)) continue;
        uint64_t dummy = 0;
        if (get_block_device_size(cand, &dummy) == 0) continue;
        memcpy(disk_name, cand, sizeof(disk_name));
        named = true;
        break;
    }
    if (!named) { log("usb bot: no free disk names, dropping disk (lun %d, port %d)\n", lun, dev->port_id); return -1; }
    bot_entries[slot].dev = dev;
    bot_entries[slot].hcd = hcd;
    bot_entries[slot].interface_number = iface;
    bot_entries[slot].lun = lun;
    bot_entries[slot].block_size = blen;
    bot_entries[slot].block_count = blocks;
    bot_entries[slot].total_size = total;
    bot_entries[slot].present = true;
    memcpy(bot_entries[slot].name, disk_name, sizeof(disk_name));
    if (register_disk_device_idx(disk_name, read_usb_bot_device, write_usb_bot_device, slot, total, DISK_BUS_USB) < 0) {
        log("usb bot: failed to register disk (lun %d, port %d)\n", lun, dev->port_id);
        memset(&bot_entries[slot], 0, sizeof(bot_entries[slot]));
        return -1;
    }
    if (!probe_gpt_for_usb_disk(slot, disk_name, total)) probe_mbr_for_usb_disk(slot, disk_name, total);
    return slot;
}

int get_usb_bot_count(void) {
    int count = 0;
    for (int i = 0; i < USB_BOT_MAX_DEVICES; i++) { if (bot_entries[i].present) count++; }
    return count;
}

uint64_t get_usb_bot_size(int index) {
    if (index < 0 || index >= USB_BOT_MAX_DEVICES) return 0;
    if (!bot_entries[index].present) return 0;
    return bot_entries[index].total_size;
}

uint32_t get_usb_bot_block_size(int index) {
    if (index < 0 || index >= USB_BOT_MAX_DEVICES) return 0;
    if (!bot_entries[index].present) return 0;
    if (bot_entries[index].block_size != 512 && bot_entries[index].block_size != 1024 && bot_entries[index].block_size != 2048 && bot_entries[index].block_size != 4096) return 0;
    return bot_entries[index].block_size;
}

bool make_usb_bot_name(char *name, uint64_t size, int index) {
    if (!name || size < 4 || index < 0 || index >= 26) return false;
    name[0] = 's';
    name[1] = 'd';
    name[2] = (char)('a' + index);
    name[3] = '\0';
    return true;
}

uint64_t read_usb_bot_device(void *buf, uint64_t count, uint64_t offset, int index) {
    if (index < 0 || index >= USB_BOT_MAX_DEVICES || !bot_entries[index].present) return (uint64_t)-ENODEV;
    if (!count) return 0;
    if (!buf) return (uint64_t)-EINVAL;
    usb_bot_entry_t *entry = &bot_entries[index];
    uint64_t size = entry->total_size;
    uint32_t blen = entry->block_size;
    if (offset >= size) return 0;
    if (count > size - offset) count = size - offset;
    uint8_t *out = (uint8_t *)buf;
    uint64_t done = 0;
    uint8_t *tmp = malloc(blen);
    if (!tmp) return (uint64_t)-ENOMEM;
    while (done < count) {
        uint64_t pos = offset + done;
        uint64_t lba = pos / blen;
        uint32_t off = (uint32_t)(pos % blen);
        uint64_t left = count - done;
        if (off == 0 && left >= blen) {
            uint64_t blocks = left / blen;
            if (blocks > USB_BOT_MAX_BLOCKS_PER_CMD) blocks = USB_BOT_MAX_BLOCKS_PER_CMD;
            if (read_bot_blocks(entry, lba, (uint16_t)blocks, out + done) != 0) { free(tmp); return (uint64_t)-EIO; }
            done += blocks * blen;
            continue;
        }
        if (read_bot_blocks(entry, lba, 1, tmp) != 0) { free(tmp); return (uint64_t)-EIO; }
        uint64_t chunk = blen - off;
        if (chunk > left) chunk = left;
        memcpy(out + done, tmp + off, (size_t)chunk);
        done += chunk;
    }
    free(tmp);
    return count;
}

uint64_t write_usb_bot_device(const void *buf, uint64_t count, uint64_t offset, int index) {
    if (index < 0 || index >= USB_BOT_MAX_DEVICES || !bot_entries[index].present) return (uint64_t)-ENODEV;
    if (!count) return 0;
    if (!buf) return (uint64_t)-EINVAL;
    usb_bot_entry_t *entry = &bot_entries[index];
    uint64_t size = entry->total_size;
    uint32_t blen = entry->block_size;
    if (offset >= size) return (uint64_t)-ENOSPC;
    if (count > size - offset) count = size - offset;
    const uint8_t *in = (const uint8_t *)buf;
    uint64_t done = 0;
    uint8_t *tmp = malloc(blen);
    if (!tmp) return (uint64_t)-ENOMEM;
    while (done < count) {
        uint64_t pos = offset + done;
        uint64_t lba = pos / blen;
        uint32_t off = (uint32_t)(pos % blen);
        uint64_t left = count - done;
        if (off == 0 && left >= blen) {
            uint64_t blocks = left / blen;
            if (blocks > USB_BOT_MAX_BLOCKS_PER_CMD) blocks = USB_BOT_MAX_BLOCKS_PER_CMD;
            if (write_bot_blocks(entry, lba, (uint16_t)blocks, (uint8_t *)in + done) != 0) { free(tmp); return (uint64_t)-EIO; }
            done += blocks * blen;
            continue;
        }
        if (read_bot_blocks(entry, lba, 1, tmp) != 0) { free(tmp); return (uint64_t)-EIO; }
        uint64_t chunk = blen - off;
        if (chunk > left) chunk = left;
        memcpy(tmp + off, in + done, (size_t)chunk);
        if (write_bot_blocks(entry, lba, 1, tmp) != 0) { free(tmp); return (uint64_t)-EIO; }
        done += chunk;
    }
    free(tmp);
    return count;
}

void remove_usb_bot(usb_hcd_t *hcd, uint8_t port_id) {
    if (!hcd) return;
    usb_device_t *dev = NULL;
    for (int i = 0; i < USB_BOT_MAX_DEVICES; i++) {
        if (!bot_entries[i].present || bot_entries[i].hcd != hcd || !bot_entries[i].dev || bot_entries[i].dev->port_id != port_id) continue;
        remove_gpt_partitions(i, DISK_BUS_USB);
        remove_mbr_partitions(i, DISK_BUS_USB);
        unregister_device(bot_entries[i].name);
        dev = bot_entries[i].dev;
        memset(&bot_entries[i], 0, sizeof(bot_entries[i]));
    }
    if (dev) unregister_usb_device(dev);
}

void init_usb_bot(usb_hcd_t *hcd, uint8_t speed, uint8_t port_id) {
    if (!hcd || !hcd->control_transfer || !hcd->bulk_transfer) return;
    if (find_bot_entry(hcd, port_id) >= 0) return;
    usb_device_t *dev = usb_allocate_device();
    if (!dev) {
        log("usb bot: failed to allocate device slot\n");
        return;
    }

    uint8_t *desc_buf = malloc(64);
    uint8_t *cfg_buf = malloc(512);
    if (!desc_buf || !cfg_buf) {
        log("usb bot: failed to allocate descriptor buffers (port %d)\n", port_id);
        goto init_fail_dev;
    }

    memset(desc_buf, 0, 64);
    memset(cfg_buf, 0, 512);
    int new_address = usb_allocate_address(0);
    if (new_address < 0) { log("usb bot: no free device addresses left (port %d)\n", port_id); goto init_fail_dev; }
    dev->address = 0;
    dev->speed = speed;
    dev->max_packet_size = 64;
    dev->port_id = port_id;
    dev->interrupt_toggle = 0;
    dev->bulk_in_toggle = 0;
    dev->bulk_out_toggle = 0;
    usb_setup_packet_t setup;
    setup.bmRequestType = USB_REQTYPE_DIR_IN | USB_REQTYPE_STANDARD | USB_REQTYPE_DEVICE;
    setup.bRequest = USB_REQ_GET_DESCRIPTOR;
    setup.wValue = (USB_DESC_DEVICE << 8) | 0;
    setup.wIndex = 0;
    setup.wLength = 18;
    if (retry_control_transfer(hcd, dev, &setup, desc_buf, 18, 4, 10) < 0) {
        log("usb bot: failed to read initial descriptor (port %d)\n", port_id);
        goto init_fail_address;
    }
    uint8_t ep0_mps = desc_buf[7];
    if (ep0_mps != 8 && ep0_mps != 16 && ep0_mps != 32 && ep0_mps != 64) ep0_mps = 64;
    dev->max_packet_size = ep0_mps;
    setup.bmRequestType = USB_REQTYPE_DIR_OUT | USB_REQTYPE_STANDARD | USB_REQTYPE_DEVICE;
    setup.bRequest = USB_REQ_SET_ADDRESS;
    setup.wValue = (uint16_t)new_address;
    setup.wIndex = 0;
    setup.wLength = 0;
    if (retry_control_transfer(hcd, dev, &setup, NULL, 0, 3, 10) < 0) {
        log("usb bot: set_address failed (port %d)\n", port_id);
        goto init_fail_address;
    }
    dev->address = (uint8_t)new_address;
    sleep(20);
    memset(desc_buf, 0, 64);
    setup.bmRequestType = USB_REQTYPE_DIR_IN | USB_REQTYPE_STANDARD | USB_REQTYPE_DEVICE;
    setup.bRequest = USB_REQ_GET_DESCRIPTOR;
    setup.wValue = (USB_DESC_DEVICE << 8) | 0;
    setup.wIndex = 0;
    setup.wLength = 18;
    if (retry_control_transfer(hcd, dev, &setup, desc_buf, 18, 5, 20) < 0) {
        log("usb bot: failed to read device descriptor (port %d)\n", port_id);
        goto init_fail_dev;
    }
    usb_device_descriptor_t *ddev = (usb_device_descriptor_t *)desc_buf;
    dev->vendor_id = ddev->idVendor;
    dev->product_id = ddev->idProduct;
    memset(cfg_buf, 0, 512);
    setup.bmRequestType = USB_REQTYPE_DIR_IN | USB_REQTYPE_STANDARD | USB_REQTYPE_DEVICE;
    setup.bRequest = USB_REQ_GET_DESCRIPTOR;
    setup.wValue = (USB_DESC_CONFIGURATION << 8) | 0;
    setup.wIndex = 0;
    setup.wLength = 9;
    if (retry_control_transfer(hcd, dev, &setup, cfg_buf, 9, 4, 10) < 0) {
        log("usb bot: failed to read config header (port %d)\n", port_id);
        goto init_fail_dev;
    }
    usb_config_descriptor_t *dcfg = (usb_config_descriptor_t *)cfg_buf;
    uint16_t cfg_len = dcfg->wTotalLength;
    if (cfg_len < 9 || cfg_len > 512) {
        log("usb bot: bad config length %d (port %d)\n", cfg_len, port_id);
        goto init_fail_dev;
    }
    if (cfg_len > 9) {
        memset(cfg_buf, 0, 512);
        setup.wLength = cfg_len;
        if (retry_control_transfer(hcd, dev, &setup, cfg_buf, cfg_len, 4, 10) < 0) {
            log("usb bot: failed to read full config descriptor (port %d)\n", port_id);
            goto init_fail_dev;
        }
    }
    dcfg = (usb_config_descriptor_t *)cfg_buf;
    uint16_t total_len = dcfg->wTotalLength;
    if (total_len > cfg_len) total_len = cfg_len;
    uint8_t iface = 0;
    uint8_t in_ep = 0;
    uint8_t out_ep = 0;
    uint16_t in_mps = 0;
    uint16_t out_mps = 0;
    if (parse_bot_config(cfg_buf, total_len, dev->speed, &iface, &in_ep, &out_ep, &in_mps, &out_mps) < 0) {
        goto init_fail_dev;
    }
    dev->bulk_in_endpoint = in_ep;
    dev->bulk_out_endpoint = out_ep;
    dev->bulk_in_max_packet = in_mps;
    dev->bulk_out_max_packet = out_mps;
    dev->bulk_in_toggle = 0;
    dev->bulk_out_toggle = 0;
    dev->interface_number = iface;
    setup.bmRequestType = USB_REQTYPE_DIR_OUT | USB_REQTYPE_STANDARD | USB_REQTYPE_DEVICE;
    setup.bRequest = USB_REQ_SET_CONFIGURATION;
    setup.wValue = dcfg->bConfigurationValue;
    setup.wIndex = 0;
    setup.wLength = 0;
    if (hcd->control_transfer(hcd, dev, &setup, NULL, 0) < 0) {
        log("usb bot: set_configuration failed (port %d)\n", port_id);
        goto init_fail_dev;
    }
    sleep(100);
    int max_lun = get_max_lun(hcd, dev, iface);
    if (max_lun < 0) max_lun = 0;
    int found = 0;
    for (int lun = 0; lun <= max_lun && lun <= USB_BOT_MAX_LUN; lun++) {
        if (probe_bot_lun(hcd, dev, iface, (uint8_t)lun) >= 0) found++;
        else log("usb bot: lun %d not usable (port %d)\n", lun, port_id);
    }
    if (found == 0) {
        log("usb bot: no usable luns (port %d)\n", port_id);
        goto init_fail_dev;
    }
    free(desc_buf);
    free(cfg_buf);
    return;
init_fail_address:
    usb_release_address((uint8_t)new_address);
init_fail_dev:
    free(desc_buf);
    free(cfg_buf);
    unregister_usb_device(dev);
}
