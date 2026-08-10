/* The `ch32` module: board-specific objects that do not belong in `machine`. */
#include "py/runtime.h"
#include "flash.h"

#if MICROPY_PY_FRAMEBUF_ACCEL
#include "framebuf_accel.h"
#include "gpha.h"

/* Select which backend framebuf's fill and blit use: 0 generic, 1 C fast
 * paths, 2 GPHA where it wins. Exists so scripts/benchmark_framebuf.py can
 * measure all three in one image; called with no argument it just reports. */
static mp_obj_t ch32_framebuf_accel(size_t n_args, const mp_obj_t *args) {
    if (n_args > 0) {
        mp_int_t mode = mp_obj_get_int(args[0]);
        if (mode < FRAMEBUF_ACCEL_OFF || mode > FRAMEBUF_ACCEL_GPHA) {
            mp_raise_ValueError(MP_ERROR_TEXT("bad mode"));
        }
        framebuf_accel_set_mode((uint8_t)mode);
    }
    return mp_obj_new_int(framebuf_accel_get_mode());
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ch32_framebuf_accel_obj, 0, 1, ch32_framebuf_accel);

static mp_obj_t ch32_gpha_available(void) {
    return mp_obj_new_bool(gpha_available());
}
static MP_DEFINE_CONST_FUN_OBJ_0(ch32_gpha_available_obj, ch32_gpha_available);

static mp_obj_t ch32_gpha_ops(void) {
    return mp_obj_new_int_from_uint(framebuf_accel_gpha_ops());
}
static MP_DEFINE_CONST_FUN_OBJ_0(ch32_gpha_ops_obj, ch32_gpha_ops);
#endif

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
    #if MICROPY_PY_FRAMEBUF_ACCEL
    { MP_ROM_QSTR(MP_QSTR_framebuf_accel), MP_ROM_PTR(&ch32_framebuf_accel_obj) },
    { MP_ROM_QSTR(MP_QSTR_gpha_available), MP_ROM_PTR(&ch32_gpha_available_obj) },
    { MP_ROM_QSTR(MP_QSTR_gpha_ops), MP_ROM_PTR(&ch32_gpha_ops_obj) },
    #endif
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
