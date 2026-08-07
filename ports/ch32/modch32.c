/* The `ch32` module: board-specific objects that do not belong in `machine`. */
#include "py/runtime.h"
#include "flash.h"

static const mp_rom_map_elem_t ch32_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_ch32) },
    { MP_ROM_QSTR(MP_QSTR_Flash),    MP_ROM_PTR(&ch32_flash_type) },
};
static MP_DEFINE_CONST_DICT(ch32_module_globals, ch32_module_globals_table);

const mp_obj_module_t mp_module_ch32 = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&ch32_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_ch32, mp_module_ch32);
