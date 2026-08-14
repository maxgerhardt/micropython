/* machine.vio18() -- the supply voltage of the high-speed I/O pads.
 *
 * Most of this chip's pins are not on VDDIO. They are on a second supply
 * called VIO18, and the pin table does not say which pin belongs to which:
 * the domains are colour-coded in the package drawing and nothing survives
 * PDF text extraction. So the port measured them, and the answer looked like
 * a hardware limitation -- I2C2, SPI2, I2S2, I2S3, SDMMC, LTDC and the FSMC
 * all sat at 1.2 V, far below what a 3.3 V part will read as a one.
 *
 * It is not a limitation. VIO18 is an LDO fed from VDDIO, and PWR_CTLR picks
 * its output:
 *
 *   [12:10] VSEL_VIO18   000 = 1.2 V, 001 = 1.8 V, 010 = 2.5 V, 011 = 3.3 V,
 *                        1xx = powered down
 *   [9]     VIO_SW_CR    1 = use VSEL_VIO18, 0 = use the power-up default,
 *                        which comes from the pull-down on XO (float = 1.8 V,
 *                        330k = 2.5 V, 100k = 1.2 V) and is 1.2 V here
 *
 * Measured on this board, driving each pad push-pull high and reading it back
 * through its own ADC channel:
 *
 *   VSEL      PC0     PC1     PC2     PC3   | PA5    PA6   (VDDIO, control)
 *   000     1.310   1.310   1.310   1.310   | 3.300  3.300
 *   001     1.821   1.820   1.820   1.819   | 3.300  3.300
 *   010     2.532   2.532   2.532   2.532   | 3.300  3.300
 *   011     3.300   3.300   3.300   3.300   | 3.300  3.300
 *
 * The port therefore selects 3.3 V at boot: a MicroPython user expects a pin
 * to drive 3.3 V, and every one of them now does. The cost is that anything
 * wired to a VIO18 pin sees 3.3 V rather than 1.2 V, so a board with 1.8 V
 * parts on those pins must call machine.vio18(1800) before using them.
 *
 * WCH's own note says that for permanent 3.3 V operation VIO18 should be
 * shorted to VDDIO on the board -- R11 next to the VIO18 pin is the
 * unpopulated 0R for exactly that -- and the LDO then disabled to save the
 * quiescent current. Running the LDO at 3.3 V from a 3.3 V input instead, as
 * this does, works and needs no soldering: it is a pass device with no
 * headroom, and it holds 3.300 V.
 */
#include "ch32h417.h"

#include "py/mphal.h"
#include "py/runtime.h"

#include "machine_vio18.h"

#define PWR_CTLR_VSEL_VIO18_Pos (10)
#define PWR_CTLR_VSEL_VIO18_Msk (7u << PWR_CTLR_VSEL_VIO18_Pos)
#define PWR_CTLR_VIO_SW_CR (1u << 9)

/* Selector values, indexed the same way as the table below. */
enum {
    VIO18_SEL_1V2 = 0,
    VIO18_SEL_1V8 = 1,
    VIO18_SEL_2V5 = 2,
    VIO18_SEL_3V3 = 3,
    VIO18_SEL_OFF = 7,
};

static const uint16_t vio18_mv[4] = {
    CH32_VIO18_1V2, CH32_VIO18_1V8, CH32_VIO18_2V5, CH32_VIO18_3V3,
};

static void vio18_set_sel(uint32_t sel) {
    PWR->CTLR = (PWR->CTLR & ~PWR_CTLR_VSEL_VIO18_Msk)
        | (sel << PWR_CTLR_VSEL_VIO18_Pos) | PWR_CTLR_VIO_SW_CR;
}

void ch32_vio18_init(void) {
    RCC_HB1PeriphClockCmd(RCC_HB1Periph_PWR, ENABLE);
    vio18_set_sel(VIO18_SEL_3V3);
    /* The rail is a few microfarads of board decoupling behind an LDO, and
     * this is the only place that raises it, so pay the settling time once
     * here rather than leaving the first pin write to find a half-risen pad. */
    mp_hal_delay_ms(2);
}

uint32_t ch32_vio18_get(void) {
    uint32_t ctlr = PWR->CTLR;
    uint32_t sel = (ctlr & PWR_CTLR_VSEL_VIO18_Msk) >> PWR_CTLR_VSEL_VIO18_Pos;
    if (!(ctlr & PWR_CTLR_VIO_SW_CR)) {
        /* Still on the hardware default. PWR_CSR reports what the XO
         * pull-down selected: 00 = 1.2 V, 01 = 2.5 V, 1x = 1.8 V -- an order
         * that is not VSEL's, so it cannot simply be passed through. */
        switch ((PWR->CSR >> 8) & 3) {
            case 0:
                return CH32_VIO18_1V2;
            case 1:
                return CH32_VIO18_2V5;
            default:
                return CH32_VIO18_1V8;
        }
    }
    return sel < 4 ? vio18_mv[sel] : CH32_VIO18_OFF;
}

static mp_obj_t machine_vio18(size_t n_args, const mp_obj_t *args) {
    if (n_args == 0) {
        return MP_OBJ_NEW_SMALL_INT(ch32_vio18_get());
    }
    mp_int_t mv = mp_obj_get_int(args[0]);
    if (mv == CH32_VIO18_OFF) {
        vio18_set_sel(VIO18_SEL_OFF);
        return mp_const_none;
    }
    for (size_t i = 0; i < MP_ARRAY_SIZE(vio18_mv); i++) {
        if (mv == vio18_mv[i]) {
            vio18_set_sel(i);
            mp_hal_delay_ms(2);
            return mp_const_none;
        }
    }
    mp_raise_ValueError(MP_ERROR_TEXT("vio18 must be 1200, 1800, 2500, 3300 or 0"));
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(machine_vio18_obj, 0, 1, machine_vio18);
