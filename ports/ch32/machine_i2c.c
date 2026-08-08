/* Hardware I2C for the CH32H417.
 *
 * The peripheral is the STM32F1-style one: STAR1/STAR2 status, CTLR1/CTLR2
 * control, a single DATAR, and the same awkward end-of-read sequences, where
 * ACK has to be cleared and STOP requested *before* the second-to-last byte is
 * read out. Those are the reason this file spells out the 1-byte, 2-byte and
 * n-byte cases separately rather than sharing one loop -- a naive loop ACKs one
 * byte too many and leaves the bus wedged.
 *
 * Alternate-function numbers differ per pin, not just per peripheral, so the
 * table below carries the AF with the pin pair.
 */
#include "ch32h417.h"

#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/runtime.h"
#include "extmod/modmachine.h"

#include "machine_pin.h"

#define I2C_DEFAULT_FREQ    (400000)
#define I2C_DEFAULT_TIMEOUT (50000)   /* microseconds for one byte-level step */

typedef struct _machine_i2c_obj_t {
    mp_obj_base_t base;
    I2C_TypeDef *i2c;
    uint8_t id;
    uint8_t scl;                      /* pin ids, as machine_pin.h encodes them */
    uint8_t sda;
    uint8_t af;
    uint32_t timeout_us;
} machine_i2c_obj_t;

/* Pin options taken from the datasheet's pin table. Only the pairs where both
 * pins belong to the same peripheral are listed; the first entry for a bus is
 * its default.
 *
 * A note for anyone adding to this: on this part PB6/PB7 are the only I2C pair
 * that sits in the VDDIO (3.3 V) domain. Everything else here is on VIO18,
 * which comes up at 1.8 V unless the board says otherwise, so those pins need
 * level shifting for an ordinary 3.3 V sensor. */
typedef struct _machine_i2c_pins_t {
    uint8_t id;
    uint8_t scl;
    uint8_t sda;
    uint8_t af;
} machine_i2c_pins_t;

#define PIN_ID(port, num) (((port) << 4) | (num))
#define PORT_A (0)
#define PORT_B (1)
#define PORT_C (2)
#define PORT_D (3)
#define PORT_F (5)

static const machine_i2c_pins_t machine_i2c_pin_options[] = {
    /* id  scl                 sda                 af */
    { 1, PIN_ID(PORT_B, 6),  PIN_ID(PORT_B, 7),  4 },   /* 3.3 V domain */
    { 1, PIN_ID(PORT_B, 8),  PIN_ID(PORT_B, 9),  4 },   /* also SWCLK/SWDIO */
    { 2, PIN_ID(PORT_B, 10), PIN_ID(PORT_B, 11), 4 },
    { 2, PIN_ID(PORT_C, 0),  PIN_ID(PORT_C, 1),  9 },
    { 3, PIN_ID(PORT_A, 8),  PIN_ID(PORT_C, 9),  4 },
    { 3, PIN_ID(PORT_A, 14), PIN_ID(PORT_A, 13), 7 },
    { 4, PIN_ID(PORT_D, 12), PIN_ID(PORT_D, 13), 4 },
    { 4, PIN_ID(PORT_F, 12), PIN_ID(PORT_F, 13), 2 },
};

static machine_i2c_obj_t machine_i2c_obj[4];

static I2C_TypeDef *const machine_i2c_regs[4] = { I2C1, I2C2, I2C3, I2C4 };

/* I2C1-3 hang off HB1 but I2C4 is on HB2, so this cannot be one table. */
static void machine_i2c_clock_enable(uint8_t id) {
    if (id == 4) {
        RCC_HB2PeriphClockCmd(RCC_HB2Periph_I2C4, ENABLE);
        return;
    }
    static const uint32_t hb1[3] = {
        RCC_HB1Periph_I2C1, RCC_HB1Periph_I2C2, RCC_HB1Periph_I2C3,
    };
    RCC_HB1PeriphClockCmd(hb1[id - 1], ENABLE);
}

/* Spin until every bit in `mask` reaches `set`, or the bus reports a fault.
 * Returns 0, -MP_ENODEV for a NACK, or -MP_ETIMEDOUT. */
static int i2c_wait(machine_i2c_obj_t *self, uint16_t mask, bool set) {
    uint64_t deadline = mp_hal_ticks_us() + self->timeout_us;
    for (;;) {
        uint16_t star1 = self->i2c->STAR1;
        if (star1 & I2C_STAR1_AF) {
            /* Nobody acknowledged. Clear the flag and let the caller stop. */
            self->i2c->STAR1 = (uint16_t) ~I2C_STAR1_AF;
            return -MP_ENODEV;
        }
        if (star1 & (I2C_STAR1_BERR | I2C_STAR1_ARLO)) {
            self->i2c->STAR1 = (uint16_t) ~(I2C_STAR1_BERR | I2C_STAR1_ARLO);
            return -MP_EIO;
        }
        if (((star1 & mask) == mask) == set) {
            return 0;
        }
        if (mp_hal_ticks_us() > deadline) {
            return -MP_ETIMEDOUT;
        }
    }
}

static int i2c_wait_not_busy(machine_i2c_obj_t *self) {
    uint64_t deadline = mp_hal_ticks_us() + self->timeout_us;
    while (self->i2c->STAR2 & I2C_STAR2_BUSY) {
        if (mp_hal_ticks_us() > deadline) {
            return -MP_EBUSY;
        }
    }
    return 0;
}

static void i2c_stop(machine_i2c_obj_t *self) {
    self->i2c->CTLR1 |= I2C_CTLR1_STOP;
}

/* Reading STAR1 then STAR2 is what clears ADDR; there is no write-1-to-clear
 * for it. The reads must not be optimised away. */
static void i2c_clear_addr(machine_i2c_obj_t *self) {
    volatile uint16_t tmp;
    tmp = self->i2c->STAR1;
    tmp = self->i2c->STAR2;
    (void)tmp;
}

static int i2c_start_and_address(machine_i2c_obj_t *self, uint16_t addr, bool read) {
    self->i2c->CTLR1 |= I2C_CTLR1_START;
    int ret = i2c_wait(self, I2C_STAR1_SB, true);
    if (ret != 0) {
        return ret;
    }
    self->i2c->DATAR = (uint16_t)((addr << 1) | (read ? 1 : 0));
    return i2c_wait(self, I2C_STAR1_ADDR, true);
}

static int machine_i2c_transfer_single(mp_obj_base_t *self_in, uint16_t addr,
    size_t len, uint8_t *buf, unsigned int flags) {
    machine_i2c_obj_t *self = (machine_i2c_obj_t *)self_in;
    I2C_TypeDef *i2c = self->i2c;
    bool read = flags & MP_MACHINE_I2C_FLAG_READ;
    bool stop = flags & MP_MACHINE_I2C_FLAG_STOP;
    int ret;

    ret = i2c_wait_not_busy(self);
    if (ret != 0) {
        return ret;
    }

    i2c->CTLR1 &= (uint16_t) ~I2C_CTLR1_POS;
    i2c->CTLR1 |= I2C_CTLR1_ACK;

    ret = i2c_start_and_address(self, addr, read);
    if (ret != 0) {
        /* A NACK here is how scan() discovers there is nothing at this
         * address, so it must not be treated as a bus fault. */
        i2c_stop(self);
        return ret;
    }

    if (!read) {
        i2c_clear_addr(self);
        for (size_t i = 0; i < len; i++) {
            ret = i2c_wait(self, I2C_STAR1_TXE, true);
            if (ret != 0) {
                i2c_stop(self);
                return ret;
            }
            i2c->DATAR = buf[i];
        }
        /* BTF means the last byte has actually left the shift register, so a
         * STOP here cannot truncate it. */
        ret = i2c_wait(self, I2C_STAR1_BTF, true);
        if (ret != 0 && len != 0) {
            i2c_stop(self);
            return ret;
        }
        if (stop) {
            i2c_stop(self);
        }
        return len;
    }

    /* Receive. ACK and STOP have to be set up ahead of the data actually
     * arriving, which is why the tail of the transfer is special-cased. */
    if (len == 0) {
        i2c_clear_addr(self);
        if (stop) {
            i2c_stop(self);
        }
        return 0;
    }

    if (len == 1) {
        i2c->CTLR1 &= (uint16_t) ~I2C_CTLR1_ACK;
        i2c_clear_addr(self);
        if (stop) {
            i2c_stop(self);
        }
        ret = i2c_wait(self, I2C_STAR1_RXNE, true);
        if (ret != 0) {
            return ret;
        }
        buf[0] = (uint8_t)i2c->DATAR;
        return 0;
    }

    if (len == 2) {
        /* POS makes ACK apply to the *next* byte, so both bytes can be sitting
         * in DATAR and the shift register with the NACK already placed. */
        i2c->CTLR1 |= I2C_CTLR1_POS;
        i2c->CTLR1 &= (uint16_t) ~I2C_CTLR1_ACK;
        i2c_clear_addr(self);
        ret = i2c_wait(self, I2C_STAR1_BTF, true);
        if (ret != 0) {
            i2c_stop(self);
            return ret;
        }
        if (stop) {
            i2c_stop(self);
        }
        buf[0] = (uint8_t)i2c->DATAR;
        buf[1] = (uint8_t)i2c->DATAR;
        i2c->CTLR1 &= (uint16_t) ~I2C_CTLR1_POS;
        return 0;
    }

    i2c_clear_addr(self);
    size_t i = 0;
    while (len - i > 3) {
        ret = i2c_wait(self, I2C_STAR1_RXNE, true);
        if (ret != 0) {
            i2c_stop(self);
            return ret;
        }
        buf[i++] = (uint8_t)i2c->DATAR;
    }
    /* Three left. Wait for BTF so that N-3 is in DATAR and N-2 is in the shift
     * register, then clear ACK before reading N-3 -- that is what makes the
     * controller NACK the final byte. */
    ret = i2c_wait(self, I2C_STAR1_BTF, true);
    if (ret != 0) {
        i2c_stop(self);
        return ret;
    }
    i2c->CTLR1 &= (uint16_t) ~I2C_CTLR1_ACK;
    buf[i++] = (uint8_t)i2c->DATAR;
    ret = i2c_wait(self, I2C_STAR1_BTF, true);
    if (ret != 0) {
        i2c_stop(self);
        return ret;
    }
    if (stop) {
        i2c_stop(self);
    }
    buf[i++] = (uint8_t)i2c->DATAR;
    buf[i++] = (uint8_t)i2c->DATAR;
    return 0;
}

/* Free a bus that someone left mid-transaction.
 *
 * A slave that was interrupted between bits -- unplugged, reset, or hot
 * swapped -- can sit holding SDA low forever, and then the controller reports
 * BUSY and every transfer times out. The way out is defined by the I2C spec:
 * drive up to nine clock pulses so the slave finishes shifting out whatever
 * byte it thought it was sending, then issue a STOP.
 *
 * Runs with the pins as plain open-drain outputs, before they are handed to
 * the peripheral's alternate function. */
static void machine_i2c_bus_recover(machine_i2c_obj_t *self) {
    GPIO_TypeDef *scl_gpio = machine_pin_gpio(self->scl);
    GPIO_TypeDef *sda_gpio = machine_pin_gpio(self->sda);
    uint16_t scl_mask = (uint16_t)(1u << (self->scl & 0xf));
    uint16_t sda_mask = (uint16_t)(1u << (self->sda & 0xf));

    GPIO_InitTypeDef init = {0};
    init.GPIO_Mode = GPIO_Mode_Out_OD;
    init.GPIO_Speed = GPIO_Speed_High;
    init.GPIO_Pin = scl_mask;
    GPIO_Init(scl_gpio, &init);
    init.GPIO_Pin = sda_mask;
    GPIO_Init(sda_gpio, &init);
    scl_gpio->BSHR = scl_mask;    /* release both lines */
    sda_gpio->BSHR = sda_mask;
    mp_hal_delay_us(10);

    for (int i = 0; i < 9 && !(sda_gpio->INDR & sda_mask); i++) {
        scl_gpio->BCR = scl_mask;
        mp_hal_delay_us(5);
        scl_gpio->BSHR = scl_mask;
        mp_hal_delay_us(5);
    }

    /* STOP: SDA rises while SCL is high. */
    sda_gpio->BCR = sda_mask;
    mp_hal_delay_us(5);
    scl_gpio->BSHR = scl_mask;
    mp_hal_delay_us(5);
    sda_gpio->BSHR = sda_mask;
    mp_hal_delay_us(10);
}

static void machine_i2c_init_pins(machine_i2c_obj_t *self) {
    machine_pin_clock_enable(self->scl);
    machine_pin_clock_enable(self->sda);
    RCC_HB2PeriphClockCmd(RCC_HB2Periph_AFIO, ENABLE);

    /* Free the bus first, while the pins are still ours to bit-bang. */
    machine_i2c_bus_recover(self);

    /* Open drain, because I2C is wired-AND: the bus is pulled up externally
     * and devices only ever pull it down. Push-pull here would fight the
     * pull-ups and break clock stretching. */
    GPIO_InitTypeDef init = {0};
    init.GPIO_Mode = GPIO_Mode_AF_OD;
    init.GPIO_Speed = GPIO_Speed_High;

    init.GPIO_Pin = (uint16_t)(1u << (self->scl & 0xf));
    GPIO_Init(machine_pin_gpio(self->scl), &init);
    init.GPIO_Pin = (uint16_t)(1u << (self->sda & 0xf));
    GPIO_Init(machine_pin_gpio(self->sda), &init);

    /* AF_OD selects "some alternate function"; the mux still has to be told
     * which one, and the number differs per pin. */
    GPIO_PinAFConfig(machine_pin_gpio(self->scl), self->scl & 0xf, self->af);
    GPIO_PinAFConfig(machine_pin_gpio(self->sda), self->sda & 0xf, self->af);
}

/* Lowest SCL the divider can express: CKCFGR's CCR field is 12 bits, so in
 * standard mode SCL >= SystemCoreClock / (2 * 4095), about 48.8 kHz at
 * 400 MHz. Requests below that are clamped; use SoftI2C for a slower bus. */
#define I2C_CCR_MAX (4095)

static void machine_i2c_init_periph(machine_i2c_obj_t *self, uint32_t freq) {
    machine_i2c_clock_enable(self->id);
    I2C_TypeDef *i2c = self->i2c;

    /* Reset the peripheral before reconfiguring. Its BUSY latch survives a
     * plain re-init, so constructing a second I2C object -- which is all
     * changing the frequency does -- could otherwise inherit a stuck-busy bus
     * from whatever the previous one was doing and fail with EBUSY. */
    i2c->CTLR1 |= I2C_CTLR1_SWRST;
    mp_hal_delay_us(10);
    i2c->CTLR1 &= (uint16_t) ~I2C_CTLR1_SWRST;

    /* Deliberately not the SDK's I2C_Init(). That derives the divider from
     * RCC's HCLK_Frequency, but this peripheral is fed by the system clock,
     * and on this board HCLK is SYSCLK >> 2. Using the vendor path produced an
     * SCL exactly 4.00x the requested rate -- measured at 50, 100, 200 and
     * 400 kHz against an SSD1306 -- which is far outside the I2C spec even
     * when a forgiving device happens to tolerate it. */
    uint32_t pclk = SystemCoreClock;

    i2c->CTLR1 &= (uint16_t) ~I2C_CTLR1_PE;

    /* FREQ tells the peripheral its own input clock, for the internal filters
     * and rise-time limit. The field tops out well below 400 MHz, so clamp it:
     * that costs timing margin, not the wrong bus frequency, because SCL comes
     * from CKCFGR below. */
    uint32_t freqrange = pclk / 1000000u;
    if (freqrange > 60) {
        freqrange = 60;
    }
    i2c->CTLR2 = (uint16_t)freqrange;

    uint32_t ccr;
    if (freq <= 100000) {
        ccr = pclk / (freq * 2);          /* SCL = pclk / (2 * CCR) */
        if (ccr < 4) {
            ccr = 4;
        }
        if (ccr > I2C_CCR_MAX) {
            ccr = I2C_CCR_MAX;
        }
        i2c->CKCFGR = (uint16_t)ccr;
        i2c->RTR = (uint16_t)(freqrange + 1);
    } else {
        ccr = pclk / (freq * 3);          /* fast mode, 2:1 duty */
        if (ccr < 1) {
            ccr = 1;
        }
        if (ccr > I2C_CCR_MAX) {
            ccr = I2C_CCR_MAX;
        }
        i2c->CKCFGR = (uint16_t)(0x8000u | ccr);   /* F/S bit */
        i2c->RTR = (uint16_t)(freqrange * 300u / 1000u + 1u);
    }

    i2c->OADDR1 = 0x4000;   /* bit 14 must read 1, per the reference manual */
    i2c->CTLR1 = I2C_CTLR1_PE | I2C_CTLR1_ACK;
}

static void machine_i2c_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    (void)kind;
    machine_i2c_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "I2C(%u, scl=P%c%u, sda=P%c%u)", self->id,
        'A' + (self->scl >> 4), self->scl & 0xf,
        'A' + (self->sda >> 4), self->sda & 0xf);
}

static mp_obj_t machine_i2c_make_new(const mp_obj_type_t *type, size_t n_args,
    size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_id, ARG_freq, ARG_scl, ARG_sda, ARG_timeout };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_id,      MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_freq,    MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = I2C_DEFAULT_FREQ} },
        { MP_QSTR_scl,     MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_sda,     MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_timeout, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = I2C_DEFAULT_TIMEOUT} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args,
        MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_int_t id = args[ARG_id].u_int;
    if (id < 1 || id > 4) {
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("I2C(%d) does not exist"), id);
    }

    /* Pick the pin pair: the bus default, or the requested one if it is a
     * combination the mux can actually produce. */
    const machine_i2c_pins_t *pins = NULL;
    for (size_t i = 0; i < MP_ARRAY_SIZE(machine_i2c_pin_options); i++) {
        const machine_i2c_pins_t *opt = &machine_i2c_pin_options[i];
        if (opt->id != id) {
            continue;
        }
        if (args[ARG_scl].u_obj == mp_const_none && args[ARG_sda].u_obj == mp_const_none) {
            pins = opt;
            break;
        }
        if (args[ARG_scl].u_obj != mp_const_none
            && machine_pin_get(args[ARG_scl].u_obj)->id != opt->scl) {
            continue;
        }
        if (args[ARG_sda].u_obj != mp_const_none
            && machine_pin_get(args[ARG_sda].u_obj)->id != opt->sda) {
            continue;
        }
        pins = opt;
        break;
    }
    if (pins == NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("those pins are not an I2C pair for this bus"));
    }

    machine_i2c_obj_t *self = &machine_i2c_obj[id - 1];
    self->base.type = type;
    self->i2c = machine_i2c_regs[id - 1];
    self->id = (uint8_t)id;
    self->scl = pins->scl;
    self->sda = pins->sda;
    self->af = pins->af;
    self->timeout_us = args[ARG_timeout].u_int;

    machine_i2c_init_pins(self);
    machine_i2c_init_periph(self, args[ARG_freq].u_int);
    return MP_OBJ_FROM_PTR(self);
}

static const mp_machine_i2c_p_t machine_i2c_p = {
    .transfer = mp_machine_i2c_transfer_adaptor,
    .transfer_single = machine_i2c_transfer_single,
};

MP_DEFINE_CONST_OBJ_TYPE(
    machine_i2c_type,
    MP_QSTR_I2C,
    MP_TYPE_FLAG_NONE,
    make_new, machine_i2c_make_new,
    print, machine_i2c_print,
    protocol, &machine_i2c_p,
    locals_dict, &mp_machine_i2c_locals_dict
    );
