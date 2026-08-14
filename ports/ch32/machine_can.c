/* machine.CAN for the CH32H417.
 *
 * Included by extmod/machine_can.c through MICROPY_PY_MACHINE_CAN_INCLUDEFILE,
 * so everything here is static and implements the contract in
 * extmod/machine_can_port.h.
 *
 * The peripheral is bxCAN, the same one STM32 has, with WCH's register names:
 * CTLR/STATR/TSTATR instead of MCR/MSR/TSR, three transmit mailboxes, two
 * receive FIFOs, and a bank of 32-bit filters shared between controllers.
 * There are three of them here, CAN1/CAN2/CAN3, and no CAN-FD.
 *
 * --- pins --------------------------------------------------------------
 *
 * Taken from the vendor's own table (EVT/EXAM/CAN/Networking/Common/
 * hardware.c), not guessed, because the alternate function differs per pin
 * rather than per peripheral -- CAN1 is AF3 on PB6/PB7 and AF9 everywhere
 * else:
 *
 *   CAN1  TX  PA12/AF9  PB7/AF3  PB9/AF9  PD1/AF9  PA14/AF5
 *         RX  PA11/AF9  PB6/AF3  PB8/AF9  PD0/AF9  PA13/AF5
 *   CAN2  TX  PB13/AF9  PB6/AF9
 *         RX  PB12/AF9  PB5/AF9
 *   CAN3  TX  PD13/AF5  PF7/AF2  PF3/AF2  PC5/AF6
 *         RX  PD12/AF5  PF6/AF2  PF4/AF2  PC4/AF6
 *
 * The defaults below pick the pairs that are in the 3.3 V domain, which on
 * this board is not most of them -- see README.md, "Pin voltage domains". That
 * gives CAN1 on PB7/PB6 and CAN3 on PC5/PC4, both of which drive an ordinary
 * 3.3 V transceiver directly and neither of which touches PB8/PB9, the SWD
 * pins. CAN2's only choices are PB12/PB13 and PB5/PB6, and PB6 collides with
 * CAN1, so CAN2 defaults to PB12/PB13 with a domain that has not been measured
 * on this board. Prefer CAN1 and CAN3.
 *
 * --- clock -------------------------------------------------------------
 *
 * The bit time is built from HCLK, asked for rather than assumed: this chip's
 * core clock is four times its bus clock and hard-coding the wrong one would
 * put every bitrate out by 4x. Note that loopback cannot catch that mistake,
 * because transmitter and receiver share the clock and stay consistent with
 * each other whatever it is -- test_can.py times a burst of frames against the
 * millisecond clock instead, which does catch it.
 */
#include <stdbool.h>
#include <string.h>

#include "ch32h417.h"

#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/runtime.h"

#include "extmod/machine_can_port.h"

#include "irq.h"
#include "machine_pin.h"

/* bxCAN's prescaler is 10 bits, so 1..1024, and its filters are shared between
 * standard and extended identifiers rather than indexed separately. */
#define CAN_BRP_MIN 1
#define CAN_BRP_MAX 1024
#define CAN_FILTERS_STD_EXT_SEPARATE 0

/* Three hardware mailboxes, so three outstanding transmits. There is no
 * software queue on top: send() returning the mailbox index is what lets a
 * caller cancel one, and a queue would hand back an index that named nothing
 * the hardware knows about. */
#define CAN_TX_QUEUE_LEN 3

/* Filter banks per controller. There are 42 in the block; CAN1 and CAN2 split
 * the first 28 at CAN2SB, and CAN3 has its own configuration registers over
 * the rest. Fourteen each keeps that split static and legible. */
#define CAN_HW_MAX_FILTER 14

/* Nothing here is claimed by the system. */
#define MICROPY_HW_CAN_IS_RESERVED(n) (false)

/* CTLR */
#define CAN_CTLR_INRQ   (1u << 0)
#define CAN_CTLR_SLEEP  (1u << 1)
#define CAN_CTLR_TXFP   (1u << 2)
#define CAN_CTLR_NART   (1u << 4)
#define CAN_CTLR_ABOM   (1u << 6)
#define CAN_CTLR_RESET  (1u << 15)

/* STATR */
#define CAN_STATR_INAK  (1u << 0)
#define CAN_STATR_SLAK  (1u << 1)

/* TSTATR */
#define CAN_TSTATR_RQCP(i)  (1u << (8 * (i)))
#define CAN_TSTATR_TXOK(i)  (1u << (8 * (i) + 1))
#define CAN_TSTATR_TERR(i)  (1u << (8 * (i) + 3))
#define CAN_TSTATR_ABRQ(i)  (1u << (8 * (i) + 7))
#define CAN_TSTATR_TME(i)   (1u << (26 + (i)))

/* RFIFOx */
#define CAN_RFIFO_FMP   (3u << 0)
#define CAN_RFIFO_FULL  (1u << 3)
#define CAN_RFIFO_FOVR  (1u << 4)
#define CAN_RFIFO_RFOM  (1u << 5)

/* INTENR */
#define CAN_INTENR_TMEIE   (1u << 0)
#define CAN_INTENR_FMPIE0  (1u << 1)
#define CAN_INTENR_FMPIE1  (1u << 4)
#define CAN_INTENR_EWGIE   (1u << 8)
#define CAN_INTENR_EPVIE   (1u << 9)
#define CAN_INTENR_BOFFIE  (1u << 10)
#define CAN_INTENR_ERRIE   (1u << 15)

/* ERRSR */
#define CAN_ERRSR_EWGF  (1u << 0)
#define CAN_ERRSR_EPVF  (1u << 1)
#define CAN_ERRSR_BOFF  (1u << 2)

/* BTIMR */
#define CAN_BTIMR_LBKM  (1u << 30)
#define CAN_BTIMR_SILM  (1u << 31)

/* Mailbox identifier register */
#define CAN_TXMIR_TXRQ  (1u << 0)
#define CAN_TXMIR_RTR   (1u << 1)
#define CAN_TXMIR_IDE   (1u << 2)

#define CAN_TX_MAILBOXES (3)
#define TX_EMPTY UINT32_MAX

/* INAK and SLAK are driven by the CAN clock domain, which at the lowest
 * bitrates is far slower than the core. Bounded, because a wait on a
 * peripheral whose clock might not be running must never be able to hang. */
#define CAN_MODE_TIMEOUT_MS (25)

typedef struct _ch32_can_hw_t {
    CAN_TypeDef *regs;
    uint32_t rcc_bit;
    uint8_t tx_pin;             // machine.Pin id, port * 16 + number
    uint8_t rx_pin;
    uint8_t af;
    uint8_t filter_base;    // first filter bank owned by this controller
    uint8_t filter_count;
    IRQn_Type irq_tx;
    IRQn_Type irq_rx0;
    IRQn_Type irq_sce;
} ch32_can_hw_t;

/* Filter banks: 42 in all. CAN1 and CAN2 split the first 28 at the boundary
 * CAN2SB, which the vendor code leaves at 14; CAN3 has its own configuration
 * registers (FMCFGR_CAN3 and friends) over the remaining 14. */
static const ch32_can_hw_t ch32_can_hw[] = {
    { CAN1, RCC_HB1Periph_CAN1, MACHINE_PIN_ID(1, 7), MACHINE_PIN_ID(1, 6), GPIO_AF3,
      0, 14, CAN1_TX_IRQn, CAN1_RX0_IRQn, CAN1_SCE_IRQn },        // PB7 / PB6
    { CAN2, RCC_HB1Periph_CAN2, MACHINE_PIN_ID(1, 13), MACHINE_PIN_ID(1, 12), GPIO_AF9,
      14, 14, CAN2_TX_IRQn, CAN2_RX0_IRQn, CAN2_SCE_IRQn },       // PB13 / PB12
    { CAN3, RCC_HB1Periph_CAN3, MACHINE_PIN_ID(2, 5), MACHINE_PIN_ID(2, 4), GPIO_AF6,
      28, 14, CAN3_TX_IRQn, CAN3_RX0_IRQn, CAN3_SCE_IRQn },       // PC5 / PC4
};

#define CH32_CAN_COUNT MP_ARRAY_SIZE(ch32_can_hw)

struct machine_can_port {
    const ch32_can_hw_t *hw;
    /* Identifier queued in each hardware mailbox, or TX_EMPTY. The extmod
     * layer hands out these indexes from send() so that cancel_send() and the
     * TX interrupt can name a specific message. */
    uint32_t tx[CAN_TX_MAILBOXES];
    /* Sticky flags collected by the ISR and drained by irq_flags(). */
    volatile uint16_t irq_flags;
    /* Last state seen, so the SCE interrupt can report only transitions. */
    machine_can_state_t last_state;
    uint8_t next_filter;
};

/* The extmod layer owns the object table -- MP_STATE_PORT(machine_can_objs) --
 * and it is a GC root, so an interrupt can reach its controller through it. */
#define ch32_can_obj(idx) (MP_STATE_PORT(machine_can_objs)[(idx)])

/* --- helpers --- */

/* Same shape as machine_spi_init_pin(): the AFIO clock has to be on or
 * GPIO_PinAFConfig writes are silently dropped, and on this F1-style mux the
 * input only reaches the peripheral once the mux points at it -- so an RX pin
 * left as a plain input receives nothing. */
static void can_init_pin(uint8_t pin, uint8_t af, uint32_t mode) {
    machine_pin_clock_enable(pin);
    RCC_HB2PeriphClockCmd(RCC_HB2Periph_AFIO, ENABLE);

    GPIO_InitTypeDef init = { 0 };
    init.GPIO_Mode = mode;
    init.GPIO_Speed = GPIO_Speed_Very_High;
    init.GPIO_Pin = MACHINE_PIN_MASK(pin);
    GPIO_Init(machine_pin_gpio(pin), &init);

    GPIO_PinAFConfig(machine_pin_gpio(pin), MACHINE_PIN_NUM(pin), af);
}

static void can_release_pin(uint8_t pin) {
    GPIO_InitTypeDef init = { 0 };
    init.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    init.GPIO_Speed = GPIO_Speed_Very_High;
    init.GPIO_Pin = MACHINE_PIN_MASK(pin);
    GPIO_Init(machine_pin_gpio(pin), &init);
}

static inline CAN_TypeDef *can_regs(const machine_can_obj_t *self) {
    return self->port->hw->regs;
}

static bool can_wait_bit(volatile uint32_t *reg, uint32_t mask, bool set) {
    mp_uint_t deadline = mp_hal_ticks_ms() + CAN_MODE_TIMEOUT_MS;
    while (((*reg & mask) != 0) != set) {
        if (mp_hal_ticks_ms() > deadline) {
            return false;
        }
    }
    return true;
}

/* Empty both receive FIFOs.
 *
 * Needed on the way in and on the way out, because nothing else clears them:
 * the peripheral keeps running across a soft reset, so a program that left
 * frames unread hands them to the next one. That is how it presents -- a
 * freshly constructed CAN object whose first recv() returns a message from the
 * program before it, with RECV_ERR_FULL set because the FIFO had filled. */
static void can_flush_rx(CAN_TypeDef *regs) {
    for (int fifo = 0; fifo < 2; fifo++) {
        volatile uint32_t *rf = fifo == 0 ? &regs->RFIFO0 : &regs->RFIFO1;
        /* Three mailboxes per FIFO, so this cannot spin: each RFOM releases
         * one and the count only goes down. */
        for (int i = 0; i < 4 && (*rf & CAN_RFIFO_FMP); i++) {
            *rf = CAN_RFIFO_RFOM;
        }
        *rf = CAN_RFIFO_FULL | CAN_RFIFO_FOVR;
    }
}

static machine_can_state_t can_state_from_hw(CAN_TypeDef *regs) {
    uint32_t esr = regs->ERRSR;
    if (esr & CAN_ERRSR_BOFF) {
        return MP_CAN_STATE_BUS_OFF;
    }
    if (esr & CAN_ERRSR_EPVF) {
        return MP_CAN_STATE_PASSIVE;
    }
    if (esr & CAN_ERRSR_EWGF) {
        return MP_CAN_STATE_WARNING;
    }
    return MP_CAN_STATE_ACTIVE;
}

/* --- the port contract --- */

static int machine_can_port_f_clock(const machine_can_obj_t *self) {
    (void)self;
    RCC_ClocksTypeDef clocks;
    RCC_GetClocksFreq(&clocks);
    return (int)clocks.HCLK_Frequency;
}

static bool machine_can_port_supports_mode(const machine_can_obj_t *self, machine_can_mode_t mode) {
    (void)self;
    /* Sleep is a real bxCAN mode but it is not an *operating* one: the
     * controller stops taking part in the bus, which is what SILENT already
     * offers with the bus still monitored. Following ports/stm32 in mapping it
     * that way would report a mode the hardware is not in, so refuse it. */
    return mode == MP_CAN_MODE_NORMAL || mode == MP_CAN_MODE_LOOPBACK
           || mode == MP_CAN_MODE_SILENT || mode == MP_CAN_MODE_SILENT_LOOPBACK;
}

static mp_uint_t machine_can_port_max_data_len(mp_uint_t flags) {
    (void)flags;
    return 8;   // classic CAN only; there is no FD controller here
}

static void machine_can_port_init(machine_can_obj_t *self) {
    const ch32_can_hw_t *hw = &ch32_can_hw[self->can_idx];

    if (self->port == NULL) {
        self->port = m_new(struct machine_can_port, 1);
    }
    memset(self->port, 0, sizeof(struct machine_can_port));
    self->port->hw = hw;
    for (int i = 0; i < CAN_TX_MAILBOXES; i++) {
        self->port->tx[i] = TX_EMPTY;
    }

    RCC_HB1PeriphClockCmd(hw->rcc_bit, ENABLE);
    /* CAN2 and CAN3 read their filter configuration out of CAN1's register
     * block, so CAN1's clock has to be on even when it is not the controller
     * being used. The vendor's own init does the same. */
    RCC_HB1PeriphClockCmd(RCC_HB1Periph_CAN1, ENABLE);

    /* TX push-pull, RX pulled up so an unconnected pin reads recessive rather
     * than floating into a storm of framing errors. */
    can_init_pin(hw->tx_pin, hw->af, GPIO_Mode_AF_PP);
    can_init_pin(hw->rx_pin, hw->af, GPIO_Mode_IPU);

    CAN_TypeDef *regs = hw->regs;

    /* Master reset first, so a fresh CAN object really is fresh. TEC and REC
     * are hardware counters with no other way to clear them -- only a reset or
     * a bus-off recovery -- so without this they carry across deinit(), across
     * a new CAN() and across a soft reset, and get_counters() reports errors
     * from a program that has already finished.
     *
     * The cost is that CAN1 owns the filter block, so resetting it drops the
     * filters of any other controller that is running. Init re-applies this
     * controller's filters a few lines below; a second controller would need
     * its set_filters() calling again. Two controllers running at once, one of
     * them being re-initialised, is the only case that notices. */
    regs->CTLR |= CAN_CTLR_RESET;
    while (regs->CTLR & CAN_CTLR_RESET) {
    }

    regs->CTLR &= ~CAN_CTLR_SLEEP;
    regs->CTLR |= CAN_CTLR_INRQ;
    if (!can_wait_bit(&regs->STATR, CAN_STATR_INAK, true)) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("CAN did not enter init mode"));
    }

    /* NART clear: retransmit until the frame is acknowledged, which is what
     * CAN is for and what every other MicroPython port does. ABOM set so a
     * controller knocked bus-off by a disconnected transceiver recovers on its
     * own once the bus is idle again, instead of staying dead until restart().
     * TXFP so mailboxes go out in the order send() filled them, which is the
     * order the caller wrote and the only one that keeps the index handed back
     * by send() meaningful. */
    regs->CTLR = (regs->CTLR & ~CAN_CTLR_NART) | CAN_CTLR_ABOM | CAN_CTLR_TXFP;

    uint32_t btimr = ((uint32_t)(self->brp - 1) & 0x3FF)
        | ((uint32_t)(self->tseg1 - 1) << 16)
        | ((uint32_t)(self->tseg2 - 1) << 20)
        | ((uint32_t)(self->sjw - 1) << 24);
    if (self->mode == MP_CAN_MODE_LOOPBACK || self->mode == MP_CAN_MODE_SILENT_LOOPBACK) {
        btimr |= CAN_BTIMR_LBKM;
    }
    if (self->mode == MP_CAN_MODE_SILENT || self->mode == MP_CAN_MODE_SILENT_LOOPBACK) {
        btimr |= CAN_BTIMR_SILM;
    }
    regs->BTIMR = btimr;

    /* Accept everything until set_filter() says otherwise: a freshly
     * constructed CAN object that dropped every frame would be a trap. */
    machine_can_port_clear_filters(self);
    machine_can_port_set_filter_done(self);

    regs->CTLR &= ~CAN_CTLR_INRQ;
    if (!can_wait_bit(&regs->STATR, CAN_STATR_INAK, false)) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("CAN did not leave init mode"));
    }

    can_flush_rx(regs);
    /* Clear any completion flags a previous owner left behind, or the first TX
     * interrupt reports mailboxes that finished before this object existed. */
    regs->TSTATR = CAN_TSTATR_RQCP(0) | CAN_TSTATR_RQCP(1) | CAN_TSTATR_RQCP(2);

    self->port->last_state = can_state_from_hw(regs);

    NVIC_EnableIRQ(hw->irq_tx);
    NVIC_EnableIRQ(hw->irq_rx0);
    NVIC_EnableIRQ(hw->irq_sce);
}

static void machine_can_port_deinit(machine_can_obj_t *self) {
    if (self->port == NULL) {
        return;
    }
    const ch32_can_hw_t *hw = self->port->hw;
    CAN_TypeDef *regs = hw->regs;

    NVIC_DisableIRQ(hw->irq_tx);
    NVIC_DisableIRQ(hw->irq_rx0);
    NVIC_DisableIRQ(hw->irq_sce);
    regs->INTENR = 0;

    for (int i = 0; i < CAN_TX_MAILBOXES; i++) {
        regs->TSTATR = CAN_TSTATR_ABRQ(i);
        self->port->tx[i] = TX_EMPTY;
    }
    can_flush_rx(regs);

    /* Back to init, then release the pins. Leaving them on the alternate
     * function would keep the transceiver driven by a controller nothing owns
     * any more. */
    regs->CTLR |= CAN_CTLR_INRQ;
    can_wait_bit(&regs->STATR, CAN_STATR_INAK, true);
    can_release_pin(hw->tx_pin);
    can_release_pin(hw->rx_pin);

}

/* --- filters ---
 *
 * 32-bit mask mode throughout. bxCAN can also do 16-bit and list mode, which
 * doubles or quadruples the number of filters, but only mask mode expresses
 * the (id, mask) pair the machine.CAN API is written in, and only 32-bit mode
 * covers an extended identifier. */
/* Filter configuration registers.
 *
 * CAN1 and CAN2 share one set and are told apart by the bank number, split at
 * CAN2SB. CAN3 has its own set -- FMCFGR_CAN3 and friends -- indexed from zero
 * again, over the second half of the same 42-entry sFilterRegister array. So
 * "which register" and "which bit" are two separate questions and this answers
 * both; treating CAN3 like the others leaves it with no active filter at all,
 * which does not look like a filter fault: the controller simply receives
 * nothing and loopback appears not to work.
 */
typedef struct _can_filter_regs_t {
    volatile uint32_t *fmcfgr;
    volatile uint32_t *fscfgr;
    volatile uint32_t *fafifor;
    volatile uint32_t *fwr;
    uint32_t bit;               // bit position within those registers
    uint32_t bank;              // index into CAN1->sFilterRegister[]
} can_filter_regs_t;

static can_filter_regs_t can_filter_regs(const machine_can_obj_t *self, int filter_idx) {
    const ch32_can_hw_t *hw = self->port->hw;
    can_filter_regs_t r = {
        .fmcfgr = &CAN1->FMCFGR,
        .fscfgr = &CAN1->FSCFGR,
        .fafifor = &CAN1->FAFIFOR,
        .fwr = &CAN1->FWR,
        .bit = hw->filter_base + filter_idx,
        .bank = hw->filter_base + filter_idx,
    };
    if (hw->regs == CAN3) {
        r.fmcfgr = &CAN1->FMCFGR_CAN3;
        r.fafifor = &CAN1->FAFIFOR_CAN3;
        r.fwr = &CAN1->FWR_CAN3;
        /* Bank minus 27, not minus 28, and FSCFGR stays the *non*-CAN3
         * register while the other three switch. Both are what the vendor's
         * CAN_FilterInit() does, and neither is what the register names
         * suggest; an off-by-one here leaves CAN3 with no active filter and it
         * receives nothing at all, including in loopback. */
        r.bit = r.bank - 27;
    }
    return r;
}

static void machine_can_port_clear_filters(machine_can_obj_t *self) {
    const ch32_can_hw_t *hw = self->port->hw;

    CAN1->FCTLR |= 1u;            // FINIT
    /* CAN2SB, the bank where CAN2's filters start. Fixed at 14 to match the
     * static split in the table above. */
    CAN1->FCTLR = (CAN1->FCTLR & ~(0x3Fu << 8)) | (14u << 8) | 1u;

    for (int i = 0; i < hw->filter_count; i++) {
        can_filter_regs_t r = can_filter_regs(self, i);
        *r.fwr &= ~(1u << r.bit);
    }
    self->port->next_filter = 0;
}

static void machine_can_port_set_filter(machine_can_obj_t *self, int filter_idx,
    mp_uint_t can_id, mp_uint_t mask, mp_uint_t flags) {
    const ch32_can_hw_t *hw = self->port->hw;

    if (filter_idx >= hw->filter_count) {
        mp_raise_ValueError(MP_ERROR_TEXT("too many filters"));
    }
    can_filter_regs_t r = can_filter_regs(self, filter_idx);

    /* The identifier sits in the top of the register exactly as it does in a
     * receive mailbox: extended identifiers from bit 3, standard ones from bit
     * 21. Matching the mailbox layout is the whole reason bxCAN filters are
     * shaped this way. */
    uint32_t id_reg, mask_reg;
    if (flags & CAN_MSG_FLAG_EXT_ID) {
        id_reg = (can_id << 3) | (1u << 2);
        mask_reg = (mask << 3) | (1u << 2);
    } else {
        id_reg = can_id << 21;
        mask_reg = mask << 21;
    }
    if (flags & CAN_MSG_FLAG_RTR) {
        id_reg |= (1u << 1);
    }
    /* IDE always matched, so a standard filter cannot accept an extended frame
     * whose top bits happen to line up. */
    mask_reg |= (1u << 2);

    CAN1->FCTLR |= 1u;                         // FINIT
    *r.fwr &= ~(1u << r.bit);                  // deactivate before writing
    *r.fscfgr |= (1u << r.bit);                // 32-bit
    *r.fmcfgr &= ~(1u << r.bit);               // mask mode
    *r.fafifor &= ~(1u << r.bit);              // into FIFO 0
    CAN1->sFilterRegister[r.bank].FR1 = id_reg;
    CAN1->sFilterRegister[r.bank].FR2 = mask_reg;
    *r.fwr |= (1u << r.bit);

    self->port->next_filter = filter_idx + 1;
}

static void machine_can_port_set_filter_done(machine_can_obj_t *self) {
    if (self->port->next_filter == 0) {
        /* No filters means accept everything, which needs one bank with a zero
         * mask rather than no banks at all -- with none active the hardware
         * accepts nothing. */
        can_filter_regs_t r = can_filter_regs(self, 0);
        CAN1->FCTLR |= 1u;
        *r.fwr &= ~(1u << r.bit);
        *r.fscfgr |= (1u << r.bit);
        *r.fmcfgr &= ~(1u << r.bit);
        *r.fafifor &= ~(1u << r.bit);
        CAN1->sFilterRegister[r.bank].FR1 = 0;
        CAN1->sFilterRegister[r.bank].FR2 = 0;
        *r.fwr |= (1u << r.bit);
    }
    CAN1->FCTLR &= ~1u;                        // leave filter init
}

/* --- transmit --- */

static mp_int_t machine_can_port_send(machine_can_obj_t *self, mp_uint_t id,
    const byte *data, size_t data_len, mp_uint_t flags) {
    CAN_TypeDef *regs = can_regs(self);

    int mb = -1;
    for (int i = 0; i < CAN_TX_MAILBOXES; i++) {
        if (regs->TSTATR & CAN_TSTATR_TME(i)) {
            mb = i;
            break;
        }
    }
    if (mb < 0) {
        return -1;   // extmod turns this into "no free mailbox"
    }

    uint32_t mir;
    if (flags & CAN_MSG_FLAG_EXT_ID) {
        mir = (id << 3) | CAN_TXMIR_IDE;
    } else {
        mir = id << 21;
    }
    if (flags & CAN_MSG_FLAG_RTR) {
        mir |= CAN_TXMIR_RTR;
    }

    regs->sTxMailBox[mb].TXMDTR = data_len & 0x0F;
    /* Byte-wise rather than a cast: data may be any buffer the caller passed
     * and is not guaranteed aligned, and these registers only take 32-bit
     * accesses. */
    uint32_t low = 0, high = 0;
    for (size_t i = 0; i < data_len && i < 4; i++) {
        low |= (uint32_t)data[i] << (8 * i);
    }
    for (size_t i = 4; i < data_len && i < 8; i++) {
        high |= (uint32_t)data[i] << (8 * (i - 4));
    }
    regs->sTxMailBox[mb].TXMDLR = low;
    regs->sTxMailBox[mb].TXMDHR = high;

    self->port->tx[mb] = id;
    regs->sTxMailBox[mb].TXMIR = mir | CAN_TXMIR_TXRQ;
    return mb;
}

static bool machine_can_port_cancel_send(machine_can_obj_t *self, mp_uint_t idx) {
    if (idx >= CAN_TX_MAILBOXES) {
        return false;
    }
    CAN_TypeDef *regs = can_regs(self);
    if (regs->TSTATR & CAN_TSTATR_TME(idx)) {
        return false;   // already gone
    }
    regs->TSTATR = CAN_TSTATR_ABRQ(idx);
    self->port->tx[idx] = TX_EMPTY;
    return true;
}

/* --- receive --- */

static bool machine_can_port_recv(machine_can_obj_t *self, void *data, size_t *dlen,
    mp_uint_t *id, mp_uint_t *flags, mp_uint_t *errors) {
    CAN_TypeDef *regs = can_regs(self);

    *errors = 0;
    if (regs->RFIFO0 & CAN_RFIFO_FOVR) {
        regs->RFIFO0 = CAN_RFIFO_FOVR;
        self->counters.rx_overruns++;
        *errors |= CAN_RECV_ERR_OVERRUN;
    }
    if ((regs->RFIFO0 & CAN_RFIFO_FMP) == 0) {
        return false;
    }
    if (regs->RFIFO0 & CAN_RFIFO_FULL) {
        regs->RFIFO0 = CAN_RFIFO_FULL;
        *errors |= CAN_RECV_ERR_FULL;
    }

    uint32_t mir = regs->sFIFOMailBox[0].RXMIR;
    *flags = 0;
    if (mir & (1u << 2)) {
        *id = mir >> 3;
        *flags |= CAN_MSG_FLAG_EXT_ID;
    } else {
        *id = mir >> 21;
    }
    if (mir & (1u << 1)) {
        *flags |= CAN_MSG_FLAG_RTR;
    }

    size_t len = regs->sFIFOMailBox[0].RXMDTR & 0x0F;
    if (len > 8) {
        len = 8;
    }
    uint32_t low = regs->sFIFOMailBox[0].RXMDLR;
    uint32_t high = regs->sFIFOMailBox[0].RXMDHR;
    uint8_t *out = data;
    for (size_t i = 0; i < len; i++) {
        out[i] = (uint8_t)((i < 4 ? low >> (8 * i) : high >> (8 * (i - 4))) & 0xFF);
    }
    *dlen = len;

    regs->RFIFO0 = CAN_RFIFO_RFOM;   // release the mailbox

    /* Re-arm the receive interrupt the ISR had to mask. See ch32_can_rx_irq():
     * FMPIE0 follows the FIFO level rather than an edge, so it cannot be left
     * enabled while an unread frame sits there. */
    if (self->mp_irq_trigger & MP_CAN_IRQ_RX) {
        regs->INTENR |= CAN_INTENR_FMPIE0 | CAN_INTENR_FMPIE1;
    }
    return true;
}

/* --- state and counters --- */

static machine_can_state_t machine_can_port_get_state(machine_can_obj_t *self) {
    if (self->port == NULL) {
        return MP_CAN_STATE_STOPPED;
    }
    return can_state_from_hw(can_regs(self));
}

static void machine_can_port_restart(machine_can_obj_t *self) {
    CAN_TypeDef *regs = can_regs(self);
    /* Bus-off recovery is the hardware's, via ABOM: it needs 128 occurrences
     * of 11 recessive bits and cannot be short-circuited. What this does is
     * bounce through init so a controller left in sleep or with a stuck
     * mailbox starts over. */
    regs->CTLR |= CAN_CTLR_INRQ;
    can_wait_bit(&regs->STATR, CAN_STATR_INAK, true);
    for (int i = 0; i < CAN_TX_MAILBOXES; i++) {
        regs->TSTATR = CAN_TSTATR_ABRQ(i);
        self->port->tx[i] = TX_EMPTY;
    }
    regs->CTLR &= ~CAN_CTLR_INRQ;
    can_wait_bit(&regs->STATR, CAN_STATR_INAK, false);
}

static void machine_can_port_update_counters(machine_can_obj_t *self) {
    CAN_TypeDef *regs = can_regs(self);
    uint32_t esr = regs->ERRSR;
    self->counters.tec = (esr >> 16) & 0xFF;
    self->counters.rec = (esr >> 24) & 0xFF;

    mp_uint_t pending = 0;
    for (int i = 0; i < CAN_TX_MAILBOXES; i++) {
        if (!(regs->TSTATR & CAN_TSTATR_TME(i))) {
            pending++;
        }
    }
    self->counters.tx_pending = pending;
    self->counters.rx_pending = regs->RFIFO0 & CAN_RFIFO_FMP;
}

/* --- interrupts --- */

static void machine_can_update_irqs(machine_can_obj_t *self) {
    CAN_TypeDef *regs = can_regs(self);
    uint32_t enr = 0;
    if (self->mp_irq_trigger & MP_CAN_IRQ_RX) {
        enr |= CAN_INTENR_FMPIE0 | CAN_INTENR_FMPIE1;
    }
    if (self->mp_irq_trigger & (MP_CAN_IRQ_TX | MP_CAN_IRQ_TX_FAILED)) {
        enr |= CAN_INTENR_TMEIE;
    }
    if (self->mp_irq_trigger & MP_CAN_IRQ_STATE) {
        enr |= CAN_INTENR_ERRIE | CAN_INTENR_EWGIE | CAN_INTENR_EPVIE | CAN_INTENR_BOFFIE;
    }
    regs->INTENR = enr;
}

static mp_uint_t machine_can_port_irq_flags(machine_can_obj_t *self) {
    /* Read and clear: these are the flags for the callback that is running
     * now, and leaving them set would repeat them on the next one. */
    uint32_t state = mp_hal_atomic_enter();
    mp_uint_t flags = self->port->irq_flags;
    self->port->irq_flags = 0;
    mp_hal_atomic_exit(state);
    return flags;
}

/* One handler body for all three controllers. Sets flags and dispatches; the
 * work of reading frames belongs to the Python callback, which runs from the
 * scheduler rather than from here. */
static void ch32_can_irq(mp_uint_t idx, uint16_t flags) {
    machine_can_obj_t *self = ch32_can_obj(idx);
    if (self == NULL || self->port == NULL) {
        return;
    }
    self->port->irq_flags |= flags;
    if (self->mp_irq_obj != NULL && (flags & self->mp_irq_trigger)) {
        mp_irq_handler(self->mp_irq_obj);
    }
}

/* The receive interrupt is a *level*: FMPIE0 asserts for as long as the FIFO
 * has a message in it, and this handler does not read the message -- the
 * Python callback does, later, from the scheduler. Leaving it enabled re-enters
 * the handler the instant it returns and the core never leaves interrupt
 * context again. So mask it here and let machine_can_port_recv() put it back
 * once the frame has been taken.
 *
 * Worth knowing what that looks like, because it is not a CAN symptom at all:
 * the first thing to fail is TinyUSB, whose event FIFO fills because
 * tud_task() is never reached, and its TU_ASSERT executes an ebreak that lands
 * on the SDK's weak Break_Point_Handler -- an unimplemented `j .`. The board
 * goes silent with mcause=3 and a PC that resolves to a hundred different weak
 * handler names, none of them CAN. */
static void ch32_can_rx_irq(mp_uint_t idx) {
    machine_can_obj_t *self = ch32_can_obj(idx);
    if (self == NULL || self->port == NULL) {
        return;
    }
    CAN_TypeDef *regs = self->port->hw->regs;
    regs->INTENR &= ~(CAN_INTENR_FMPIE0 | CAN_INTENR_FMPIE1);
    /* Read back, so the mask has reached the peripheral before mret. It is
     * clocked at a quarter of the core and there is nothing else between the
     * write and the return to make the ordering certain. */
    (void)regs->INTENR;
    ch32_can_irq(idx, MP_CAN_IRQ_RX);
}

static void ch32_can_tx_irq(mp_uint_t idx) {
    machine_can_obj_t *self = ch32_can_obj(idx);
    if (self == NULL || self->port == NULL) {
        return;
    }
    CAN_TypeDef *regs = self->port->hw->regs;
    uint16_t flags = 0;
    for (int i = 0; i < CAN_TX_MAILBOXES; i++) {
        if (regs->TSTATR & CAN_TSTATR_RQCP(i)) {
            bool ok = regs->TSTATR & CAN_TSTATR_TXOK(i);
            /* Writing RQCP back clears RQCP, TXOK, ALST and TERR together. */
            regs->TSTATR = CAN_TSTATR_RQCP(i);
            (void)regs->TSTATR;     // see ch32_can_rx_irq()
            self->port->tx[i] = TX_EMPTY;
            /* The mailbox index rides along in the flags word, which is how
             * the callback tells which send() completed. */
            flags |= (ok ? MP_CAN_IRQ_TX : MP_CAN_IRQ_TX_FAILED)
                | ((i & MP_CAN_IRQ_IDX_MASK) << MP_CAN_IRQ_IDX_SHIFT);
        }
    }
    if (flags) {
        ch32_can_irq(idx, flags);
    }
}

static void ch32_can_sce_irq(mp_uint_t idx) {
    machine_can_obj_t *self = ch32_can_obj(idx);
    if (self == NULL || self->port == NULL) {
        return;
    }
    CAN_TypeDef *regs = self->port->hw->regs;
    regs->STATR = (1u << 2);   // ERRI, write-1-to-clear
    (void)regs->STATR;         // see ch32_can_rx_irq()

    machine_can_state_t now = can_state_from_hw(regs);
    if (now != self->port->last_state) {
        switch (now) {
            case MP_CAN_STATE_WARNING:
                self->counters.num_warning++;
                break;
            case MP_CAN_STATE_PASSIVE:
                self->counters.num_passive++;
                break;
            case MP_CAN_STATE_BUS_OFF:
                self->counters.num_bus_off++;
                break;
            default:
                break;
        }
        self->port->last_state = now;
        ch32_can_irq(idx, MP_CAN_IRQ_STATE);
    }
}

#define CH32_CAN_HANDLERS(n, idx)                        \
    void CH32_IRQ_HANDLER(CAN##n##_TX_IRQHandler);       \
    void CAN##n##_TX_IRQHandler(void) {                  \
        ch32_can_tx_irq(idx);                            \
    }                                                    \
    void CH32_IRQ_HANDLER(CAN##n##_RX0_IRQHandler);      \
    void CAN##n##_RX0_IRQHandler(void) {                 \
        ch32_can_rx_irq(idx);                            \
    }                                                    \
    void CH32_IRQ_HANDLER(CAN##n##_SCE_IRQHandler);      \
    void CAN##n##_SCE_IRQHandler(void) {                 \
        ch32_can_sce_irq(idx);                           \
    }

CH32_CAN_HANDLERS(1, 0)
CH32_CAN_HANDLERS(2, 1)
CH32_CAN_HANDLERS(3, 2)

/* No additional (CAN-FD) timings to report: this is a classic CAN controller
 * and there is no second bit rate. */
static mp_obj_t machine_can_port_get_additional_timings(machine_can_obj_t *self, mp_obj_t optional_arg) {
    (void)self;
    (void)optional_arg;
    return mp_const_none;
}
