#ifndef MICROPY_INCLUDED_CH32_FLASH_H
#define MICROPY_INCLUDED_CH32_FLASH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The filesystem occupies the flash tail. These are ABSOLUTE addresses: the
 * SDK flash driver validates against FLASH_BASE (0x08000000), so the 0x00000000
 * alias used by the linker scripts must not be passed to it.
 *
 *   0x08000000   64K  V3F boot stub
 *   0x08010000  640K  V5F MicroPython image
 *   0x080B0000  256K  FAT volume            <- this region
 *   0x080F0000        ValidAddrEnd_Dual, end of the 960 KB user area
 *
 * The volume gave up 128K to the image when the MP3 decoder went in. Moving
 * this boundary reformats the internal filesystem, because every sector of it
 * shifts; that is survivable here in a way it would not be on a board without
 * an SD card, which is where anything worth keeping now lives.
 *
 * CH32_FLASH_FS_BASE must equal the end of the FLASH region in the V5F linker
 * script, boards/CH32H417QEU6_V5F/ch32h417_v5f.ld. The two are separate
 * statements of one boundary and nothing checks them against each other at
 * build time, so changing either means changing both.
 *
 * That boundary used to sit at 0x08070000, giving the image 384K. mbedtls took
 * it to 410K, and the excess landed inside the volume: the first file written
 * put FAT sectors on top of the image's last 26K, and the board boot-looped on
 * the next reset with the V5F faulting before UART came up. The linker could
 * not warn because its FLASH region ran to the end of the chip and knew
 * nothing about the filesystem; it is now cut off here, so an image that no
 * longer fits fails the link instead.
 */
#define CH32_FLASH_FS_BASE      (0x080B0000u)
#define CH32_FLASH_FS_SIZE      (0x40000u)
#define CH32_FLASH_PAGE_SIZE    (0x2000u)     /* erase granularity at DBMODE=1 */
#define CH32_FLASH_BLOCK_SIZE   (512u)        /* logical sector seen by FAT */
#define CH32_FLASH_NUM_BLOCKS   (CH32_FLASH_FS_SIZE / CH32_FLASH_BLOCK_SIZE)

/* Erased flash reads as this on the CH32H417, NOT 0xFFFFFFFF. */
#define CH32_FLASH_ERASED_WORD  (0xE339E339u)

bool ch32_flash_erase_page(uint32_t addr);
bool ch32_flash_write(uint32_t addr, const void *src, size_t len);
void ch32_flash_read(uint32_t addr, void *dst, size_t len);
bool ch32_flash_page_is_erased(uint32_t addr);

/* Block device over the region above, exposed to Python as ch32.Flash(). */
#include "py/obj.h"
extern const mp_obj_type_t ch32_flash_type;
void ch32_flashbdev_init(void);

/* Block access used by both the Python ch32.Flash type and the USB MSC
 * callbacks, so the two paths cannot drift apart. All return true on success. */
bool ch32_flashbdev_read_blocks(uint8_t *dst, uint32_t block, uint32_t count);
bool ch32_flashbdev_write_blocks(const uint8_t *src, uint32_t block, uint32_t count);
bool ch32_flashbdev_flush(void);

#endif // MICROPY_INCLUDED_CH32_FLASH_H
