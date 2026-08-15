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
#include "irq.h"

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
    /* Meaningful only while this object is the one machine_i2c_async points
     * at: what the interrupt still has to do when the data phase ends. */
    bool xfer_read;
    bool xfer_stop;
} machine_i2c_obj_t;

/* Pin options taken from the datasheet's pin table. Only the pairs where both
 * pins belong to the same peripheral are listed; the first entry for a bus is
 * its default.
 *
 * A note for anyone adding to this: on this part PB6/PB7 are the only I2C pair
 * that sits in the VDDIO (3.3 V) domain. Everything else here is on VIO18,
 * which comes up well below 3.3 V, so those pins need level shifting for an
 * ordinary 3.3 V sensor. (The datasheet only distinguishes the domains by
 * colour in the package figure, so the exact level has to be measured; see
 * docs/hw/ch32h417-notes.md, where it is still an open question.) */
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

/* DMA1 channel 6 carries the data phase. Channel 1 is AudioOut, 2 and 3 are
 * SPI, 4 and 5 are I2S; 7 and 8 stay free. One channel is enough where SPI
 * needs two, because I2C is half duplex -- only the request routing changes
 * between a read and a write. */
#define I2C_DMA_CHANNEL       (DMA1_Channel6)
#define I2C_DMA_MUX_CHANNEL   (DMA_MuxChannel6)
#define I2C_DMA_IRQN          (DMA1_Channel6_IRQn)
#define I2C_DMA_FLAGS         ((uint32_t)0x00F00000)   /* channel 6 nibble */

/* DMAMUX request numbers, reference manual table 10-2: I2C1_TX is 73 and
 * I2C1_RX 74, with each further bus two higher. */
#define I2C_DMA_TX_REQ(id)    (71 + 2 * (id))
#define I2C_DMA_RX_REQ(id)    (72 + 2 * (id))

/* CNTR is 16 bits. A longer buffer stays on the polled path rather than being
 * chunked: the receive tail rules below make splitting a transfer far more
 * delicate than it is worth for a length no I2C device asks for. */
#define I2C_DMA_MAX_LEN       (65535)

static machine_i2c_obj_t machine_i2c_obj[4];

/* The transfer in flight, or NULL. */
static machine_i2c_obj_t *machine_i2c_async;

/* The irq() handler, in a root pointer rather than in the object: these
 * objects are static, so the collector never traces them and a callback kept
 * there could be collected while still in use. */
#define machine_i2c_handler(self) (MP_STATE_PORT(machine_i2c_irq_handler)[(self)->id - 1])

/* Event interrupts are not evenly spaced, so they are listed rather than
 * calculated. */
static const IRQn_Type machine_i2c_ev_irqn[4] = {
    I2C1_EV_IRQn, I2C2_EV_IRQn, I2C3_EV_IRQn, I2C4_EV_IRQn
};

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

/* --- interrupt-driven transfers ---
 *
 * machine.I2S sets the pattern and this follows it: irq() takes a callback,
 * and with one set the ordinary readfrom()/writeto()/readfrom_mem() calls
 * start the transfer and return, with the callback run when it ends.
 *
 * Only the data phase moves to the DMA. Addressing stays exactly as it was --
 * it costs tens of microseconds against milliseconds of data, and keeping it
 * synchronous means an address NAK still raises where the caller can catch it,
 * which is what scan() and every "is my sensor plugged in" check rely on. What
 * is not reported in this mode is a NAK partway through a write: the transfer
 * ends and the callback still runs.
 *
 * The buffers must stay referenced until the callback arrives, because the DMA
 * is reading or writing them the whole time and nothing here can keep them
 * alive.
 */
static void machine_i2c_finish(machine_i2c_obj_t *self) {
    machine_i2c_async = NULL;
    mp_obj_t handler = machine_i2c_handler(self);
    if (handler != MP_OBJ_NULL) {
        mp_sched_schedule(handler, MP_OBJ_FROM_PTR(self));
    }
}

static void machine_i2c_start_dma(machine_i2c_obj_t *self, size_t len,
    uint8_t *buf, bool read, bool stop) {
    I2C_TypeDef *i2c = self->i2c;

    self->xfer_read = read;
    self->xfer_stop = stop;
    machine_i2c_async = self;

    RCC_HBPeriphClockCmd(RCC_HBPeriph_DMA1, ENABLE);
    DMA_MuxChannelConfig(I2C_DMA_MUX_CHANNEL,
        read ? I2C_DMA_RX_REQ(self->id) : I2C_DMA_TX_REQ(self->id));

    DMA_Cmd(I2C_DMA_CHANNEL, DISABLE);
    DMA1->INTFCR = I2C_DMA_FLAGS;

    DMA_InitTypeDef init = {0};
    init.DMA_PeripheralBaseAddr = (uint32_t)&i2c->DATAR;
    init.DMA_Memory0BaseAddr = (uint32_t)buf;
    init.DMA_DIR = read ? DMA_DIR_PeripheralSRC : DMA_DIR_PeripheralDST;
    init.DMA_BufferSize = (uint16_t)len;
    init.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    init.DMA_MemoryInc = DMA_MemoryInc_Enable;
    init.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
    init.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
    init.DMA_Mode = DMA_Mode_Normal;
    init.DMA_Priority = DMA_Priority_High;
    init.DMA_M2M = DMA_M2M_Disable;
    DMA_Init(I2C_DMA_CHANNEL, &init);

    NVIC_ClearPendingIRQ(I2C_DMA_IRQN);
    NVIC_EnableIRQ(I2C_DMA_IRQN);
    NVIC_EnableIRQ(machine_i2c_ev_irqn[self->id - 1]);
    DMA_ITConfig(I2C_DMA_CHANNEL, DMA_IT_TC, ENABLE);
    DMA_Cmd(I2C_DMA_CHANNEL, ENABLE);

    I2C_DMACmd(i2c, ENABLE);
    if (read) {
        /* Makes the controller NACK the byte the DMA counts as its last. This
         * is why a DMA read is simpler than the polled one rather than harder:
         * no POS juggling, and no reading the tail out by hand. */
        I2C_DMALastTransferCmd(i2c, ENABLE);
    }

    /* Clearing ADDR releases the bus and starts bytes moving, so it goes last:
     * with the DMA already armed there is no window where a byte can arrive
     * with nothing waiting to collect it. */
    i2c_clear_addr(self);
}

/* Abandon a transfer in flight, from the soft-reset path. The DMA is reading
 * or writing a Python buffer whose heap is about to be rebuilt, and the root
 * pointer would otherwise dangle into it -- the same pair of problems
 * machine_i2s_deinit_all() documents. */
void machine_i2c_deinit_all(void) {
    machine_i2c_obj_t *self = machine_i2c_async;
    if (self != NULL) {
        DMA_ITConfig(I2C_DMA_CHANNEL, DMA_IT_TC, DISABLE);
        DMA_Cmd(I2C_DMA_CHANNEL, DISABLE);
        I2C_DMACmd(self->i2c, DISABLE);
        I2C_ITConfig(self->i2c, I2C_IT_EVT, DISABLE);
        i2c_stop(self);
        machine_i2c_async = NULL;
    }
    for (size_t i = 0; i < MP_ARRAY_SIZE(machine_i2c_obj); i++) {
        MP_STATE_PORT(machine_i2c_irq_handler)[i] = MP_OBJ_NULL;
    }
}

void CH32_IRQ_HANDLER(DMA1_Channel6_IRQHandler);
void DMA1_Channel6_IRQHandler(void) {
    DMA1->INTFCR = I2C_DMA_FLAGS;
    machine_i2c_obj_t *self = machine_i2c_async;
    if (self == NULL) {
        return;
    }
    I2C_TypeDef *i2c = self->i2c;

    DMA_ITConfig(I2C_DMA_CHANNEL, DMA_IT_TC, DISABLE);
    DMA_Cmd(I2C_DMA_CHANNEL, DISABLE);
    I2C_DMACmd(i2c, DISABLE);

    if (self->xfer_read) {
        /* The last byte was NACKed by the controller, so there is nothing left
         * on the bus for a STOP to truncate. */
        if (self->xfer_stop) {
            i2c_stop(self);
        }
        machine_i2c_finish(self);
        return;
    }

    /* A write is not over when the DMA is: the last byte has only reached
     * DATAR. Stopping now would cut it off, and waiting here would hold the
     * core for a byte time -- 90 us at 100 kHz, in interrupt context. Hand the
     * tail to the event interrupt, which fires on BTF once it has gone. */
    I2C_ITConfig(i2c, I2C_IT_EVT, ENABLE);
}

static void machine_i2c_ev_handler(void) {
    machine_i2c_obj_t *self = machine_i2c_async;
    if (self == NULL) {
        return;
    }
    I2C_TypeDef *i2c = self->i2c;
    if (!(i2c->STAR1 & I2C_STAR1_BTF)) {
        return;
    }
    I2C_ITConfig(i2c, I2C_IT_EVT, DISABLE);
    if (self->xfer_stop) {
        i2c_stop(self);
    }
    machine_i2c_finish(self);
}

void CH32_IRQ_HANDLER(I2C1_EV_IRQHandler);
void I2C1_EV_IRQHandler(void) {
    machine_i2c_ev_handler();
}

void CH32_IRQ_HANDLER(I2C2_EV_IRQHandler);
void I2C2_EV_IRQHandler(void) {
    machine_i2c_ev_handler();
}

void CH32_IRQ_HANDLER(I2C3_EV_IRQHandler);
void I2C3_EV_IRQHandler(void) {
    machine_i2c_ev_handler();
}

void CH32_IRQ_HANDLER(I2C4_EV_IRQHandler);
void I2C4_EV_IRQHandler(void) {
    machine_i2c_ev_handler();
}

static int machine_i2c_transfer_single(mp_obj_base_t *self_in, uint16_t addr,
    size_t len, uint8_t *buf, unsigned int flags) {
    machine_i2c_obj_t *self = (machine_i2c_obj_t *)self_in;
    I2C_TypeDef *i2c = self->i2c;
    bool read = flags & MP_MACHINE_I2C_FLAG_READ;
    bool stop = flags & MP_MACHINE_I2C_FLAG_STOP;
    int ret;

    /* Only a data phase with something in it is worth handing to the DMA. A
     * zero-length transfer is scan() probing for an ACK, and anything past
     * CNTR's range would have to be chunked, so both take the ordinary path. */
    bool async = machine_i2c_handler(self) != MP_OBJ_NULL
        && len > 0 && len <= I2C_DMA_MAX_LEN;
    if (async && machine_i2c_async != NULL) {
        /* Tested before the bus is touched, so a refused transfer leaves the
         * one already running undisturbed. */
        mp_raise_OSError(MP_EBUSY);
    }

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

    if (async) {
        machine_i2c_start_dma(self, len, buf, read, stop);
        /* The same counts the polled paths return, promised rather than
         * observed: what the bus does from here is the interrupt's business. */
        return read ? 0 : (int)len;
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

    /* The SDK's I2C_Init() derives the divider from RCC's HCLK_Frequency,
     * which is the clock this peripheral actually runs on, so just use it.
     *
     * An earlier version of this file programmed CTLR2/CKCFGR/RTR by hand from
     * SystemCoreClock, because the bus measured exactly 4.00x the requested
     * rate. That reasoning was wrong. The measurement timed transfers with
     * mp_hal_ticks_us(), and SysTick was itself configured from
     * SystemCoreClock when it counts at HCLK, so the whole timebase read 4x
     * short. "The peripheral runs at 4x HCLK" and "the clock measuring it runs
     * 4x slow" fit that data equally well; timing a device-side sleep against
     * a host stopwatch separated them, and it was the timebase. The moral:
     * do not measure a clock with a clock derived from the same suspect
     * source. */
    I2C_InitTypeDef init = {0};
    init.I2C_ClockSpeed = freq;
    init.I2C_Mode = I2C_Mode_I2C;
    init.I2C_DutyCycle = I2C_DutyCycle_2;
    init.I2C_OwnAddress1 = 0;
    init.I2C_Ack = I2C_Ack_Enable;
    init.I2C_AcknowledgedAddress = I2C_AcknowledgedAddress_7bit;

    I2C_Cmd(i2c, DISABLE);
    I2C_Init(i2c, &init);
    I2C_Cmd(i2c, ENABLE);
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

/* Set a callback and transfers become non-blocking; pass None and they go
 * back to blocking. Same call, same meaning, as machine.I2S.irq() and
 * machine.SPI.irq() on this port.
 *
 * The handler is called with the I2C object, from the scheduler rather than
 * from the interrupt, so it may allocate and raise like any other Python
 * code. */
static mp_obj_t machine_i2c_irq(mp_obj_t self_in, mp_obj_t handler) {
    machine_i2c_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (handler != mp_const_none && !mp_obj_is_callable(handler)) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid callback"));
    }
    machine_i2c_handler(self) = (handler == mp_const_none) ? MP_OBJ_NULL : handler;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(machine_i2c_irq_obj, machine_i2c_irq);

/* Add irq() without restating the dozen methods extmod already provides.
 *
 * Unlike SPI, none of the generic I2C method objects are declared in
 * modmachine.h, so a port cannot assemble its own dictionary out of them. What
 * it can do is answer for the one name it adds and hand everything else back:
 * setting dest[1] to MP_OBJ_SENTINEL is how mp_load_method_maybe() is told to
 * carry on into locals_dict, which is still extmod's own. */
static void machine_i2c_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    if (dest[0] == MP_OBJ_NULL && attr == MP_QSTR_irq) {
        dest[0] = MP_OBJ_FROM_PTR(&machine_i2c_irq_obj);
        dest[1] = self_in;
        return;
    }
    dest[1] = MP_OBJ_SENTINEL;
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
    attr, machine_i2c_attr,
    protocol, &machine_i2c_p,
    locals_dict, &mp_machine_i2c_locals_dict
    );

MP_REGISTER_ROOT_POINTER(mp_obj_t machine_i2c_irq_handler[4]);
