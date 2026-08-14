/* VIO18 -- the supply behind most of this chip's I/O pads. */
#ifndef MICROPY_INCLUDED_CH32_MACHINE_VIO18_H
#define MICROPY_INCLUDED_CH32_MACHINE_VIO18_H

#include "py/obj.h"

/* Millivolts the LDO can be set to, plus 0 for "off". */
#define CH32_VIO18_OFF (0)
#define CH32_VIO18_1V2 (1200)
#define CH32_VIO18_1V8 (1800)
#define CH32_VIO18_2V5 (2500)
#define CH32_VIO18_3V3 (3300)

/* Called once at boot, before any pin is configured. */
void ch32_vio18_init(void);

/* Current setting, in millivolts; 0 if the rail is powered down. */
uint32_t ch32_vio18_get(void);

extern const mp_obj_fun_builtin_var_t machine_vio18_obj;

#endif // MICROPY_INCLUDED_CH32_MACHINE_VIO18_H
