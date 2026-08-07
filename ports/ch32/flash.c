#include <string.h>

#include "ch32h417.h"
#include "flash.h"

/* Flash is memory-mapped for reads, so this never needs the controller. */
void ch32_flash_read(uint32_t addr, void *dst, size_t len) {
    memcpy(dst, (const void *)addr, len);
}

bool ch32_flash_page_is_erased(uint32_t addr) {
    const uint32_t *p = (const uint32_t *)addr;
    for (size_t i = 0; i < CH32_FLASH_PAGE_SIZE / 4; i++) {
        if (p[i] != CH32_FLASH_ERASED_WORD) {
            return false;
        }
    }
    return true;
}

bool ch32_flash_erase_page(uint32_t addr) {
    /* FLASH_ROM_ERASE rejects anything not aligned to the 8 KB erase unit. */
    if (addr % CH32_FLASH_PAGE_SIZE != 0) {
        return false;
    }
    FLASH_Unlock();
    FLASH_Status st = FLASH_ROM_ERASE(addr, CH32_FLASH_PAGE_SIZE);
    FLASH_Lock();
    return st == FLASH_COMPLETE;
}

bool ch32_flash_write(uint32_t addr, const void *src, size_t len) {
    FLASH_Unlock();
    /* The SDK takes a uint32_t* and writes whole words; callers in this port
     * always pass word-aligned buffers and word-multiple lengths. */
    FLASH_Status st = FLASH_ROM_WRITE(addr, (uint32_t *)src, len);
    FLASH_Lock();
    return st == FLASH_COMPLETE;
}
