#ifndef MICROPY_INCLUDED_CH32_MACHINE_MEM_BACKUP_H
#define MICROPY_INCLUDED_CH32_MACHINE_MEM_BACKUP_H

/* Zero the backup region if it does not look like it survived a reset. Call
 * once at startup, outside the soft-reset loop -- calling it again per soft
 * reset would defeat the point. */
void ch32_mem_backup_init(void);

#endif // MICROPY_INCLUDED_CH32_MACHINE_MEM_BACKUP_H
