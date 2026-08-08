/* USB mass storage over the internal flash filesystem.
 *
 * Follows ports/stm32/msc_disk.c: the callbacks go through the same block layer
 * the VFS uses, so both see one page cache and neither can hold a stale page.
 *
 * As on stm32 and rp2, there is deliberately NO arbitration between the host
 * and MicroPython. Both may write, and a host that has cached FAT structures
 * can overwrite changes it did not see. Use os.sync() and eject before
 * switching sides. This matches how a pyboard behaves; CircuitPython is the
 * one that makes the volume exclusive. */
#include "tusb.h"

#if CFG_TUD_MSC

#include <string.h>

#include "py/mpconfig.h"
#include "py/misc.h"

#include "flash.h"

static bool ejected = false;


void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16],
    uint8_t product_rev[4]) {
    (void)lun;
    const char *vid = MICROPY_HW_USB_MSC_INQUIRY_VENDOR_STRING;
    const char *pid = MICROPY_HW_USB_MSC_INQUIRY_PRODUCT_STRING;
    const char *rev = MICROPY_HW_USB_MSC_INQUIRY_REVISION_STRING;
    memcpy(vendor_id, vid, MIN(strlen(vid), 8));
    memcpy(product_id, pid, MIN(strlen(pid), 16));
    memcpy(product_rev, rev, MIN(strlen(rev), 4));
}

bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    if (ejected) {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3a, 0x00);
        return false;
    }
    return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size) {
    (void)lun;
    *block_count = CH32_FLASH_NUM_BLOCKS;
    *block_size = CH32_FLASH_BLOCK_SIZE;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start,
    bool load_eject) {
    (void)lun;
    (void)power_condition;
    if (load_eject) {
        ejected = !start;
        if (ejected) {
            /* The host is detaching; do not leave anything in the cache. */
            ch32_flashbdev_flush();
        }
    }
    return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer,
    uint32_t bufsize) {
    (void)lun;
    (void)offset;   /* CFG_TUD_MSC_BUFSIZE equals the block size, so always 0. */
    uint32_t count = bufsize / CH32_FLASH_BLOCK_SIZE;
    if (!ch32_flashbdev_read_blocks(buffer, lba, count)) {
        return -1;
    }
    return count * CH32_FLASH_BLOCK_SIZE;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
    uint8_t *buffer, uint32_t bufsize) {
    (void)lun;
    (void)offset;
    uint32_t count = bufsize / CH32_FLASH_BLOCK_SIZE;
    if (!ch32_flashbdev_write_blocks(buffer, lba, count)) {
        return -1;
    }
    return count * CH32_FLASH_BLOCK_SIZE;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer,
    uint16_t bufsize) {
    (void)buffer;
    (void)bufsize;
    switch (scsi_cmd[0]) {
        case SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL:
            /* The host is about to eject, or has finished writing. */
            ch32_flashbdev_flush();
            return 0;
        default:
            tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
            return -1;
    }
}

#endif // CFG_TUD_MSC
