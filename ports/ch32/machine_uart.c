/* machine.UART for the CH32H417.
 *
 * Included by extmod/machine_uart.c via MICROPY_PY_MACHINE_UART_INCLUDEFILE,
 * so the mp_machine_uart_* hooks below keep the static linkage declared there.
 *
 * Both directions are interrupt-driven through ring buffers. Reads take
 * whatever the RX interrupt has already collected, so a byte arriving while
 * Python is busy is kept rather than lost, and writes hand the data to the TX
 * interrupt and return as soon as it fits -- only a full buffer blocks. The
 * console UART in uart.c is deliberately separate and untouched: it is the
 * REPL transport and must keep working even if a script misconfigures a UART.
 *
 * "Remapping" on this part is not the CH32V307/STM32F1 remap-bit scheme. The
 * H417 has an STM32F4-style per-pin alternate-function multiplexer, so any pin
 * the datasheet lists for a signal can carry it, selected with its own AF
 * number -- see machine_uart_pins.h, which is generated from the datasheet
 * rather than typed. That makes every documented pin option available, which
 * is strictly more than a remap bit would give.
 *
 * Two silent failure modes, both already documented in
 * docs/hw/ch32h417-notes.md and both guarded against here: a wrong AF number
 * leaves the USART running with nothing on the wire, and a missing AFIO clock
 * makes the AF writes disappear entirely.
 */

#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/runtime.h"
#include "py/ringbuf.h"
#include "py/stream.h"

#include "ch32h417.h"
#include "irq.h"
#include "machine_pin.h"
#include "machine_uart_pins.h"
#include "uart.h"

#define UART_ID_MIN (1)
#define UART_ID_MAX (8)

#define UART_DEFAULT_RXBUF (256)
#define UART_DEFAULT_TXBUF (256)

// machine.UART.RTS / .CTS, matching every other port's flow constants.
#define UART_FLOW_RTS (1)
#define UART_FLOW_CTS (2)

/* Exposed on the class so flow= reads the same as on every other port. */
#define MICROPY_PY_MACHINE_UART_CLASS_CONSTANTS \
    { MP_ROM_QSTR(MP_QSTR_RTS), MP_ROM_INT(UART_FLOW_RTS) }, \
    { MP_ROM_QSTR(MP_QSTR_CTS), MP_ROM_INT(UART_FLOW_CTS) },

typedef struct _machine_uart_obj_t {
    mp_obj_base_t base;
    USART_TypeDef *usart;
    uint8_t id;
    uint8_t bits;                   // data bits, 5-9, excluding parity
    uint8_t stop_tenths;            // 5, 10, 15 or 20 -- 0.5 to 2 stop bits
    uint8_t flow;
    char parity;                    // 'N', 'O' or 'E'
    uint32_t baudrate;
    uint16_t timeout;               // ms to wait for the first character
    uint16_t timeout_char;          // ms to wait between characters
    const machine_pin_obj_t *tx_pin;
    const machine_pin_obj_t *rx_pin;
    const machine_pin_obj_t *cts_pin;
    const machine_pin_obj_t *rts_pin;
    ringbuf_t rx_ring;
    ringbuf_t tx_ring;
} machine_uart_obj_t;

/* Indexed by id - 1. The interrupt handlers need to reach the object without
 * going through Python, and the buffers must outlive any particular reference,
 * so the live UARTs are rooted here and registered with the GC. */
static machine_uart_obj_t *machine_uart_objs[UART_ID_MAX];
MP_REGISTER_ROOT_POINTER(struct _machine_uart_obj_t * machine_uart_obj_all[8]);

static USART_TypeDef *const machine_uart_regs[UART_ID_MAX] = {
    USART1, USART2, USART3, USART4, USART5, USART6, USART7, USART8,
};

static const IRQn_Type machine_uart_irqn[UART_ID_MAX] = {
    USART1_IRQn, USART2_IRQn, USART3_IRQn, USART4_IRQn,
    USART5_IRQn, USART6_IRQn, USART7_IRQn, USART8_IRQn,
};

static const uint32_t machine_uart_rcc[UART_ID_MAX] = {
    RCC_HB2Periph_USART1, RCC_HB1Periph_USART2, RCC_HB1Periph_USART3,
    RCC_HB1Periph_USART4, RCC_HB1Periph_USART5, RCC_HB1Periph_USART6,
    RCC_HB1Periph_USART7, RCC_HB1Periph_USART8,
};

// USART1 hangs off HB2; the rest off HB1.
static void machine_uart_clock_enable(uint8_t id) {
    if (id == 1) {
        RCC_HB2PeriphClockCmd(machine_uart_rcc[0], ENABLE);
    } else {
        RCC_HB1PeriphClockCmd(machine_uart_rcc[id - 1], ENABLE);
    }
    /* The enable is a read-modify-write, so nothing pushes the store out and
     * the first register access afterwards can be dropped. Same trap as the
     * GPHA clock enable. */
    (void)RCC->HB1PCENR;
}

/* Find the AF number for `pin` on this USART's `sig`, or -1 if the datasheet
 * does not route that signal there. Rejecting the pin is the whole point: a
 * plausible-looking pin with no entry would otherwise be configured as AF0 and
 * fail silently. */
static int machine_uart_pin_af(uint8_t id, uint8_t sig, uint8_t pin_id) {
    for (size_t i = 0; i < MP_ARRAY_SIZE(uart_pin_af_table); i++) {
        const uart_pin_af_t *e = &uart_pin_af_table[i];
        if (e->uart == id && e->sig == sig && e->pin == pin_id) {
            return e->af;
        }
    }
    return -1;
}

static const char *const machine_uart_sig_name[] = { "tx", "rx", "cts", "rts" };

static void machine_uart_config_pin(uint8_t id, uint8_t sig, const machine_pin_obj_t *pin) {
    int af = machine_uart_pin_af(id, sig, pin->id);
    if (af < 0) {
        mp_raise_msg_varg(&mp_type_ValueError,
            MP_ERROR_TEXT("pin cannot be UART%d %s"), id, machine_uart_sig_name[sig]);
    }

    machine_pin_clock_enable(pin->id);
    /* Without the AFIO clock the AF writes below are silently dropped and the
     * pad stays on whatever it was, with no error anywhere. */
    RCC_HB2PeriphClockCmd(RCC_HB2Periph_AFIO, ENABLE);
    (void)RCC->HB2PCENR;

    GPIO_PinAFConfig(machine_pin_gpio(pin->id), MACHINE_PIN_NUM(pin->id), af);

    GPIO_InitTypeDef init = { 0 };
    init.GPIO_Pin = MACHINE_PIN_MASK(pin->id);
    init.GPIO_Speed = GPIO_Speed_High;
    /* RX and CTS are driven by the peripheral or the far end; TX and RTS are
     * driven by us. An input pin left as AF push-pull would fight its driver. */
    init.GPIO_Mode = (sig == UART_SIG_RX || sig == UART_SIG_CTS)
        ? GPIO_Mode_IPU : GPIO_Mode_AF_PP;
    GPIO_Init(machine_pin_gpio(pin->id), &init);
}

/* --- interrupts --- */

static void machine_uart_irq_handler(machine_uart_obj_t *self) {
    USART_TypeDef *u = self->usart;

    /* Read STATR once: reading it and then DATAR is also how the error flags
     * are cleared, so sampling it twice can lose an event. */
    uint32_t statr = u->STATR;

    if (statr & USART_STATR_RXNE) {
        if (ringbuf_free(&self->rx_ring) > 0) {
            ringbuf_put(&self->rx_ring, (uint8_t)(u->DATAR & 0xff));
        } else {
            /* Ring full: deliberately leave the byte in DATAR rather than
             * reading and discarding it. RXNE therefore stays set, which is
             * precisely the condition the hardware drives RTS from, so RTS
             * deasserts and a flow-controlled peer stops sending -- and the
             * byte itself is preserved instead of thrown away.
             *
             * The obvious alternative, draining DATAR and dropping on a full
             * ring, silently defeats hardware RTS: the receive register is
             * emptied on every interrupt, so it is never full, so RTS never
             * deasserts. Measured that way, enabling RTS made no difference
             * whatsoever to a burst that overran the ring.
             *
             * RXNEIE is masked here so the interrupt does not re-enter on a
             * byte it cannot take; the reader re-enables it after making room.
             * Without flow control this still behaves better than dropping:
             * the hardware holds one more byte and only then overruns. */
            u->CTLR1 &= ~USART_CTLR1_RXNEIE;
        }
    }

    /* Framing, parity, noise and overrun all latch in STATR and are cleared by
     * the read above followed by a read of DATAR. Without draining DATAR an
     * overrun wedges the receiver: RXNE never sets again. */
    if (statr & (USART_STATR_ORE | USART_STATR_FE | USART_STATR_NE | USART_STATR_PE)) {
        (void)u->DATAR;
    }

    if ((statr & USART_STATR_TXE) && (u->CTLR1 & USART_CTLR1_TXEIE)) {
        int c = ringbuf_get(&self->tx_ring);
        if (c < 0) {
            /* Nothing left: stop asking. Leaving TXEIE set would re-enter this
             * handler forever, since TXE stays asserted while idle. */
            u->CTLR1 &= ~USART_CTLR1_TXEIE;
        } else {
            u->DATAR = (uint16_t)c;
        }
    }
}

#define MACHINE_UART_IRQ_HANDLER(n)                                  \
    void CH32_IRQ_HANDLER(USART##n##_IRQHandler);                    \
    void USART##n##_IRQHandler(void) {                               \
        machine_uart_obj_t *self = machine_uart_objs[(n) - 1];       \
        if (self != NULL) {                                          \
            machine_uart_irq_handler(self);                          \
        }                                                            \
    }

/* USART1 is the REPL console, driven by uart.c, which owns that vector. */
MACHINE_UART_IRQ_HANDLER(2)
MACHINE_UART_IRQ_HANDLER(3)
MACHINE_UART_IRQ_HANDLER(4)
MACHINE_UART_IRQ_HANDLER(5)
MACHINE_UART_IRQ_HANDLER(6)
MACHINE_UART_IRQ_HANDLER(7)
MACHINE_UART_IRQ_HANDLER(8)

/* --- configuration --- */

static void machine_uart_apply(machine_uart_obj_t *self) {
    USART_TypeDef *u = self->usart;

    USART_InitTypeDef init = { 0 };
    init.USART_BaudRate = self->baudrate;
    /* 9 data bits is really 8 data + 1 parity in the hardware's counting, so a
     * parity-carrying 8-bit frame has to ask for 9. */
    /* The hardware's word length counts the parity bit, so 8E1 is a 9-bit
     * word carrying 8 data bits. MicroPython's `bits` means data bits, which
     * is what a user counts, so the parity bit is added here rather than
     * being the caller's problem. */
    uint32_t total = self->bits + (self->parity != 'N' ? 1 : 0);
    static const uint16_t wordlen[] = {
        USART_WordLength_5b, USART_WordLength_6b, USART_WordLength_7b,
        USART_WordLength_8b, USART_WordLength_9b,
    };
    init.USART_WordLength = wordlen[total - 5];
    init.USART_StopBits =
        self->stop_tenths == 5 ? USART_StopBits_0_5 :
        self->stop_tenths == 15 ? USART_StopBits_1_5 :
        self->stop_tenths == 20 ? USART_StopBits_2 : USART_StopBits_1;
    init.USART_Parity = (self->parity == 'O') ? USART_Parity_Odd
        : (self->parity == 'E') ? USART_Parity_Even : USART_Parity_No;
    init.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;
    /* CTS goes to the hardware, which genuinely gates each transmitted byte on
     * the pin. RTS deliberately does NOT: the hardware drives it from the
     * receive *register*, and this driver empties that register into a ring
     * buffer on every interrupt, so hardware RTS would never deassert and
     * would protect nothing. Measured before this was changed: a 256-byte
     * burst at 115200 into a 64-byte ring lost exactly as much with hardware
     * RTS enabled as without it -- 82 bytes arrived either way. RTS is driven
     * from the interrupt below against the ring's own occupancy instead. */
    init.USART_HardwareFlowControl =
        ((self->flow & UART_FLOW_RTS) ? USART_HardwareFlowControl_RTS : 0)
        | ((self->flow & UART_FLOW_CTS) ? USART_HardwareFlowControl_CTS : 0);

    machine_uart_clock_enable(self->id);
    USART_Init(u, &init);

    /* RX interrupt on from the start so bytes are collected whether or not
     * anything is reading yet; TX interrupt only while there is data to send. */
    u->CTLR1 |= USART_CTLR1_RXNEIE;
    USART_Cmd(u, ENABLE);

    NVIC_EnableIRQ(machine_uart_irqn[self->id - 1]);
}

static void mp_machine_uart_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    (void)kind;
    machine_uart_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "UART(%u, baudrate=%u, bits=%u, parity=%s, stop=%.1f",
        self->id, self->baudrate, self->bits,
        self->parity == 'N' ? "None" : (self->parity == 'O' ? "1" : "0"),
        (double)self->stop_tenths / 10);
    if (self->tx_pin) {
        mp_printf(print, ", tx=P%c%u", 'A' + MACHINE_PIN_PORT(self->tx_pin->id),
            MACHINE_PIN_NUM(self->tx_pin->id));
    }
    if (self->rx_pin) {
        mp_printf(print, ", rx=P%c%u", 'A' + MACHINE_PIN_PORT(self->rx_pin->id),
            MACHINE_PIN_NUM(self->rx_pin->id));
    }
    if (self->flow) {
        mp_printf(print, ", flow=%s%s",
            (self->flow & UART_FLOW_RTS) ? "RTS" : "",
            (self->flow & UART_FLOW_CTS) ? ((self->flow & UART_FLOW_RTS) ? "|CTS" : "CTS") : "");
    }
    mp_printf(print, ", timeout=%u, timeout_char=%u, rxbuf=%u, txbuf=%u)",
        self->timeout, self->timeout_char,
        self->rx_ring.size - 1, self->tx_ring.size - 1);
}

enum {
    ARG_baudrate, ARG_bits, ARG_parity, ARG_stop,
    ARG_tx, ARG_rx, ARG_cts, ARG_rts, ARG_flow,
    ARG_timeout, ARG_timeout_char, ARG_rxbuf, ARG_txbuf,
};

static const mp_arg_t machine_uart_allowed_args[] = {
    { MP_QSTR_baudrate,     MP_ARG_INT,  {.u_int = -1} },
    { MP_QSTR_bits,         MP_ARG_INT,  {.u_int = -1} },
    { MP_QSTR_parity,       MP_ARG_OBJ,  {.u_rom_obj = MP_ROM_INT(-1)} },
    /* OBJ rather than INT because 0.5 and 1.5 stop bits are real settings on
     * this USART, and an int parameter could not express them. */
    { MP_QSTR_stop,         MP_ARG_OBJ,  {.u_rom_obj = MP_ROM_INT(-1)} },
    { MP_QSTR_tx,           MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
    { MP_QSTR_rx,           MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
    { MP_QSTR_cts,          MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
    { MP_QSTR_rts,          MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
    { MP_QSTR_flow,         MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1} },
    { MP_QSTR_timeout,      MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1} },
    { MP_QSTR_timeout_char, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1} },
    { MP_QSTR_rxbuf,        MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1} },
    { MP_QSTR_txbuf,        MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1} },
};

static void mp_machine_uart_init_helper(machine_uart_obj_t *self,
    size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {

    mp_arg_val_t args[MP_ARRAY_SIZE(machine_uart_allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args,
        MP_ARRAY_SIZE(machine_uart_allowed_args), machine_uart_allowed_args, args);

    if (args[ARG_baudrate].u_int > 0) {
        self->baudrate = args[ARG_baudrate].u_int;
    }
    uint8_t bits = self->bits;
    if (args[ARG_bits].u_int > 0) {
        if (args[ARG_bits].u_int < 5 || args[ARG_bits].u_int > 9) {
            mp_raise_ValueError(MP_ERROR_TEXT("bits must be 5 to 9"));
        }
        bits = args[ARG_bits].u_int;
    }
    char parity = self->parity;
    if (args[ARG_parity].u_obj != MP_OBJ_NEW_SMALL_INT(-1)) {
        if (args[ARG_parity].u_obj == mp_const_none) {
            parity = 'N';
        } else {
            parity = (mp_obj_get_int(args[ARG_parity].u_obj) & 1) ? 'O' : 'E';
        }
    }

    /* Data bits plus the parity bit must land on a word length the hardware
     * has. 9 data bits with parity would need a 10-bit word, which it does
     * not, so say so rather than silently dropping the parity. */
    if (bits + (parity != 'N' ? 1 : 0) > 9) {
        mp_raise_ValueError(MP_ERROR_TEXT("bits+parity exceeds a 9-bit word"));
    }
    self->bits = bits;
    self->parity = parity;

    if (args[ARG_stop].u_obj != MP_OBJ_NEW_SMALL_INT(-1)) {
        mp_float_t st = mp_obj_get_float(args[ARG_stop].u_obj);
        int tenths = (int)(st * 10 + (mp_float_t)0.5);
        if (tenths != 5 && tenths != 10 && tenths != 15 && tenths != 20) {
            mp_raise_ValueError(MP_ERROR_TEXT("stop must be 0.5, 1, 1.5 or 2"));
        }
        self->stop_tenths = tenths;
    }
    if (args[ARG_timeout].u_int >= 0) {
        self->timeout = args[ARG_timeout].u_int;
    }
    if (args[ARG_timeout_char].u_int >= 0) {
        self->timeout_char = args[ARG_timeout_char].u_int;
    }
    /* Everything from here is validated into locals before being stored,
     * because UART(id) returns a singleton: raising after a partial update
     * would leave the object carrying settings the caller never successfully
     * asked for, and the *next* construction would then fail for a reason
     * that has nothing to do with its own arguments. */
    uint8_t flow = self->flow;
    if (args[ARG_flow].u_int >= 0) {
        if (args[ARG_flow].u_int & ~(UART_FLOW_RTS | UART_FLOW_CTS)) {
            mp_raise_ValueError(MP_ERROR_TEXT("bad flow"));
        }
        flow = args[ARG_flow].u_int;
    }

    const machine_pin_obj_t *tx = self->tx_pin, *rx = self->rx_pin;
    const machine_pin_obj_t *cts = self->cts_pin, *rts = self->rts_pin;
    if (args[ARG_tx].u_obj != mp_const_none) {
        tx = machine_pin_get(args[ARG_tx].u_obj);
    }
    if (args[ARG_rx].u_obj != mp_const_none) {
        rx = machine_pin_get(args[ARG_rx].u_obj);
    }
    if (args[ARG_cts].u_obj != mp_const_none) {
        cts = machine_pin_get(args[ARG_cts].u_obj);
    }
    if (args[ARG_rts].u_obj != mp_const_none) {
        rts = machine_pin_get(args[ARG_rts].u_obj);
    }

    /* Flow control without the matching pin would look configured and do
     * nothing, which is worse than refusing. */
    if ((flow & UART_FLOW_RTS) && rts == NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("flow=RTS needs rts="));
    }
    if ((flow & UART_FLOW_CTS) && cts == NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("flow=CTS needs cts="));
    }

    /* Check every pin against the datasheet table before touching any of
     * them, so a bad third pin cannot leave the first two reconfigured. */
    if (tx && machine_uart_pin_af(self->id, UART_SIG_TX, tx->id) < 0) {
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("pin cannot be UART%d tx"), self->id);
    }
    if (rx && machine_uart_pin_af(self->id, UART_SIG_RX, rx->id) < 0) {
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("pin cannot be UART%d rx"), self->id);
    }
    if (cts && machine_uart_pin_af(self->id, UART_SIG_CTS, cts->id) < 0) {
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("pin cannot be UART%d cts"), self->id);
    }
    if (rts && machine_uart_pin_af(self->id, UART_SIG_RTS, rts->id) < 0) {
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("pin cannot be UART%d rts"), self->id);
    }

    self->flow = flow;
    self->tx_pin = tx;
    self->rx_pin = rx;
    self->cts_pin = cts;
    self->rts_pin = rts;

    size_t rxbuf = args[ARG_rxbuf].u_int > 0 ? args[ARG_rxbuf].u_int : UART_DEFAULT_RXBUF;
    size_t txbuf = args[ARG_txbuf].u_int > 0 ? args[ARG_txbuf].u_int : UART_DEFAULT_TXBUF;
    if (self->rx_ring.buf == NULL || args[ARG_rxbuf].u_int > 0) {
        ringbuf_alloc(&self->rx_ring, rxbuf + 1);
    }
    if (self->tx_ring.buf == NULL || args[ARG_txbuf].u_int > 0) {
        ringbuf_alloc(&self->tx_ring, txbuf + 1);
    }

    if (self->tx_pin) {
        machine_uart_config_pin(self->id, UART_SIG_TX, self->tx_pin);
    }
    if (self->rx_pin) {
        machine_uart_config_pin(self->id, UART_SIG_RX, self->rx_pin);
    }
    if (self->cts_pin) {
        machine_uart_config_pin(self->id, UART_SIG_CTS, self->cts_pin);
    }
    if (self->rts_pin) {
        machine_uart_config_pin(self->id, UART_SIG_RTS, self->rts_pin);
    }

    /* A timeout_char of 0 makes read(n) give up after the first byte and
     * return one or two, which turns a single read into a churn of calls with
     * interpreter overhead between them -- long enough at 115200 to overrun a
     * small ring and lose most of a burst. Floor it at one character time, 13
     * bits to allow for the start, stop and parity bits, exactly as ports/stm32
     * does. A caller who wants a truly non-blocking read still has any(). */
    uint32_t min_timeout_char = 13000 / self->baudrate + 1;
    if (self->timeout_char < min_timeout_char) {
        self->timeout_char = min_timeout_char;
    }

    machine_uart_apply(self);
}

static mp_obj_t mp_machine_uart_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *args) {

    mp_arg_check_num(n_args, n_kw, 1, MP_OBJ_FUN_ARGS_MAX, true);
    mp_int_t id = mp_obj_get_int(args[0]);
    if (id < UART_ID_MIN || id > UART_ID_MAX) {
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("UART(%d) does not exist"), (int)id);
    }
    if (id == 1) {
        /* USART1 carries the REPL. Handing it to a script would take the
         * console away mid-session, with no way to report what happened. */
        mp_raise_ValueError(MP_ERROR_TEXT("UART(1) is the REPL console"));
    }

    machine_uart_obj_t *self = machine_uart_objs[id - 1];
    if (self == NULL) {
        self = mp_obj_malloc(machine_uart_obj_t, type);
        self->id = id;
        self->usart = machine_uart_regs[id - 1];
        self->baudrate = 9600;
        self->bits = 8;
        self->parity = 'N';
        self->stop_tenths = 10;
        self->flow = 0;
        self->timeout = 0;
        self->timeout_char = 0;
        self->tx_pin = self->rx_pin = self->cts_pin = self->rts_pin = NULL;
        self->rx_ring.buf = NULL;
        self->tx_ring.buf = NULL;
        machine_uart_objs[id - 1] = self;
        MP_STATE_PORT(machine_uart_obj_all)[id - 1] = self;
    }
    self->base.type = type;

    mp_map_t kw_args;
    mp_map_init_fixed_table(&kw_args, n_kw, args + n_args);
    mp_machine_uart_init_helper(self, n_args - 1, args + 1, &kw_args);
    return MP_OBJ_FROM_PTR(self);
}

static void mp_machine_uart_deinit(machine_uart_obj_t *self) {
    NVIC_DisableIRQ(machine_uart_irqn[self->id - 1]);
    USART_Cmd(self->usart, DISABLE);
    self->usart->CTLR1 &= ~(USART_CTLR1_RXNEIE | USART_CTLR1_TXEIE);
    machine_uart_objs[self->id - 1] = NULL;
    MP_STATE_PORT(machine_uart_obj_all)[self->id - 1] = NULL;
}

/* Every UART is torn down before the heap goes, or an interrupt arriving after
 * a soft reset would push a byte into a ring buffer that no longer exists. */
void machine_uart_deinit_all(void) {
    for (int i = 0; i < UART_ID_MAX; i++) {
        if (machine_uart_objs[i] != NULL) {
            mp_machine_uart_deinit(machine_uart_objs[i]);
        }
    }
}

/* Re-arm the receive interrupt once there is somewhere to put a byte. Paired
 * with the ISR masking it on a full ring. */
static inline void machine_uart_rx_resume(machine_uart_obj_t *self) {
    if (ringbuf_free(&self->rx_ring) > 0) {
        self->usart->CTLR1 |= USART_CTLR1_RXNEIE;
    }
}

static mp_int_t mp_machine_uart_any(machine_uart_obj_t *self) {
    /* Also the recovery point for a caller that drains with any()/read(any())
     * and never goes through the per-byte path below. */
    machine_uart_rx_resume(self);
    return ringbuf_avail(&self->rx_ring);
}

static bool mp_machine_uart_txdone(machine_uart_obj_t *self) {
    return ringbuf_avail(&self->tx_ring) == 0 && (self->usart->STATR & USART_STATR_TC);
}

static void mp_machine_uart_sendbreak(machine_uart_obj_t *self) {
    self->usart->CTLR1 |= USART_CTLR1_SBK;
}

static mp_uint_t mp_machine_uart_read(mp_obj_t self_in, void *buf_in, mp_uint_t size, int *errcode) {
    machine_uart_obj_t *self = MP_OBJ_TO_PTR(self_in);
    uint8_t *dest = buf_in;

    /* The first byte gets `timeout`, each later one `timeout_char`. Waiting is
     * done with mp_event_wait_ms() so the scheduler and USB keep running and
     * Ctrl-C still works. */
    for (size_t got = 0; got < size;) {
        mp_uint_t deadline = mp_hal_ticks_ms() + (got == 0 ? self->timeout : self->timeout_char);
        int c;
        while ((c = ringbuf_get(&self->rx_ring)) < 0) {
            if ((mp_int_t)(mp_hal_ticks_ms() - deadline) >= 0) {
                machine_uart_rx_resume(self);
                if (got == 0) {
                    *errcode = MP_EAGAIN;
                    return MP_STREAM_ERROR;
                }
                return got;
            }
            mp_event_wait_ms(1);
        }
        dest[got++] = (uint8_t)c;
        machine_uart_rx_resume(self);
    }
    return size;
}

static mp_uint_t mp_machine_uart_write(mp_obj_t self_in, const void *buf_in, mp_uint_t size, int *errcode) {
    machine_uart_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const uint8_t *src = buf_in;

    for (size_t sent = 0; sent < size;) {
        if (ringbuf_put(&self->tx_ring, src[sent]) < 0) {
            /* Buffer full: let the interrupt drain some and try again. This is
             * the only case that blocks, and it is bounded by the line rate. */
            self->usart->CTLR1 |= USART_CTLR1_TXEIE;
            mp_uint_t deadline = mp_hal_ticks_ms() + (self->timeout ? self->timeout : 1000);
            while (ringbuf_free(&self->tx_ring) == 0) {
                if ((mp_int_t)(mp_hal_ticks_ms() - deadline) >= 0) {
                    if (sent == 0) {
                        *errcode = MP_EAGAIN;
                        return MP_STREAM_ERROR;
                    }
                    return sent;
                }
                mp_event_wait_ms(1);
            }
            continue;
        }
        sent++;
    }

    /* Kick the transmitter last: enabling TXEIE earlier would let the handler
     * race ahead of the bytes still being queued. */
    self->usart->CTLR1 |= USART_CTLR1_TXEIE;
    return size;
}

static mp_int_t mp_machine_uart_readchar(machine_uart_obj_t *self) {
    int c = ringbuf_get(&self->rx_ring);
    machine_uart_rx_resume(self);
    return c;
}

static void mp_machine_uart_writechar(machine_uart_obj_t *self, uint16_t data) {
    ringbuf_put(&self->tx_ring, (uint8_t)data);
    self->usart->CTLR1 |= USART_CTLR1_TXEIE;
}

static mp_uint_t mp_machine_uart_ioctl(mp_obj_t self_in, mp_uint_t request, uintptr_t arg, int *errcode) {
    machine_uart_obj_t *self = MP_OBJ_TO_PTR(self_in);
    switch (request) {
        case MP_STREAM_POLL: {
            mp_uint_t ret = 0;
            if ((arg & MP_STREAM_POLL_RD) && ringbuf_avail(&self->rx_ring) > 0) {
                ret |= MP_STREAM_POLL_RD;
            }
            if ((arg & MP_STREAM_POLL_WR) && ringbuf_free(&self->tx_ring) > 0) {
                ret |= MP_STREAM_POLL_WR;
            }
            return ret;
        }
        case MP_STREAM_FLUSH: {
            mp_uint_t deadline = mp_hal_ticks_ms() + 1000;
            while (!mp_machine_uart_txdone(self)) {
                if ((mp_int_t)(mp_hal_ticks_ms() - deadline) >= 0) {
                    *errcode = MP_ETIMEDOUT;
                    return MP_STREAM_ERROR;
                }
                mp_event_wait_ms(1);
            }
            return 0;
        }
        default:
            *errcode = MP_EINVAL;
            return MP_STREAM_ERROR;
    }
}
