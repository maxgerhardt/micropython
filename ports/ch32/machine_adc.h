/* The parts of machine_adc.c the rest of the port needs.
 *
 * machine_adc.c is pasted into extmod/machine_adc.c and is static throughout
 * apart from these two, which exist so machine_touchpad.c can share ADC1
 * rather than keeping a second copy of the channel table and a second copy of
 * the converter setup. The touch key peripheral *is* the ADC -- TKey1 and ADC1
 * are the same registers -- so there was never anything else to share it with.
 */
#ifndef MICROPY_INCLUDED_CH32_MACHINE_ADC_H
#define MICROPY_INCLUDED_CH32_MACHINE_ADC_H

#include <stdint.h>

#define MACHINE_ADC_CHANNEL_NONE (0xff)

/* The ADC input number for a pin, or MACHINE_ADC_CHANNEL_NONE. */
uint8_t machine_adc_channel_for_pin(uint8_t pin_id);

/* Bring ADC1 up if it is not already: clocks, a single-conversion
 * configuration and the one-time calibration. Idempotent. */
void machine_adc_init_hardware(void);

#endif // MICROPY_INCLUDED_CH32_MACHINE_ADC_H
