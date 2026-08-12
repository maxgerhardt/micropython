/* The `network.LAN` object.
 *
 * Deliberately holds no hardware knowledge: every register touch lives in
 * eth.c, and this file only translates between Python and that interface. The
 * split mirrors ports/stm32.
 */

#include "py/runtime.h"

#if MICROPY_PY_NETWORK_LAN

#include "extmod/modnetwork.h"
#include "lwip/netif.h"

#include "eth.h"

typedef struct _network_lan_obj_t {
    mp_obj_base_t base;
    eth_t *eth;
} network_lan_obj_t;

static const network_lan_obj_t network_lan_eth0 = { { &network_lan_type }, &eth_instance };

static void network_lan_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    network_lan_obj_t *self = MP_OBJ_TO_PTR(self_in);
    struct netif *netif = eth_netif(self->eth);
    mp_printf(print, "<LAN %u %u.%u.%u.%u>",
        eth_link_status(self->eth),
        netif->ip_addr.addr & 0xff,
        netif->ip_addr.addr >> 8 & 0xff,
        netif->ip_addr.addr >> 16 & 0xff,
        netif->ip_addr.addr >> 24);
}

/* Takes no phy_addr or phy_type: the PHY is on the die at a fixed SMI address,
 * so unlike an external-PHY port there is nothing here for a board to choose. */
static mp_obj_t network_lan_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    mp_arg_check_num(n_args, n_kw, 0, 0, false);

    const network_lan_obj_t *self = &network_lan_eth0;
    int ret = eth_init(self->eth);
    if (ret != 0) {
        mp_raise_OSError(-ret);
    }
    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t network_lan_active(size_t n_args, const mp_obj_t *args) {
    network_lan_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    if (n_args == 1) {
        // Whether the interface is enabled, not whether a cable is plugged in.
        return mp_obj_new_bool(eth_is_active(self->eth));
    }
    int ret = mp_obj_is_true(args[1]) ? eth_start(self->eth) : eth_stop(self->eth);
    if (ret < 0) {
        mp_raise_OSError(-ret);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(network_lan_active_obj, 1, 2, network_lan_active);

static mp_obj_t network_lan_isconnected(mp_obj_t self_in) {
    network_lan_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_bool(eth_link_status(self->eth) == 3);
}
static MP_DEFINE_CONST_FUN_OBJ_1(network_lan_isconnected_obj, network_lan_isconnected);

static mp_obj_t network_lan_ifconfig(size_t n_args, const mp_obj_t *args) {
    network_lan_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    return mod_network_nic_ifconfig(eth_netif(self->eth), n_args - 1, args + 1);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(network_lan_ifconfig_obj, 1, 2, network_lan_ifconfig);

static mp_obj_t network_lan_ipconfig(size_t n_args, const mp_obj_t *args, mp_map_t *kwargs) {
    network_lan_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    return mod_network_nic_ipconfig(eth_netif(self->eth), n_args - 1, args + 1, kwargs);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(network_lan_ipconfig_obj, 1, network_lan_ipconfig);

static mp_obj_t network_lan_status(size_t n_args, const mp_obj_t *args) {
    network_lan_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    if (n_args == 1) {
        return MP_OBJ_NEW_SMALL_INT(eth_link_status(self->eth));
    }
    mp_raise_ValueError(MP_ERROR_TEXT("unknown status param"));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(network_lan_status_obj, 1, 2, network_lan_status);

static mp_obj_t network_lan_config(size_t n_args, const mp_obj_t *args, mp_map_t *kwargs) {
    network_lan_obj_t *self = MP_OBJ_TO_PTR(args[0]);

    if (kwargs->used != 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("unknown config param"));
    }
    if (n_args != 2) {
        mp_raise_TypeError(MP_ERROR_TEXT("must query one param"));
    }

    switch (mp_obj_str_get_qstr(args[1])) {
        case MP_QSTR_mac:
            return mp_obj_new_bytes(&eth_netif(self->eth)->hwaddr[0], 6);
        default:
            mp_raise_ValueError(MP_ERROR_TEXT("unknown config param"));
    }
}
static MP_DEFINE_CONST_FUN_OBJ_KW(network_lan_config_obj, 1, network_lan_config);

static const mp_rom_map_elem_t network_lan_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_active), MP_ROM_PTR(&network_lan_active_obj) },
    { MP_ROM_QSTR(MP_QSTR_isconnected), MP_ROM_PTR(&network_lan_isconnected_obj) },
    { MP_ROM_QSTR(MP_QSTR_ifconfig), MP_ROM_PTR(&network_lan_ifconfig_obj) },
    { MP_ROM_QSTR(MP_QSTR_ipconfig), MP_ROM_PTR(&network_lan_ipconfig_obj) },
    { MP_ROM_QSTR(MP_QSTR_status), MP_ROM_PTR(&network_lan_status_obj) },
    { MP_ROM_QSTR(MP_QSTR_config), MP_ROM_PTR(&network_lan_config_obj) },
};
static MP_DEFINE_CONST_DICT(network_lan_locals_dict, network_lan_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    network_lan_type,
    MP_QSTR_LAN,
    MP_TYPE_FLAG_NONE,
    make_new, network_lan_make_new,
    print, network_lan_print,
    locals_dict, &network_lan_locals_dict
    );

#endif // MICROPY_PY_NETWORK_LAN
