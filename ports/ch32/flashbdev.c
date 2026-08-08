/* FAT block device over the internal flash tail.
 *
 * FAT writes 512-byte sectors but the flash erase unit is 8 KB, so a sector
 * write is unavoidably read-modify-erase-program of a whole page. The page
 * buffer below exists for that, and it coalesces a run of sectors within one
 * write_blocks() call into a single erase.
 *
 * It is deliberately NOT a write-back cache. Dirty data never survives the
 * call that produced it: write_blocks() flushes before returning, so once any
 * block-device operation completes, flash matches what the filesystem thinks
 * it wrote.
 *
 * That costs write throughput, and it was originally written the other way --
 * holding a page dirty until something asked for a sync. Three things made
 * that untenable on this board:
 *
 *  - ch32_flashbdev_init() runs on every mount, including the remount after a
 *    soft reset, and simply dropped whatever was still dirty. run-tests.py
 *    soft-resets between tests, so a test that wrote to the filesystem could
 *    leave FAT half-updated. This is what corrupted the volume in practice.
 *  - USB MSC only flushed on eject or PREVENT_ALLOW_MEDIUM_REMOVAL. A host
 *    that wrote and never ejected left the data in RAM indefinitely.
 *  - A debugger reset (OpenOCD/wlink), which is how this board is reset all
 *    day, gives firmware no notice whatsoever. No amount of flushing at
 *    firmware-visible exit points can cover it.
 *
 * The residual window is inherent to NOR flash without a journal: a reset
 * between the erase and the reprogram leaves that one 8 KB page erased. It
 * cannot be closed without a log-structured format, and FAT is required here
 * because the volume is exposed over USB MSC.
 *
 * Reads bypass the buffer -- flash is memory-mapped -- except for a page held
 * dirty, which cannot happen between calls any more but is still handled so
 * that reads stay correct if deferred flushing is ever reintroduced. */
#include <string.h>

#include "py/runtime.h"
#include "py/mperrno.h"
#include "extmod/vfs.h"
#include "lib/oofatfs/ff.h"

#include "flash.h"

#define BLOCKS_PER_PAGE (CH32_FLASH_PAGE_SIZE / CH32_FLASH_BLOCK_SIZE)   /* 16 */
#define PAGE_NONE       (0xFFFFFFFFu)

static uint8_t flash_cache[CH32_FLASH_PAGE_SIZE];
static uint32_t cached_page = PAGE_NONE;   /* page index, not an address */
static bool cache_dirty;

static uint32_t page_addr(uint32_t page) {
    return CH32_FLASH_FS_BASE + page * CH32_FLASH_PAGE_SIZE;
}

static bool flash_cache_flush(void) {
    if (cached_page == PAGE_NONE || !cache_dirty) {
        return true;
    }
    uint32_t addr = page_addr(cached_page);
    /* Skip a rewrite that would change nothing. Flushing on every write makes
     * repeated writes of identical content common -- FAT rewrites its
     * allocation table and directory entries constantly -- and comparing 8 KB
     * of memory-mapped flash costs far less than an erase, in time and in
     * flash life. */
    if (memcmp((const void *)addr, flash_cache, CH32_FLASH_PAGE_SIZE) == 0) {
        cache_dirty = false;
        return true;
    }
    if (!ch32_flash_erase_page(addr)) {
        return false;
    }
    if (!ch32_flash_write(addr, flash_cache, CH32_FLASH_PAGE_SIZE)) {
        return false;
    }
    cache_dirty = false;
    return true;
}

static bool flash_cache_select(uint32_t page) {
    if (cached_page == page) {
        return true;
    }
    if (!flash_cache_flush()) {
        return false;
    }
    ch32_flash_read(page_addr(page), flash_cache, CH32_FLASH_PAGE_SIZE);
    cached_page = page;
    cache_dirty = false;
    return true;
}

void ch32_flashbdev_init(void) {
    /* Flush rather than discard. Nothing should be dirty here now that every
     * write flushes, but dropping staged data on a mount is precisely the bug
     * that used to corrupt the volume, so do not reintroduce it by assuming. */
    flash_cache_flush();
    cached_page = PAGE_NONE;
    cache_dirty = false;
}

bool ch32_flashbdev_read_blocks(uint8_t *dst, uint32_t block, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        uint32_t b = block + i;
        if (b >= CH32_FLASH_NUM_BLOCKS) {
            return false;
        }
        uint8_t *out = dst + i * CH32_FLASH_BLOCK_SIZE;
        uint32_t page = b / BLOCKS_PER_PAGE;
        if (page == cached_page && cache_dirty) {
            /* Unflushed data lives only in RAM; serve it from there. */
            memcpy(out, flash_cache + (b % BLOCKS_PER_PAGE) * CH32_FLASH_BLOCK_SIZE,
                CH32_FLASH_BLOCK_SIZE);
        } else {
            ch32_flash_read(CH32_FLASH_FS_BASE + b * CH32_FLASH_BLOCK_SIZE,
                out, CH32_FLASH_BLOCK_SIZE);
        }
    }
    return true;
}

bool ch32_flashbdev_write_blocks(const uint8_t *src, uint32_t block, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        uint32_t b = block + i;
        if (b >= CH32_FLASH_NUM_BLOCKS) {
            return false;
        }
        if (!flash_cache_select(b / BLOCKS_PER_PAGE)) {
            return false;
        }
        memcpy(flash_cache + (b % BLOCKS_PER_PAGE) * CH32_FLASH_BLOCK_SIZE,
            src + i * CH32_FLASH_BLOCK_SIZE, CH32_FLASH_BLOCK_SIZE);
        cache_dirty = true;
    }
    /* Commit before returning: no caller is required to sync, and the resets
     * that matter on this board cannot be intercepted. */
    return flash_cache_flush();
}

bool ch32_flashbdev_flush(void) {
    return flash_cache_flush();
}

/* FatFS timestamp callback. The board has an RTC but nothing sets it, so a
 * fixed date is more honest than a counter that restarts at every boot and
 * makes files appear to travel backwards in time.
 *
 * Planned: once Ethernet lands, sync the RTC over NTP and return the real time
 * from here. That is the point at which file timestamps become meaningful.
 *
 * FAT packing: bits 31:25 year-1980, 24:21 month, 20:16 day,
 *              15:11 hour, 10:5 minute, 4:0 seconds/2. */
DWORD get_fattime(void) {
    return ((DWORD)(2026 - 1980) << 25)   /* year  */
           | ((DWORD)1 << 21)             /* month */
           | ((DWORD)1 << 16);            /* day   */
}

/* --- Python-visible block device --- */

typedef struct _ch32_flash_obj_t {
    mp_obj_base_t base;
} ch32_flash_obj_t;

static const ch32_flash_obj_t ch32_flash_obj = {{&ch32_flash_type}};

static mp_obj_t ch32_flash_make_new(const mp_obj_type_t *type, size_t n_args,
    size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 0, 0, false);
    (void)type;
    (void)args;
    return MP_OBJ_FROM_PTR(&ch32_flash_obj);
}

static mp_obj_t ch32_flash_readblocks(mp_obj_t self, mp_obj_t block_num, mp_obj_t buf) {
    (void)self;
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(buf, &bufinfo, MP_BUFFER_WRITE);
    if (!ch32_flashbdev_read_blocks(bufinfo.buf, mp_obj_get_int(block_num),
        bufinfo.len / CH32_FLASH_BLOCK_SIZE)) {
        mp_raise_OSError(MP_EIO);
    }
    return MP_OBJ_NEW_SMALL_INT(0);
}
static MP_DEFINE_CONST_FUN_OBJ_3(ch32_flash_readblocks_obj, ch32_flash_readblocks);

static mp_obj_t ch32_flash_writeblocks(mp_obj_t self, mp_obj_t block_num, mp_obj_t buf) {
    (void)self;
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(buf, &bufinfo, MP_BUFFER_READ);
    if (!ch32_flashbdev_write_blocks(bufinfo.buf, mp_obj_get_int(block_num),
        bufinfo.len / CH32_FLASH_BLOCK_SIZE)) {
        mp_raise_OSError(MP_EIO);
    }
    return MP_OBJ_NEW_SMALL_INT(0);
}
static MP_DEFINE_CONST_FUN_OBJ_3(ch32_flash_writeblocks_obj, ch32_flash_writeblocks);

static mp_obj_t ch32_flash_ioctl(mp_obj_t self, mp_obj_t cmd_in, mp_obj_t arg_in) {
    (void)self;
    (void)arg_in;
    switch (mp_obj_get_int(cmd_in)) {
        case MP_BLOCKDEV_IOCTL_INIT:
            ch32_flashbdev_init();
            return MP_OBJ_NEW_SMALL_INT(0);
        case MP_BLOCKDEV_IOCTL_DEINIT:
        case MP_BLOCKDEV_IOCTL_SYNC:
            return MP_OBJ_NEW_SMALL_INT(ch32_flashbdev_flush() ? 0 : -1);
        case MP_BLOCKDEV_IOCTL_BLOCK_COUNT:
            return MP_OBJ_NEW_SMALL_INT(CH32_FLASH_NUM_BLOCKS);
        case MP_BLOCKDEV_IOCTL_BLOCK_SIZE:
            return MP_OBJ_NEW_SMALL_INT(CH32_FLASH_BLOCK_SIZE);
        default:
            return mp_const_none;
    }
}
static MP_DEFINE_CONST_FUN_OBJ_3(ch32_flash_ioctl_obj, ch32_flash_ioctl);

static const mp_rom_map_elem_t ch32_flash_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_readblocks),  MP_ROM_PTR(&ch32_flash_readblocks_obj) },
    { MP_ROM_QSTR(MP_QSTR_writeblocks), MP_ROM_PTR(&ch32_flash_writeblocks_obj) },
    { MP_ROM_QSTR(MP_QSTR_ioctl),       MP_ROM_PTR(&ch32_flash_ioctl_obj) },
};
static MP_DEFINE_CONST_DICT(ch32_flash_locals_dict, ch32_flash_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    ch32_flash_type,
    MP_QSTR_Flash,
    MP_TYPE_FLAG_NONE,
    make_new, ch32_flash_make_new,
    locals_dict, &ch32_flash_locals_dict
    );
