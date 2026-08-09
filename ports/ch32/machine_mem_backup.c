/* machine.mem_backup() for the CH32H417.
 *
 * This file is never compiled standalone; extmod/machine_mem.c includes it via
 * MICROPY_PY_MACHINE_MEM_BACKUP_INCLUDEFILE.
 *
 * The region is ordinary SRAM, not battery-backed storage, because this part
 * has none to offer. The datasheet mentions "the backup register" alongside the
 * RTC, but no such register block appears in the reference manual's register
 * tables, the SDK has no header for one, and probing the address STM32F1 uses
 * (0x40006C00) finds nothing writable -- twenty slots read back as zero after
 * being written, with PWR and BKP clocked and DBP set. The only thing in this
 * chip's backup power domain that holds a value is the RTC counter itself.
 *
 * So the guarantee here is the one esp32, rp2 and nrf give: contents survive a
 * soft reset and machine.reset(), and are lost when power goes away. Nothing
 * copies or clears this section -- that is what the separate NOLOAD section in
 * the linker script is for -- so a reset leaves it exactly as it was.
 *
 * After power-off it would otherwise come back as whatever the SRAM happened to
 * settle to, which is indistinguishable from real data. A magic word in front
 * of the user area turns that into a defined result: if it does not match, the
 * region is zeroed and the word written, so a cold boot reads zeros and a warm
 * one reads what was there.
 */
#include <stddef.h>
#include <string.h>

#include "machine_mem_backup.h"

extern uint8_t _mem_backup_start[];

/* Arbitrary, just unlikely to be what uninitialised SRAM settles to. */
#define CH32_MEM_BACKUP_MAGIC  (0x6B426843)
#define CH32_MEM_BACKUP_BYTES  (1024)
#define CH32_MEM_BACKUP_HEADER (8)

typedef struct _ch32_mem_backup_t {
    uint32_t magic;
    uint32_t reserved;                       /* keeps `data` 8-byte aligned */
    uint8_t data[CH32_MEM_BACKUP_BYTES];
} ch32_mem_backup_t;

MP_STATIC_ASSERT(offsetof(ch32_mem_backup_t, data) == CH32_MEM_BACKUP_HEADER);

void ch32_mem_backup_init(void) {
    ch32_mem_backup_t *mem = (ch32_mem_backup_t *)_mem_backup_start;
    if (mem->magic != CH32_MEM_BACKUP_MAGIC) {
        memset(mem->data, 0, sizeof(mem->data));
        mem->magic = CH32_MEM_BACKUP_MAGIC;
    }
}

/* Byte granularity: it is plain SRAM, so there is no word-write restriction to
 * force 'I' on callers the way a register-backed region would. */
static const mp_obj_array_t machine_mem_backup_regions[] = {
    BACKUP_MV('B', CH32_MEM_BACKUP_BYTES, (void *)(_mem_backup_start + CH32_MEM_BACKUP_HEADER)),
};
