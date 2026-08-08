/* Hardware SPI for the CH32H417.
 *
 * The peripheral is the familiar STM32 one -- CTLR1/CTLR2, a status register
 * (STATR here, not SR), one DATAR shared by transmit and receive -- so the
 * transfer loop is the classic "push a byte, pull a byte" full-duplex shift.
 * There is no FIFO: every write is answered by exactly one read, and the
 * received byte must be collected before the next one arrives or OVR is set
 * and the data is lost. This driver therefore never runs ahead of the receiver.
 *
 * Chip select is deliberately not handled. Hardware NSS ties one bus to one
 * device and gets in the way the moment there are two, so MicroPython's
 * convention everywhere is that the caller drives CS with an ordinary Pin. The
 * peripheral runs with software slave management (SSM=1, SSI=1), which is also
 * what keeps a master out of mode-fault when NSS floats.
 *
 * Alternate-function numbers differ per pin, not just per peripheral, so each
 * table below carries the AF alongside the pin.
 */
#include "ch32h417.h"

#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/runtime.h"
#include "extmod/modmachine.h"

#include "machine_pin.h"

#define SPI_DEFAULT_BAUDRATE  (1000000)
#define SPI_DEFAULT_POLARITY  (0)
#define SPI_DEFAULT_PHASE     (0)
#define SPI_DEFAULT_BITS      (8)
#define SPI_DEFAULT_FIRSTBIT  (MICROPY_PY_MACHINE_SPI_MSB)

/* One byte at the slowest possible clock (HCLK/256, so ~390 kHz) takes about
 * 21 us. This is three orders of magnitude above that: it is here to turn a
 * wedged peripheral into an exception instead of a hang, not to police
 * timing. */
#define SPI_TIMEOUT_US        (20000)

typedef struct _machine_spi_obj_t {
    mp_obj_base_t base;
    SPI_TypeDef *spi;
    uint8_t id;
    uint8_t sck;                      /* pin ids, as machine_pin.h encodes them */
    uint8_t miso;
    uint8_t mosi;
    uint8_t polarity;
    uint8_t phase;
    uint8_t firstbit;
    uint8_t prescaler_log2;           /* SCK = HCLK >> (prescaler_log2 + 1) */
    uint32_t baudrate;                /* the rate actually achieved */
} machine_spi_obj_t;

/* Which pins each signal can come out on, and with which alternate function.
 *
 * Kept as three independent tables rather than fixed triples because the mux
 * combines them freely: any SCK option for a bus works with any MISO and MOSI
 * option for that bus. The first entry for a bus is its default.
 *
 * Domain note, the same one that applies to I2C: SPI1 on PA5/PA6/PA7 and SPI4
 * on PE2/PE5/PE6 sit in the 3.3 V supply domains. Most of the rest is on
 * VIO18, which comes up well below 3.3 V, so a 3.3 V peripheral wired straight
 * to those pins needs level shifting. The datasheet distinguishes the domains
 * only by colour in the package figure, so this came from measurement: driving
 * PA5, PA6 and PA7 high and reading each back through the ADC gives 3.299 V.
 *
 * Some entries collide with functions this board already uses -- SPI2_SCK on
 * PA9 is the REPL's UART TX, on PA12 it is USB D+ -- so they are listed last
 * and are only ever selected if the caller asks for them by name. */
typedef struct _machine_spi_pin_t {
    uint8_t id;
    uint8_t pin;
    uint8_t af;
} machine_spi_pin_t;

#define PIN_ID(port, num) (((port) << 4) | (num))
#define PORT_A (0)
#define PORT_B (1)
#define PORT_C (2)
#define PORT_D (3)
#define PORT_E (4)
#define PORT_F (5)

static const machine_spi_pin_t machine_spi_sck_options[] = {
    { 1, PIN_ID(PORT_A, 5),  5 },   /* 3.3 V domain */
    { 1, PIN_ID(PORT_B, 3),  5 },
    { 1, PIN_ID(PORT_F, 5),  5 },
    { 1, PIN_ID(PORT_F, 7),  3 },
    { 2, PIN_ID(PORT_B, 13), 5 },
    { 2, PIN_ID(PORT_B, 10), 5 },
    { 2, PIN_ID(PORT_C, 2),  5 },
    { 2, PIN_ID(PORT_D, 3),  5 },
    { 2, PIN_ID(PORT_E, 14), 5 },
    { 2, PIN_ID(PORT_A, 9),  5 },   /* also USART1 TX */
    { 2, PIN_ID(PORT_A, 12), 5 },   /* also USB D+ */
    { 3, PIN_ID(PORT_B, 3),  6 },
    { 3, PIN_ID(PORT_C, 10), 6 },
    { 3, PIN_ID(PORT_A, 14), 1 },
    { 4, PIN_ID(PORT_E, 2),  5 },   /* 3.3 V domain */
    { 4, PIN_ID(PORT_E, 12), 5 },
};

static const machine_spi_pin_t machine_spi_miso_options[] = {
    { 1, PIN_ID(PORT_A, 6),  5 },   /* 3.3 V domain */
    { 1, PIN_ID(PORT_B, 4),  5 },
    { 1, PIN_ID(PORT_F, 3),  5 },
    { 1, PIN_ID(PORT_F, 9),  3 },
    { 2, PIN_ID(PORT_B, 14), 5 },
    { 2, PIN_ID(PORT_C, 2),  5 },
    { 3, PIN_ID(PORT_C, 11), 6 },
    { 3, PIN_ID(PORT_B, 4),  6 },
    { 3, PIN_ID(PORT_C, 9),  5 },
    { 4, PIN_ID(PORT_E, 5),  5 },   /* 3.3 V domain */
    { 4, PIN_ID(PORT_E, 13), 5 },
};

static const machine_spi_pin_t machine_spi_mosi_options[] = {
    { 1, PIN_ID(PORT_A, 7),  5 },   /* 3.3 V domain */
    { 1, PIN_ID(PORT_B, 5),  5 },
    { 1, PIN_ID(PORT_D, 7),  5 },
    { 1, PIN_ID(PORT_F, 8),  3 },
    { 2, PIN_ID(PORT_B, 15), 5 },
    { 2, PIN_ID(PORT_C, 3),  5 },
    { 2, PIN_ID(PORT_C, 1),  5 },
    { 3, PIN_ID(PORT_C, 12), 6 },
    { 3, PIN_ID(PORT_B, 5),  7 },
    { 3, PIN_ID(PORT_B, 2),  7 },
    { 3, PIN_ID(PORT_D, 6),  5 },
    { 3, PIN_ID(PORT_A, 13), 1 },
    { 4, PIN_ID(PORT_E, 6),  5 },   /* 3.3 V domain */
    { 4, PIN_ID(PORT_E, 14), 5 },
};

static machine_spi_obj_t machine_spi_obj[4];

static SPI_TypeDef *const machine_spi_regs[4] = { SPI1, SPI2, SPI3, SPI4 };

/* SPI1 hangs off HB2 but SPI2-4 are on HB1, so this cannot be one table.
 * (Note that the split is the other way round from I2C, where 1-3 are on HB1
 * and only I2C4 is on HB2.) */
static void machine_spi_clock_enable(uint8_t id) {
    if (id == 1) {
        RCC_HB2PeriphClockCmd(RCC_HB2Periph_SPI1, ENABLE);
        return;
    }
    static const uint32_t hb1[3] = {
        RCC_HB1Periph_SPI2, RCC_HB1Periph_SPI3, RCC_HB1Periph_SPI4,
    };
    RCC_HB1PeriphClockCmd(hb1[id - 2], ENABLE);
}

static void machine_spi_reset(uint8_t id) {
    if (id == 1) {
        RCC_HB2PeriphResetCmd(RCC_HB2Periph_SPI1, ENABLE);
        RCC_HB2PeriphResetCmd(RCC_HB2Periph_SPI1, DISABLE);
        return;
    }
    static const uint32_t hb1[3] = {
        RCC_HB1Periph_SPI2, RCC_HB1Periph_SPI3, RCC_HB1Periph_SPI4,
    };
    RCC_HB1PeriphResetCmd(hb1[id - 2], ENABLE);
    RCC_HB1PeriphResetCmd(hb1[id - 2], DISABLE);
}

/* SPI runs off HCLK. Ask RCC for it rather than using SystemCoreClock, which on
 * this part is the 400 MHz V5F core clock and four times too high -- the same
 * mistake that once had the whole timebase running 4x slow. */
static uint32_t machine_spi_periph_clock(void) {
    RCC_ClocksTypeDef clocks;
    RCC_GetClocksFreq(&clocks);
    return clocks.HCLK_Frequency;
}

/* The divider is a power of two from 2 to 256 (BR = 0..7), so an arbitrary
 * baudrate cannot be hit exactly. Round the clock *down* to the next available
 * step: a device with a documented maximum SCK must not be overclocked because
 * the arithmetic happened to land one step high.
 *
 * The one case that cannot honour that is a request below HCLK/256, about
 * 390 kHz, where the slowest available clock is still faster than asked. This
 * clamps rather than raising, because the upstream test suite asks for 100 kHz
 * and every other port clamps too -- but it does mean a request under 390 kHz
 * gets a *faster* bus than requested, so print(spi) is the only honest answer
 * and SoftSPI is the way to go slower. */
static uint8_t machine_spi_prescaler_for(uint32_t hclk, uint32_t baudrate) {
    for (uint8_t br = 0; br < 7; br++) {
        if ((hclk >> (br + 1)) <= baudrate) {
            return br;
        }
    }
    return 7;
}

/* Configure one pin as an alternate function of the SPI block.
 *
 * All three signals get AF push-pull, including MISO, which is an input. That
 * looks wrong and is not: on this part the alternate-function mux owns the
 * pad's output enable, so a pin whose selected function does not drive is not
 * driven. uart.c does exactly this for USART1 RX and has worked since the first
 * milestone. Configuring MISO as a plain floating input instead would be the
 * STM32F1 idiom, but this chip has an F4-style per-pin AF mux bolted onto
 * F1-style config registers, and in that arrangement the input only reaches the
 * peripheral once the mux is pointed at it. */
static void machine_spi_init_pin(uint8_t pin, uint8_t af) {
    machine_pin_clock_enable(pin);
    /* GPIO_PinAFConfig writes land in the AFIO block and are silently dropped
     * while its clock is off. */
    RCC_HB2PeriphClockCmd(RCC_HB2Periph_AFIO, ENABLE);

    GPIO_InitTypeDef init = {0};
    init.GPIO_Mode = GPIO_Mode_AF_PP;
    /* SCK can run at 50 MHz. A slow slew rate would round the edges into
     * unusable clocks well before that. */
    init.GPIO_Speed = GPIO_Speed_Very_High;
    init.GPIO_Pin = (uint16_t)(1u << MACHINE_PIN_NUM(pin));
    GPIO_Init(machine_pin_gpio(pin), &init);

    GPIO_PinAFConfig(machine_pin_gpio(pin), MACHINE_PIN_NUM(pin), af);
}

static void machine_spi_init_periph(machine_spi_obj_t *self) {
    machine_spi_clock_enable(self->id);

    /* Reset before reconfiguring. CTLR1 fields are documented as
     * "cannot be modified during communication", and re-running init on a bus
     * that is mid-transfer -- which is all that changing the baudrate does --
     * would otherwise leave the peripheral in a state the datasheet does not
     * define. */
    machine_spi_reset(self->id);

    SPI_InitTypeDef init = {0};
    init.SPI_Direction = SPI_Direction_2Lines_FullDuplex;
    init.SPI_Mode = SPI_Mode_Master;
    init.SPI_DataSize = SPI_DataSize_8b;
    init.SPI_CPOL = self->polarity ? SPI_CPOL_High : SPI_CPOL_Low;
    init.SPI_CPHA = self->phase ? SPI_CPHA_2Edge : SPI_CPHA_1Edge;
    /* Software slave management. Without it a master watching its NSS pin go
     * low decides another master has taken the bus, clears MSTR and stops. */
    init.SPI_NSS = SPI_NSS_Soft;
    init.SPI_BaudRatePrescaler = (uint16_t)(self->prescaler_log2 << 3);
    init.SPI_FirstBit = (self->firstbit == MICROPY_PY_MACHINE_SPI_LSB)
        ? SPI_FirstBit_LSB : SPI_FirstBit_MSB;
    init.SPI_CRCPolynomial = 7;

    SPI_Cmd(self->spi, DISABLE);
    SPI_Init(self->spi, &init);
    SPI_Cmd(self->spi, ENABLE);
}

/* Spin until `mask` is set in STATR. Returns 0 or -MP_ETIMEDOUT. */
static int machine_spi_wait(machine_spi_obj_t *self, uint16_t mask) {
    uint64_t deadline = mp_hal_ticks_us() + SPI_TIMEOUT_US;
    while (!(self->spi->STATR & mask)) {
        if (mp_hal_ticks_us() > deadline) {
            return -MP_ETIMEDOUT;
        }
    }
    return 0;
}

static void machine_spi_transfer(mp_obj_base_t *self_in, size_t len,
    const uint8_t *src, uint8_t *dest) {
    machine_spi_obj_t *self = (machine_spi_obj_t *)self_in;
    SPI_TypeDef *spi = self->spi;

    /* Drain anything a previous aborted transfer left behind, so a stale byte
     * cannot be handed back as if it belonged to this one. Reading DATAR is
     * what clears RXNE; reading STATR afterwards clears OVR. */
    if (spi->STATR & SPI_STATR_RXNE) {
        (void)spi->DATAR;
    }
    (void)spi->STATR;

    if (dest == NULL) {
        /* Write-only. Nothing is going to look at the received bytes, so let
         * them overrun: OVR affects only the receive buffer, transmission
         * carries on regardless, and the prologue above clears it before the
         * next transfer. Halving the per-byte work buys no extra throughput at
         * rates the full-duplex path already keeps up with -- both reach the
         * line rate -- but it is what lets write() keep working at HCLK/2,
         * where collecting each byte in time is impossible. */
        for (size_t i = 0; i < len; i++) {
            if (machine_spi_wait(self, SPI_STATR_TXE) != 0) {
                mp_raise_OSError(MP_ETIMEDOUT);
            }
            spi->DATAR = src != NULL ? src[i] : 0;
        }
    } else {
        /* Full duplex. Keep one byte in flight ahead of the one being
         * collected: the transmit buffer is one deep, so loading the next byte
         * while the current one is still shifting is what keeps SCK running
         * continuously instead of stalling for a round trip per byte.
         *
         * Never more than one ahead, though. The receive buffer is also one
         * deep, and a second completed byte arriving before the first is read
         * sets OVR and discards it. */
        size_t tx = 0, rx = 0;
        uint64_t deadline = mp_hal_ticks_us() + SPI_TIMEOUT_US;
        while (rx < len) {
            /* An overrun here is not a transient glitch to be retried: the
             * received byte is gone, and until OVR is cleared the receive
             * buffer stops updating altogether, so the loop below would wait
             * for an RXNE that can never arrive and report a timeout instead
             * of the real cause. It means the bus is clocking faster than this
             * CPU can collect bytes -- at HCLK/2 a byte is 160 ns, which no
             * polling loop reaching across the bus matrix can service. Clear
             * it (read DATAR, then STATR) and say so. */
            if (spi->STATR & SPI_STATR_OVR) {
                (void)spi->DATAR;
                (void)spi->STATR;
                mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT(
                    "SPI receive overrun: baudrate too high for full duplex"));
            }
            if (tx < len && tx - rx < 2 && (spi->STATR & SPI_STATR_TXE)) {
                /* SPI.write() passes no source buffer; send zeros then, which
                 * is also what SPI.read() defaults to sending. */
                spi->DATAR = src != NULL ? src[tx] : 0;
                tx++;
                deadline = mp_hal_ticks_us() + SPI_TIMEOUT_US;
            }
            if (spi->STATR & SPI_STATR_RXNE) {
                dest[rx] = (uint8_t)spi->DATAR;
                rx++;
                deadline = mp_hal_ticks_us() + SPI_TIMEOUT_US;
            }
            if (mp_hal_ticks_us() > deadline) {
                mp_raise_OSError(MP_ETIMEDOUT);
            }
        }
    }

    /* Do not return while SCK is still moving: the caller's next act is
     * usually to raise CS, and doing that mid-byte truncates the frame. */
    uint64_t deadline = mp_hal_ticks_us() + SPI_TIMEOUT_US;
    while (spi->STATR & SPI_STATR_BSY) {
        if (mp_hal_ticks_us() > deadline) {
            mp_raise_OSError(MP_ETIMEDOUT);
        }
    }
}

static void machine_spi_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    (void)kind;
    machine_spi_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print,
        "SPI(%u, baudrate=%u, polarity=%u, phase=%u, bits=8, firstbit=%u, "
        "sck=P%c%u, mosi=P%c%u, miso=P%c%u)",
        self->id, self->baudrate, self->polarity, self->phase, self->firstbit,
        'A' + MACHINE_PIN_PORT(self->sck), MACHINE_PIN_NUM(self->sck),
        'A' + MACHINE_PIN_PORT(self->mosi), MACHINE_PIN_NUM(self->mosi),
        'A' + MACHINE_PIN_PORT(self->miso), MACHINE_PIN_NUM(self->miso));
}

/* Resolve one signal: the caller's pin if they named one and the mux can
 * produce it, otherwise the bus default. Returns the table entry so the caller
 * gets the alternate-function number with it. */
static const machine_spi_pin_t *machine_spi_find_pin(const machine_spi_pin_t *options,
    size_t n_options, uint8_t id, mp_obj_t requested, qstr what) {
    for (size_t i = 0; i < n_options; i++) {
        if (options[i].id != id) {
            continue;
        }
        if (requested == mp_const_none || machine_pin_get(requested)->id == options[i].pin) {
            return &options[i];
        }
    }
    mp_raise_msg_varg(&mp_type_ValueError,
        MP_ERROR_TEXT("SPI(%u) has no %q on that pin"), id, what);
}

static void machine_spi_configure(machine_spi_obj_t *self, size_t n_args,
    const mp_obj_t *pos_args, mp_map_t *kw_args, bool is_new) {
    enum { ARG_baudrate, ARG_polarity, ARG_phase, ARG_bits, ARG_firstbit,
           ARG_sck, ARG_mosi, ARG_miso };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_baudrate, MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_polarity, MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_phase,    MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_bits,     MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_firstbit, MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_sck,      MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_mosi,     MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_miso,     MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args,
        MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    /* -1 means "not given". On a fresh object that becomes the default; on
     * init() it means "leave this setting alone", which is what lets
     * spi.init(baudrate=...) keep the pins and mode already in use. */
    uint32_t baudrate = SPI_DEFAULT_BAUDRATE;
    if (args[ARG_baudrate].u_int >= 0) {
        baudrate = args[ARG_baudrate].u_int;
    } else if (!is_new) {
        baudrate = self->baudrate;
    }
    if (baudrate == 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("baudrate must be positive"));
    }

    if (args[ARG_bits].u_int >= 0 && args[ARG_bits].u_int != 8) {
        /* The peripheral can do 16, but only as whole 16-bit words with a
         * byte order the buffer protocol does not describe, so exposing it
         * would be a trap rather than a feature. */
        mp_raise_ValueError(MP_ERROR_TEXT("only bits=8 is supported"));
    }

    if (args[ARG_polarity].u_int >= 0) {
        self->polarity = args[ARG_polarity].u_int ? 1 : 0;
    } else if (is_new) {
        self->polarity = SPI_DEFAULT_POLARITY;
    }
    if (args[ARG_phase].u_int >= 0) {
        self->phase = args[ARG_phase].u_int ? 1 : 0;
    } else if (is_new) {
        self->phase = SPI_DEFAULT_PHASE;
    }
    if (args[ARG_firstbit].u_int >= 0) {
        self->firstbit = args[ARG_firstbit].u_int ? MICROPY_PY_MACHINE_SPI_LSB
            : MICROPY_PY_MACHINE_SPI_MSB;
    } else if (is_new) {
        self->firstbit = SPI_DEFAULT_FIRSTBIT;
    }

    bool pins_given = args[ARG_sck].u_obj != mp_const_none
        || args[ARG_mosi].u_obj != mp_const_none
        || args[ARG_miso].u_obj != mp_const_none;

    if (is_new || pins_given) {
        const machine_spi_pin_t *sck = machine_spi_find_pin(machine_spi_sck_options,
            MP_ARRAY_SIZE(machine_spi_sck_options), self->id, args[ARG_sck].u_obj, MP_QSTR_sck);
        const machine_spi_pin_t *miso = machine_spi_find_pin(machine_spi_miso_options,
            MP_ARRAY_SIZE(machine_spi_miso_options), self->id, args[ARG_miso].u_obj, MP_QSTR_miso);
        const machine_spi_pin_t *mosi = machine_spi_find_pin(machine_spi_mosi_options,
            MP_ARRAY_SIZE(machine_spi_mosi_options), self->id, args[ARG_mosi].u_obj, MP_QSTR_mosi);

        self->sck = sck->pin;
        self->miso = miso->pin;
        self->mosi = mosi->pin;

        machine_spi_init_pin(sck->pin, sck->af);
        machine_spi_init_pin(miso->pin, miso->af);
        machine_spi_init_pin(mosi->pin, mosi->af);
    }

    uint32_t hclk = machine_spi_periph_clock();
    self->prescaler_log2 = machine_spi_prescaler_for(hclk, baudrate);
    self->baudrate = hclk >> (self->prescaler_log2 + 1);

    machine_spi_init_periph(self);
}

static mp_obj_t machine_spi_make_new(const mp_obj_type_t *type, size_t n_args,
    size_t n_kw, const mp_obj_t *all_args) {
    mp_arg_check_num(n_args, n_kw, 1, MP_OBJ_FUN_ARGS_MAX, true);

    mp_int_t id = mp_obj_get_int(all_args[0]);
    if (id < 1 || id > 4) {
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("SPI(%d) does not exist"), id);
    }

    machine_spi_obj_t *self = &machine_spi_obj[id - 1];
    self->base.type = type;
    self->spi = machine_spi_regs[id - 1];
    self->id = (uint8_t)id;

    mp_map_t kw_args;
    mp_map_init_fixed_table(&kw_args, n_kw, all_args + n_args);
    machine_spi_configure(self, n_args - 1, all_args + 1, &kw_args, true);

    return MP_OBJ_FROM_PTR(self);
}

static void machine_spi_init(mp_obj_base_t *self_in, size_t n_args,
    const mp_obj_t *pos_args, mp_map_t *kw_args) {
    machine_spi_obj_t *self = (machine_spi_obj_t *)self_in;
    machine_spi_configure(self, n_args, pos_args, kw_args, false);
}

static void machine_spi_deinit(mp_obj_base_t *self_in) {
    machine_spi_obj_t *self = (machine_spi_obj_t *)self_in;
    SPI_Cmd(self->spi, DISABLE);
    /* The pins keep their alternate function. Handing them back as inputs
     * would drop SCK and CS-adjacent lines to whatever the board pulls them
     * to, which is a worse default than leaving an idle bus idle. */
}

static const mp_machine_spi_p_t machine_spi_p = {
    .init = machine_spi_init,
    .deinit = machine_spi_deinit,
    .transfer = machine_spi_transfer,
};

MP_DEFINE_CONST_OBJ_TYPE(
    machine_spi_type,
    MP_QSTR_SPI,
    MP_TYPE_FLAG_NONE,
    make_new, machine_spi_make_new,
    print, machine_spi_print,
    protocol, &machine_spi_p,
    locals_dict, &mp_machine_spi_locals_dict
    );
