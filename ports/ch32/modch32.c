/* The `ch32` module: board-specific objects that do not belong in `machine`. */
#include "py/runtime.h"
#include "flash.h"

#if MICROPY_HW_ENABLE_USBDEV
#include "tusb.h"
extern volatile uint32_t ch32_usbd_task_count;
extern volatile uint32_t ch32_usbd_irq_count;

/* (tud_task calls, USB interrupts, mounted, cdc connected, cdc write avail) */
static mp_obj_t ch32_usb_stat(void) {
    mp_obj_t t[5] = {
        mp_obj_new_int_from_uint(ch32_usbd_task_count),
        mp_obj_new_int_from_uint(ch32_usbd_irq_count),
        mp_obj_new_bool(tud_mounted()),
        mp_obj_new_bool(tud_cdc_connected()),
        mp_obj_new_int_from_uint(tud_cdc_write_available()),
    };
    return mp_obj_new_tuple(5, t);
}
static MP_DEFINE_CONST_FUN_OBJ_0(ch32_usb_stat_obj, ch32_usb_stat);

#endif

static const mp_rom_map_elem_t ch32_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_ch32) },
    { MP_ROM_QSTR(MP_QSTR_Flash),    MP_ROM_PTR(&ch32_flash_type) },
    #if MICROPY_HW_ENABLE_USBDEV
    { MP_ROM_QSTR(MP_QSTR_usb_stat), MP_ROM_PTR(&ch32_usb_stat_obj) },
    #endif
};
static MP_DEFINE_CONST_DICT(ch32_module_globals, ch32_module_globals_table);

const mp_obj_module_t mp_module_ch32 = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&ch32_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_ch32, mp_module_ch32);
