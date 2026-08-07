/* FAT block device over the internal flash tail.
 *
 * FAT writes 512-byte sectors but the flash erase unit is 8 KB, so a naive
 * implementation would erase and reprogram 8 KB for every sector write: 16x
 * write amplification, and FAT rewrites its allocation table constantly. A
 * single write-back page cache turns a run of sequential sector writes into
 * one erase per 8 KB page.
 *
 * Reads bypass the cache entirely -- flash is memory-mapped -- except for the
 * page currently held dirty, which must be served from RAM to stay coherent. */
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
    cached_page = PAGE_NONE;
    cache_dirty = false;
}

/* FatFS timestamp callback. The board has an RTC but nothing sets it, so a
 * fixed date is more honest than a counter that restarts at every boot and
 * makes files appear to travel backwards in time. Wire this to the RTC once
 * there is a way to set the clock.
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
    uint32_t block = mp_obj_get_int(block_num);
    size_t nblocks = bufinfo.len / CH32_FLASH_BLOCK_SIZE;

    for (size_t i = 0; i < nblocks; i++) {
        uint32_t b = block + i;
        uint8_t *dst = (uint8_t *)bufinfo.buf + i * CH32_FLASH_BLOCK_SIZE;
        uint32_t page = b / BLOCKS_PER_PAGE;
        if (page == cached_page && cache_dirty) {
            /* Unflushed data lives only in RAM; serve it from there. */
            memcpy(dst, flash_cache + (b % BLOCKS_PER_PAGE) * CH32_FLASH_BLOCK_SIZE,
                CH32_FLASH_BLOCK_SIZE);
        } else {
            ch32_flash_read(CH32_FLASH_FS_BASE + b * CH32_FLASH_BLOCK_SIZE,
                dst, CH32_FLASH_BLOCK_SIZE);
        }
    }
    return MP_OBJ_NEW_SMALL_INT(0);
}
static MP_DEFINE_CONST_FUN_OBJ_3(ch32_flash_readblocks_obj, ch32_flash_readblocks);

static mp_obj_t ch32_flash_writeblocks(mp_obj_t self, mp_obj_t block_num, mp_obj_t buf) {
    (void)self;
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(buf, &bufinfo, MP_BUFFER_READ);
    uint32_t block = mp_obj_get_int(block_num);
    size_t nblocks = bufinfo.len / CH32_FLASH_BLOCK_SIZE;

    for (size_t i = 0; i < nblocks; i++) {
        uint32_t b = block + i;
        if (b >= CH32_FLASH_NUM_BLOCKS) {
            mp_raise_OSError(MP_EINVAL);
        }
        if (!flash_cache_select(b / BLOCKS_PER_PAGE)) {
            mp_raise_OSError(MP_EIO);
        }
        memcpy(flash_cache + (b % BLOCKS_PER_PAGE) * CH32_FLASH_BLOCK_SIZE,
            (const uint8_t *)bufinfo.buf + i * CH32_FLASH_BLOCK_SIZE,
            CH32_FLASH_BLOCK_SIZE);
        cache_dirty = true;
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
            return MP_OBJ_NEW_SMALL_INT(flash_cache_flush() ? 0 : -1);
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
