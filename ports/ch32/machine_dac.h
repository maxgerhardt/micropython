#ifndef MICROPY_INCLUDED_CH32_MACHINE_DAC_H
#define MICROPY_INCLUDED_CH32_MACHINE_DAC_H

#include "py/obj.h"

/* machine.DAC. Lives in its own compilation unit rather than in an
 * MICROPY_PY_MACHINE_*_INCLUDEFILE because extmod has no DAC of its own to
 * paste it into: the class is defined here in full and added to the machine
 * module by modmachine.c. */
extern const mp_obj_type_t machine_dac_type;

/* Stop both converters driving their pins. Called on soft reset, for the same
 * reason machine_pwm_deinit_all() is: the hardware outlives the object. */
void machine_dac_deinit_all(void);

#endif // MICROPY_INCLUDED_CH32_MACHINE_DAC_H
