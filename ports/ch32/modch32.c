/* The `ch32` module: board-specific objects that do not belong in `machine`. */
#include "py/runtime.h"
#include "flash.h"
#include "machine_wdt.h"
#include "mphalport.h"

/* Raw RCC_RSTSCKR from boot. machine.reset_cause() collapses causes that this
 * keeps separate -- notably pin reset, which the V5F's always-set software
 * reset flag would otherwise hide. */
static mp_obj_t ch32_reset_flags(void) {
    return mp_obj_new_int_from_uint(machine_wdt_reset_flags());
}
static MP_DEFINE_CONST_FUN_OBJ_0(ch32_reset_flags_obj, ch32_reset_flags);

/* (bytes of stack used at the deepest point since boot, bytes available).
 *
 * Worth checking after anything that recurses deeply -- lwip's receive path
 * inside a blocking socket call is the current record holder. The stack grows
 * down into the GC heap, so exceeding it corrupts objects rather than raising. */
static mp_obj_t ch32_stack_usage_py(void) {
    uint32_t used, total;
    ch32_stack_usage(&used, &total);
    mp_obj_t t[2] = {
        mp_obj_new_int_from_uint(used),
        mp_obj_new_int_from_uint(total),
    };
    return mp_obj_new_tuple(2, t);
}
static MP_DEFINE_CONST_FUN_OBJ_0(ch32_stack_usage_obj, ch32_stack_usage_py);

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

#if MICROPY_PY_MACHINE_I2S
/* The sample rate the SAI divider could actually produce, which is not always
 * the one asked for: MCKDIV is 6 bits, so the achievable rates are coarse.
 * Exposed because a few tenths of a percent off shows up as a pitch shift in
 * a recording and is otherwise invisible. */
uint32_t machine_i2s_actual_rate(mp_int_t i2s_id);

static mp_obj_t ch32_i2s_actual_rate(mp_obj_t id_in) {
    return mp_obj_new_int_from_uint(machine_i2s_actual_rate(mp_obj_get_int(id_in)));
}
static MP_DEFINE_CONST_FUN_OBJ_1(ch32_i2s_actual_rate_obj, ch32_i2s_actual_rate);

/* Run an I2S block as a bus slave, taking SCK and WS from its pins.
 *
 * machine.I2S has no argument for this because it models one master driving a
 * codec. The case that needs it is a loopback from this port's own I2S2 to its
 * I2S3, which is how the transmit side gets checked on a board with no I2S
 * input device -- two masters would drive SCK and WS against each other.
 *
 * Sticky per id, and consulted when the object is constructed, so arm it
 * before calling I2S(). */
void machine_i2s_set_slave(mp_int_t i2s_id, bool slave);

static mp_obj_t ch32_i2s_slave(mp_obj_t id_in, mp_obj_t slave_in) {
    machine_i2s_set_slave(mp_obj_get_int(id_in), mp_obj_is_true(slave_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(ch32_i2s_slave_obj, ch32_i2s_slave);
#endif

#if MICROPY_PY_NETWORK_LAN
#include "eth.h"

/* Ethernet bring-up and traffic counters in one tuple.
 *
 * This exists because "the link is down" has at least six distinct causes on
 * this part -- PLL off, MAC clock off, PHY still in reset or powered down, SMI
 * not answering, auto-negotiation unfinished, or no cable -- and they are
 * indistinguishable from Python without it. */
static mp_obj_t ch32_eth_diag(void) {
    eth_diag_t d;
    eth_get_diag(&d);
    const eth_stats_t *s = eth_get_stats(&eth_instance);

    mp_obj_t regs[14] = {
        mp_obj_new_int_from_uint(d.rcc_ctlr),
        mp_obj_new_int_from_uint(d.hbpcenr),
        mp_obj_new_int_from_uint(d.macphycr),
        mp_obj_new_int_from_uint(d.maccr),
        mp_obj_new_int_from_uint(d.dmasr),
        mp_obj_new_int_from_uint(d.dmaomr),
        mp_obj_new_int_from_uint(d.phy_id1),
        mp_obj_new_int_from_uint(d.phy_id2),
        mp_obj_new_int_from_uint(d.phy_bcr),
        mp_obj_new_int_from_uint(d.phy_bsr),
        mp_obj_new_int_from_uint(d.phy_anlpar),
        mp_obj_new_int_from_uint(d.phy_status),
        mp_obj_new_int_from_uint(d.lock_depth),
        mp_obj_new_int_from_uint(d.irq_enabled),
    };
    mp_obj_t stats[6] = {
        mp_obj_new_int_from_uint(s->rx_frames),
        mp_obj_new_int_from_uint(s->tx_frames),
        mp_obj_new_int_from_uint(s->rx_dropped),
        mp_obj_new_int_from_uint(s->rx_buf_unavail),
        mp_obj_new_int_from_uint(s->tx_errors),
        mp_obj_new_int_from_uint(s->link_changes),
    };

    mp_obj_t pair[2] = {
        mp_obj_new_tuple(14, regs),
        mp_obj_new_tuple(6, stats),
    };
    return mp_obj_new_tuple(2, pair);
}
static MP_DEFINE_CONST_FUN_OBJ_0(ch32_eth_diag_obj, ch32_eth_diag);
#endif

#if MICROPY_HW_ENABLE_USBDEV
#include "tusb.h"
extern volatile uint32_t ch32_usbd_task_count;
extern volatile uint32_t ch32_usbd_isr_task_count;
extern volatile uint32_t ch32_usbd_irq_count;

/* (tud_task calls, USB interrupts, mounted, cdc connected, cdc write avail,
   task runs driven from the ISR) */
static mp_obj_t ch32_usb_stat(void) {
    mp_obj_t t[6] = {
        mp_obj_new_int_from_uint(ch32_usbd_task_count),
        mp_obj_new_int_from_uint(ch32_usbd_irq_count),
        mp_obj_new_bool(tud_mounted()),
        mp_obj_new_bool(tud_cdc_connected()),
        mp_obj_new_int_from_uint(tud_cdc_write_available()),
        /* Appended, not inserted: scripts/check_usb.py indexes this tuple by
           position and inserting in the middle would silently move mounted and
           cdc_connected under it. */
        mp_obj_new_int_from_uint(ch32_usbd_isr_task_count),
    };
    return mp_obj_new_tuple(6, t);
}
static MP_DEFINE_CONST_FUN_OBJ_0(ch32_usb_stat_obj, ch32_usb_stat);

#endif

static const mp_rom_map_elem_t ch32_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_ch32) },
    { MP_ROM_QSTR(MP_QSTR_Flash),    MP_ROM_PTR(&ch32_flash_type) },
    { MP_ROM_QSTR(MP_QSTR_stack_usage), MP_ROM_PTR(&ch32_stack_usage_obj) },
    { MP_ROM_QSTR(MP_QSTR_reset_flags), MP_ROM_PTR(&ch32_reset_flags_obj) },
    #if MICROPY_PY_MACHINE_I2S
    { MP_ROM_QSTR(MP_QSTR_i2s_actual_rate), MP_ROM_PTR(&ch32_i2s_actual_rate_obj) },
    { MP_ROM_QSTR(MP_QSTR_i2s_slave), MP_ROM_PTR(&ch32_i2s_slave_obj) },
    #endif
    #if MICROPY_PY_FRAMEBUF_ACCEL
    { MP_ROM_QSTR(MP_QSTR_framebuf_accel), MP_ROM_PTR(&ch32_framebuf_accel_obj) },
    { MP_ROM_QSTR(MP_QSTR_gpha_available), MP_ROM_PTR(&ch32_gpha_available_obj) },
    { MP_ROM_QSTR(MP_QSTR_gpha_ops), MP_ROM_PTR(&ch32_gpha_ops_obj) },
    #endif
    #if MICROPY_PY_NETWORK_LAN
    { MP_ROM_QSTR(MP_QSTR_eth_diag), MP_ROM_PTR(&ch32_eth_diag_obj) },
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
